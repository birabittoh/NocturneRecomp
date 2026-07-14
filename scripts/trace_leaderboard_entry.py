#!/usr/bin/env python3
"""
Continuously samples Main XThread's RIP + a host-stack scan, resolving each
host address to its guest function via the live PPCFuncMappings table (see
resolve_gpu_wait.py for the technique), and prints any guest function seen
for the first time. Known "always running" engine/render functions are
filtered out so new output should be whatever code newly starts running
when a UI action happens (e.g. selecting a menu entry).

Usage: python scripts/trace_leaderboard_entry.py [pid] [duration_seconds]
Start this BEFORE performing the action you want to trace, since it prints
new functions live as they're seen.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

k32 = ctypes.windll.kernel32

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
TH32CS_SNAPTHREAD = 0x00000004
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
CONTEXT_FULL = 0x00100000 | 0x00000001 | 0x00000002 | 0x00000004
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
THREAD_SUSPEND_RESUME = 0x0002

# Functions already confirmed to be part of the always-running per-frame
# engine/render loop -- not interesting for finding a new UI action's entry
# point. See LEADERBOARD_HANG_INVESTIGATION.md.
KNOWN_BORING = {
    0x8252F840, 0x825252C8, 0x82525630, 0x82525120, 0x82524330,
    0x825249B8, 0x82534378, 0x82534910, 0x825134E0, 0x825D1618,
    0x82578A30, 0x825D9340, 0x825D9510,
}


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
        ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
        ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD),
    ]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
        ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
        ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)), ("modBaseSize", wt.DWORD),
        ("hModule", wt.HMODULE), ("szModule", ctypes.c_char * 256),
        ("szExePath", ctypes.c_char * 260),
    ]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_ulonglong), ("High", ctypes.c_longlong)]


class CONTEXT64(ctypes.Structure):
    _fields_ = [
        ("P1Home", ctypes.c_ulonglong), ("P2Home", ctypes.c_ulonglong),
        ("P3Home", ctypes.c_ulonglong), ("P4Home", ctypes.c_ulonglong),
        ("P5Home", ctypes.c_ulonglong), ("P6Home", ctypes.c_ulonglong),
        ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
        ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD),
        ("SegGs", wt.WORD), ("SegSs", wt.WORD), ("EFlags", wt.DWORD),
        ("Dr0", ctypes.c_ulonglong), ("Dr1", ctypes.c_ulonglong), ("Dr2", ctypes.c_ulonglong),
        ("Dr3", ctypes.c_ulonglong), ("Dr6", ctypes.c_ulonglong), ("Dr7", ctypes.c_ulonglong),
        ("Rax", ctypes.c_ulonglong), ("Rcx", ctypes.c_ulonglong), ("Rdx", ctypes.c_ulonglong),
        ("Rbx", ctypes.c_ulonglong), ("Rsp", ctypes.c_ulonglong), ("Rbp", ctypes.c_ulonglong),
        ("Rsi", ctypes.c_ulonglong), ("Rdi", ctypes.c_ulonglong),
        ("R8", ctypes.c_ulonglong), ("R9", ctypes.c_ulonglong), ("R10", ctypes.c_ulonglong),
        ("R11", ctypes.c_ulonglong), ("R12", ctypes.c_ulonglong), ("R13", ctypes.c_ulonglong),
        ("R14", ctypes.c_ulonglong), ("R15", ctypes.c_ulonglong),
        ("Rip", ctypes.c_ulonglong),
        ("FltSave", ctypes.c_byte * 512),
        ("VectorRegister", M128A * 26), ("VectorControl", ctypes.c_ulonglong),
        ("DebugControl", ctypes.c_ulonglong), ("LastBranchToRip", ctypes.c_ulonglong),
        ("LastBranchFromRip", ctypes.c_ulonglong), ("LastExceptionToRip", ctypes.c_ulonglong),
        ("LastExceptionFromRip", ctypes.c_ulonglong),
    ]


def find_pid(name="nocturnerecomp.exe"):
    import subprocess
    out = subprocess.check_output(["tasklist"], text=True, errors="ignore")
    for line in out.splitlines():
        if name.lower() in line.lower():
            parts = line.split()
            return int(parts[1])
    return None


def find_module(pid, name="nocturnerecomp.exe"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32()
    me.dwSize = ctypes.sizeof(MODULEENTRY32)
    found = None
    if k32.Module32First(snap, ctypes.byref(me)):
        while True:
            if me.szModule.decode(errors="ignore").lower() == name.lower():
                found = (ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value, me.modBaseSize)
                break
            if not k32.Module32Next(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    return found


def find_threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(THREADENTRY32)
    out = []
    if k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                out.append(te.th32ThreadID)
            if not k32.Thread32Next(snap, ctypes.byref(te)):
                break
    k32.CloseHandle(snap)
    return out


def get_thread_description(tid):
    h = k32.OpenThread(THREAD_QUERY_INFORMATION, False, tid)
    if not h:
        return None
    try:
        buf = wt.LPWSTR()
        hr = ctypes.windll.kernel32.GetThreadDescription(h, ctypes.byref(buf))
        if hr < 0 or not buf.value:
            return None
        desc = buf.value
        k32.LocalFree(buf)
        return desc
    finally:
        k32.CloseHandle(h)


def read_mem(hproc, addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size, ctypes.byref(n))
    if not ok or n.value != size:
        return None
    return buf.raw


def find_func_table(hproc, base, size):
    CHUNK = 4 * 1024 * 1024
    off = 0
    marker = struct.pack("<Q", 0x82230000)
    while off < size:
        n = min(CHUNK, size - off)
        data = read_mem(hproc, base + off, n)
        if data:
            idx = 0
            while True:
                idx = data.find(marker, idx)
                if idx == -1:
                    break
                if idx + 16 <= len(data):
                    host_ptr = struct.unpack_from("<Q", data, idx + 8)[0]
                    if base <= host_ptr < base + size:
                        return base + off + idx
                idx += 1
        off += n - 16
    return None


def read_func_table(hproc, table_addr, base, size):
    entries = []
    addr = table_addr
    while True:
        data = read_mem(hproc, addr, 16)
        if not data:
            break
        guest, host = struct.unpack("<QQ", data)
        if not (0x82000000 <= guest < 0x83000000):
            break
        if not (base <= host < base + size):
            break
        entries.append((guest, host))
        addr += 16
    return entries


def resolve(entries_by_host, addr):
    import bisect
    hosts = [h for h, g in entries_by_host]
    i = bisect.bisect_right(hosts, addr) - 1
    if i < 0:
        return None
    host, guest = entries_by_host[i]
    return guest, addr - host


def get_thread_context_nosuspend(h):
    ctx = CONTEXT64()
    ctx.ContextFlags = CONTEXT_FULL
    if not k32.GetThreadContext(h, ctypes.byref(ctx)):
        return None
    return ctx


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else find_pid()
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    if not pid:
        print("nocturnerecomp.exe not found")
        return 1
    print(f"[*] Target PID: {pid}, tracing for {duration}s -- perform the action now")

    hproc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    base, size = find_module(pid)
    table_addr = find_func_table(hproc, base, size)
    entries = read_func_table(hproc, table_addr, base, size)
    entries_by_host = sorted((h, g) for g, h in entries)
    print(f"[*] Resolved {len(entries)} guest functions")

    main_tid = None
    for tid in find_threads(pid):
        desc = get_thread_description(tid)
        if desc and "Main XThread" in desc:
            main_tid = tid
            break
    if not main_tid:
        print("Could not find Main XThread")
        return 1

    hthread = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, False, main_tid)

    seen = set()
    start = time.time()
    sample_n = 0
    while time.time() - start < duration:
        k32.SuspendThread(hthread)
        ctx = get_thread_context_nosuspend(hthread)
        k32.ResumeThread(hthread)
        sample_n += 1
        if not ctx:
            continue

        found_this_sample = set()
        g = resolve(entries_by_host, ctx.Rip)
        if g:
            found_this_sample.add(g[0])

        stack = read_mem(hproc, ctx.Rsp, 2048 * 8)
        if stack:
            for i in range(2048):
                val = struct.unpack_from("<Q", stack, i * 8)[0]
                if base <= val < base + size:
                    g2 = resolve(entries_by_host, val)
                    if g2 and g2[1] < 0x2000:
                        found_this_sample.add(g2[0])

        new_funcs = found_this_sample - seen - KNOWN_BORING
        if new_funcs:
            elapsed = time.time() - start
            for f in sorted(new_funcs):
                print(f"[+{elapsed:6.2f}s] NEW guest function seen: sub_{f:08X}")
        seen |= found_this_sample

        time.sleep(0.03)

    print(f"[*] Done ({sample_n} samples). All non-boring functions seen this run:")
    for f in sorted(seen - KNOWN_BORING):
        print(f"    sub_{f:08X}")

    k32.CloseHandle(hthread)
    k32.CloseHandle(hproc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
