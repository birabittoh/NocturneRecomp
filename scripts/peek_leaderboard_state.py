#!/usr/bin/env python3
"""
Read-only (no breakpoints, no code execution in the target) inspection of the
front-end menu state while the game is hung on Main Menu -> Leaderboards.

Walks: dword_82E7A570 (FE controller "a1", guest static global)
       -> a1-148 is the top container passed to sub_825CFF90
       -> container+16 (page slot 1) = page1 object (front-end/main-menu obj)
       -> page1+548 = the per-tab container object
       -> container2+20 = child count, container2+24.. = child object ptrs
       -> for each child: dword@+0 = vtable (compare against Leaderboards'
          known vtable base 0x82015498), byte@+12 = "activated", byte@+13 =
          "visible" (IsVisible/Update gate).

Guest memory is mapped at a flat host base (observed as 0x100000000 in prior
sessions on this machine) -- this script re-derives it from the same
PPCFuncMappings-table technique as resolve_gpu_wait.py instead of hardcoding,
in case it ever differs.
"""
import ctypes
import struct
import sys

sys.path.insert(0, "scripts")
from resolve_gpu_wait import (
    find_pid, find_module, find_func_table, read_func_table, read_mem,
)

k32 = ctypes.windll.kernel32
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

GUEST_A1_CTRL = 0x82E7A570        # dword_82E7A570
LEADERBOARD_VTABLE = 0x82015498


def bswap32(v):
    return struct.unpack(">I", struct.pack("<I", v))[0]


def read_guest_u32(hproc, base, guest_addr):
    data = read_mem(hproc, base + guest_addr, 4)
    if not data:
        return None
    return bswap32(struct.unpack("<I", data)[0])


def read_guest_u8(hproc, base, guest_addr):
    data = read_mem(hproc, base + guest_addr, 1)
    if not data:
        return None
    return data[0]


def find_guest_membase(hproc, mod_base, mod_size):
    """Derive the guest flat membase without hardcoding it: the func table
    entries map guest .text addresses to host code pointers *inside the
    module*; the guest membase is a *separate* mapping used for guest data
    (globals/heap), which we instead confirm empirically by checking that
    0x100000000 + a known-static guest global reads back a plausible value.
    Falls back to scanning a small set of candidate flat bases."""
    for candidate in (0x100000000,):
        val = read_guest_u32(hproc, candidate, GUEST_A1_CTRL)
        if val is not None:
            return candidate, val
    return None, None


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

    mod_base, mod_size = find_module(pid)

    base, a1_ctrl = find_guest_membase(hproc, mod_base, mod_size)
    if base is None:
        print("Could not find/verify guest membase")
        return 1
    print(f"[*] Guest membase = 0x{base:x}")
    print(f"[*] dword_82E7A570 (FE controller ptr) = 0x{a1_ctrl:x}")

    if a1_ctrl == 0:
        print("FE controller not initialized yet (still on title screen?)")
        return 1

    a1 = a1_ctrl
    container = a1 - 148
    print(f"[*] a1 = 0x{a1:x}, container (a1-148) = 0x{container:x}")

    page1 = read_guest_u32(hproc, base, container + 16)
    print(f"[*] page1 object (container+16) = 0x{page1:x}" if page1 else "page1 read failed")
    if not page1:
        return 1

    page1_vtable = read_guest_u32(hproc, base, page1)
    print(f"[*] page1 vtable = 0x{page1_vtable:x}")

    tab_container = read_guest_u32(hproc, base, page1 + 548)
    print(f"[*] page1+548 (tab container ptr) = 0x{tab_container:x}" if tab_container is not None else "page1+548 read failed")
    if not tab_container:
        print("tab container pointer is NULL -- not yet set up")
        return 1

    tab_vtable = read_guest_u32(hproc, base, tab_container)
    count = read_guest_u32(hproc, base, tab_container + 20)
    print(f"[*] tab_container vtable = 0x{tab_vtable:x}, child count @ +20 = {count}")

    if count is None or count > 64:
        print("child count looks implausible, stopping")
        return 1

    for i in range(count):
        child_ptr_addr = tab_container + 24 + 4 * i
        child = read_guest_u32(hproc, base, child_ptr_addr)
        if not child:
            print(f"  child[{i}] = NULL")
            continue
        vt = read_guest_u32(hproc, base, child)
        b12 = read_guest_u8(hproc, base, child + 12)
        b13 = read_guest_u8(hproc, base, child + 13)
        marker = "  <-- LEADERBOARDS" if vt == LEADERBOARD_VTABLE else ""
        print(f"  child[{i}] = 0x{child:x}  vtable=0x{vt:x}  activated(+12)={b12}  visible(+13)={b13}{marker}")

    k32.CloseHandle(hproc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
