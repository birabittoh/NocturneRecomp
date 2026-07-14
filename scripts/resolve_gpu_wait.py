#!/usr/bin/env python3
"""
Live-attach to a running nocturnerecomp.exe, locate the guest->host
PPCFuncMappings table in process memory (no PDB needed), and use it to:

  1. Resolve the Main XThread's current PC to an exact guest function.
  2. Scan the host stack for return addresses and resolve each one too,
     reconstructing the real guest call chain (host frame-pointer chains
     aren't reliable in this build, so we brute-force scan qwords on the
     stack for anything that lands inside a mapped guest function).

Usage: python scripts/resolve_gpu_wait.py [pid]
If pid is omitted, finds the first nocturnerecomp.exe process.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import struct

k32 = ctypes.windll.kernel32

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
PROCESS_ALL_ACCESS = 0x1F0FFF
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPTHREAD = 0x00000004
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010

CONTEXT_FULL = 0x00100000 | 0x00000001 | 0x00000002 | 0x00000004
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
THREAD_SUSPEND_RESUME = 0x0002


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)), ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_char * 260),
    ]


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


def find_pid(name="nocturnerecomp.exe"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32()
    pe.dwSize = ctypes.sizeof(PROCESSENTRY32)
    found = None
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.decode(errors="ignore").lower() == name.lower():
                found = pe.th32ProcessID
                break
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return found


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


def read_mem(hproc, addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size, ctypes.byref(n))
    if not ok or n.value != size:
        return None
    return buf.raw


def get_thread_description(tid):
    # GetThreadDescription needs a handle; use minimal rights
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


def get_thread_context(tid):
    h = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, False, tid)
    if not h:
        raise RuntimeError(f"OpenThread failed for tid {tid}: {ctypes.GetLastError()}")
    try:
        k32.SuspendThread(h)
        ctx = CONTEXT64()
        ctx.ContextFlags = CONTEXT_FULL
        if not k32.GetThreadContext(h, ctypes.byref(ctx)):
            raise RuntimeError(f"GetThreadContext failed: {ctypes.GetLastError()}")
        return ctx
    finally:
        k32.ResumeThread(h)
        k32.CloseHandle(h)


def find_func_table(hproc, base, size):
    """Scan the module for the PPCFuncMappings table: repeated 16-byte
    entries of (u64 guest_addr in 0x82000000-0x83000000, u64 host_ptr in [base,base+size))."""
    CHUNK = 4 * 1024 * 1024
    off = 0
    marker = struct.pack("<Q", 0x82230000)  # first guest function address, from nocturnerecomp_init.cpp
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
                        return base + off + idx  # address of table start
                idx += 1
        off += n - 16  # overlap for entries spanning chunk boundary
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
    """Binary search: largest host <= addr."""
    import bisect
    hosts = [h for h, g in entries_by_host]
    i = bisect.bisect_right(hosts, addr) - 1
    if i < 0:
        return None
    host, guest = entries_by_host[i]
    return guest, addr - host


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else find_pid()
    if not pid:
        print("nocturnerecomp.exe not found")
        return 1
    print(f"[*] Target PID: {pid}")

    hproc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not hproc:
        print(f"OpenProcess failed: {ctypes.GetLastError()}")
        return 1

    base, size = find_module(pid)
    print(f"[*] Module base=0x{base:x} size=0x{size:x}")

    print("[*] Scanning for PPCFuncMappings table...")
    table_addr = find_func_table(hproc, base, size)
    if not table_addr:
        print("Could not find PPCFuncMappings table")
        return 1
    print(f"[*] Found table at 0x{table_addr:x}")

    entries = read_func_table(hproc, table_addr, base, size)
    print(f"[*] Read {len(entries)} guest->host mappings")
    entries_by_host = sorted((h, g) for g, h in entries)

    # find Main XThread
    main_tid = None
    for tid in find_threads(pid):
        desc = get_thread_description(tid)
        if desc and "Main XThread" in desc:
            main_tid = tid
            break
    if not main_tid:
        print("Could not find Main XThread")
        return 1
    print(f"[*] Main XThread tid=0x{main_tid:x} ({main_tid})")

    ctx = get_thread_context(main_tid)
    rip = ctx.Rip
    g = resolve(entries_by_host, rip)
    print(f"\n[*] RIP = 0x{rip:x}  -> guest sub_{g[0]:08X} + 0x{g[1]:x}" if g else f"RIP 0x{rip:x} not resolved")

    # PPCContext is normally kept in a callee-saved register across a guest
    # function body; try common candidates.
    for name, val in [("rdi", ctx.Rdi), ("rsi", ctx.Rsi), ("rbx", ctx.Rbx), ("rbp", ctx.Rbp)]:
        lr_bytes = read_mem(hproc, val + 0x100, 8)
        if lr_bytes:
            lr = struct.unpack("<Q", lr_bytes)[0]
            if 0x82000000 <= lr < 0x83000000:
                print(f"[*] ctx candidate in {name}=0x{val:x}  ctx.lr = 0x{lr:x}")

    # Stack scan: walk qwords above RSP looking for values that resolve to
    # a guest function -- reconstructs the guest call chain even though the
    # host frame-pointer chain isn't walkable.
    print("\n[*] Stack scan (host RSP upward, resolving return-address-shaped values):")
    STACK_WORDS = 4096
    stack = read_mem(hproc, ctx.Rsp, STACK_WORDS * 8)
    if stack:
        seen = []
        for i in range(STACK_WORDS):
            val = struct.unpack_from("<Q", stack, i * 8)[0]
            if base <= val < base + size:
                g = resolve(entries_by_host, val)
                if g and g[1] < 0x2000:  # plausible "just returned here" offset
                    seen.append((i * 8, val, g))
        last_guest = None
        for stack_off, hostaddr, (guest, delta) in seen:
            if guest != last_guest:
                print(f"    [rsp+0x{stack_off:04x}] host=0x{hostaddr:x} -> guest sub_{guest:08X} + 0x{delta:x}")
                last_guest = guest

    k32.CloseHandle(hproc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
