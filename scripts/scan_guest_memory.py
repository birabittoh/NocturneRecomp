#!/usr/bin/env python3
"""Reverse-engineering helper: scan or read a running NocturneRecomp process's
guest memory without attaching a debugger.

Background/technique this supports is documented in the project's
"live-memory-reverse-engineering-technique" memory: break on an SDK function
that takes a guest address directly (e.g. rex::system::XFile::Write) with
lldb to get one guest_address/value data point, then use this script to scan
the whole process for the exact byte pattern of a *distinctive* live value
(not a common one -- common small values produce hundreds of false hits) to
find the address for good, and to re-read it over time to confirm it's live
and stable.

Only works on Windows (uses OpenProcess/VirtualQueryEx/ReadProcessMemory
directly via ctypes) and only makes sense against a process you control,
for reverse-engineering your own recompiled build.

Usage:
    python scripts/scan_guest_memory.py scan <pid> --hex 0001090e
    python scripts/scan_guest_memory.py scan <pid> --u32be 1 9 14
    python scripts/scan_guest_memory.py read <pid> <host_addr_hex> --count 3 --u32be
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100

kernel32 = ctypes.windll.kernel32


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("PartitionId", wt.WORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


def open_process(pid):
    hproc = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not hproc:
        print(f"error: OpenProcess({pid}) failed (GetLastError={ctypes.GetLastError()})", file=sys.stderr)
        sys.exit(1)
    return hproc


def read_memory(hproc, addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(0)
    ok = kernel32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size, ctypes.byref(n))
    if not ok:
        return None
    return buf.raw[: n.value]


def be32(v):
    return bytes([(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])


def scan(pid, pattern, max_region_bytes):
    hproc = open_process(pid)
    addr = 0
    mbi = MEMORY_BASIC_INFORMATION()
    found = []
    while addr < 0x8000000000:
        ret = kernel32.VirtualQueryEx(hproc, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        if ret == 0:
            break
        region_size = mbi.RegionSize
        base = mbi.BaseAddress or addr
        if (
            mbi.State == MEM_COMMIT
            and not (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            and region_size <= max_region_bytes
        ):
            data = read_memory(hproc, base, region_size)
            if data:
                start = 0
                while True:
                    idx = data.find(pattern, start)
                    if idx == -1:
                        break
                    found.append(base + idx)
                    start = idx + 1
        addr = base + region_size
    return found


def cmd_scan(args):
    if args.hex:
        pattern = bytes.fromhex(args.hex)
    elif args.u32be:
        pattern = b"".join(be32(v) for v in args.u32be)
    else:
        print("error: provide --hex or --u32be", file=sys.stderr)
        sys.exit(1)

    print(f"pattern: {pattern.hex()}")
    hits = scan(args.pid, pattern, args.max_region * 1024 * 1024)
    print(f"found {len(hits)} match(es)")
    for a in hits:
        print(hex(a))


def cmd_read(args):
    hproc = open_process(args.pid)
    addr = int(args.addr, 16)
    if args.u32be:
        for i in range(args.count):
            data = read_memory(hproc, addr + i * 4, 4)
            if data is None:
                print(f"+0x{i * 4:x}: read failed")
                continue
            value = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]
            print(f"+0x{i * 4:x}: {value} (0x{value:08x})")
    else:
        data = read_memory(hproc, addr, args.count)
        print(data.hex() if data is not None else "read failed")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    p_scan = sub.add_parser("scan", help="scan process memory for an exact byte pattern")
    p_scan.add_argument("pid", type=int)
    p_scan.add_argument("--hex", help="pattern as a hex string, e.g. 0001090e")
    p_scan.add_argument("--u32be", type=int, nargs="+", help="pattern as a list of big-endian uint32 values")
    p_scan.add_argument("--max-region", type=int, default=256, help="skip regions larger than this many MB (default 256)")
    p_scan.set_defaults(func=cmd_scan)

    p_read = sub.add_parser("read", help="read raw bytes (or big-endian uint32s) at a host address")
    p_read.add_argument("pid", type=int)
    p_read.add_argument("addr", help="host address in hex, e.g. 0x183173cc8")
    p_read.add_argument("--count", type=int, default=16, help="bytes to read, or number of uint32s with --u32be")
    p_read.add_argument("--u32be", action="store_true", help="interpret as --count consecutive big-endian uint32 fields")
    p_read.set_defaults(func=cmd_read)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
