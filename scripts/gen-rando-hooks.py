#!/usr/bin/env python3
"""Compiles the randomizer's code patches into native hooks.

The randomizer core (ext/SOTN_XB_RANDO) patches instructions: it changes
immediates, nops branches, rewrites short sequences and branches into code caves
of its own. Recompiled functions never re-read their instructions from guest
memory, so each patched instruction ("site") gets a [[midasm_hook]] that runs
the patched version natively when the applied world has changed it:

  * the site's possible values are fixed by the core: a new 16-bit immediate
    for the same instruction ("imm", seed-dependent, read from guest memory
    when the world is applied) or one of a few whole instructions;
  * every value is translated here into C++ with the same semantics as the
    SDK's codegen output, and so is the code in the caves (the core's own
    blobs, read from its source), each entry point as a function that returns
    where it leaves to;
  * which value a site holds is decided when a world is applied
    (src/rando_runtime.cpp calls Activate), not per call.

A hook has two exits at most (`jump_address_on_true` / `_false`). A site whose
values all leave the same way returns false while it isn't patched, and the
recompiled instruction runs. A site with two ways out also runs its original
instruction compiled here. A run of sites that needs more is hooked as one
unit at its first word. Bounty Hunter's item drop hook has three ways out and
hands the third to the hook on the next word (FORWARD below).

Inputs: assets/default.xex (the instructions at the sites, the function
bounds) and the core's source (the caves). Outputs:
  src/rando_hooks.generated.cpp   built into the exe (gitignored)
  nocturnerecomp_rando_hooks.toml the hooks, included by the manifest; checked
                                  in, since codegen reads it. Rewritten with
                                  --write-toml, otherwise only verified.

SITES lists every word the core can write in .text and the whole instructions
it can write there. It was found by running the core over every preset (with
its options off and on, several seeds) and every enhancement, logging each
write (see the probe in the history of this file). A site or value missing
here is reported when a world using it is applied, and that change stays off.
"""
import argparse
import os
import re
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "ext", "KryptonRecomp"))
from xex_image import XexImage  # noqa: E402

XEX_PATH = os.path.join(ROOT, "assets", "default.xex")
CORE_PATH = os.path.join(ROOT, "ext", "SOTN_XB_RANDO", "Source", "xb_rando.cpp")
CPP_PATH = os.path.join(ROOT, "src", "rando_hooks.generated.cpp")
TOML_PATH = os.path.join(ROOT, "nocturnerecomp_rando_hooks.toml")

# address: "imm" (any immediate in the original instruction) and/or whole words.
# "tu": the Title Update 1 port already hooks this instruction with the same
# change (nocturnerecomp_tu_hooks.toml), so the site gets no hook of its own.
SITES = """
0x8223AD30 imm
0x8223AD34 imm
0x8223DC14 4864237C
0x8223FC80 4864034C
0x8224035C 4863FB14
0x8224E3B4 imm
0x8225AF1C imm
0x8225AF30 imm
0x8225AF48 imm
0x8225AF5C imm
0x8225AF6C imm
0x8225AF70 imm
0x8225AF84 imm
0x8225AF94 imm
0x8225AF9C imm
0x8225AFB0 imm
0x8225AFC0 imm
0x8225AFC8 imm
0x8225AFDC imm
0x8225AFEC imm
0x8225AFF4 imm
0x8225B00C imm
0x8225B01C imm
0x8225B024 imm
0x8225B04C imm
0x8225B070 imm
0x8225B080 imm
0x8225B200 imm 60000000
0x8225B204 38800001 38800003 38800061 60000000
0x8225B208 60000000 988B4940 988B4944 988B4947 988B494F 988B49C8 988B4BA3
0x8225B20C 38800063 60000000 988B4945 988B494C 988B494F 988B4A28
0x8225B210 imm 38840001 60000000 988B4946 988B494D 988B4952 988B4BA3
0x8225B214 imm 60000000 988B4953 988B49EC
0x8225B218 38840001 60000000 988B4954
0x8225B21C 60000000 988B494D 988B4955
0x8225B220 60000000 988B4944 988B4956
0x8225B2A4 60000000
0x8225B2A8 60000000
0x8225B2AC 60000000
0x8225B2B0 60000000
0x8225B2B8 imm
0x8225B2FC 60000000
0x8225B304 imm
0x8225B3E4 60000000
0x8225B400 39200001
0x8225B40C 39200001
0x8225B418 39200063
0x8225B424 39200001
0x8225B490 imm
0x8225B498 imm
0x8225B4A0 imm
0x8225B4A8 imm
0x8225B4B0 imm
0x8225B4B8 imm
0x8225B8F4 60000000
0x8225B934 60000000
0x8225B93C 60000000
0x8225BAC8 60000000
0x8225BAF8 60000000
0x8225BB58 60000000
0x822AB3FC 485CE608
0x822AB514 716B00A0
0x822AB540 485CE4C8
0x822AB58C 716B0060
0x822ABAD8 485CDF28
0x822C3D64 485B5C9C
0x822CC6F0 imm
0x822CCAAC imm
0x822CCBC4 7FFF0040
0x822CD988 60000000
0x822CDAB8 60000000
0x822CDB2C 485ABED4
0x822CE164 imm
0x822CE198 imm
0x822D75F0 485A2414
0x822D75F4 60000000
0x822E4FB8 imm
0x822E8140 imm
0x8230A84C imm
0x8230A868 48000034
0x8231223C 60000000
0x823123BC imm
0x823123C0 imm
0x823123C4 894B0117
0x823123C8 60000000
0x823123CC imm
0x823123D0 419A079C
0x823123D4 60000000
0x823123D8 60000000
0x823123DC 896B0118
0x823123E0 60000000
0x823123E4 imm
0x823123E8 419A0784
0x823123EC 60000000
0x823123F0 60000000
0x82314EEC 60000000
0x8231D158 60000000
0x8231D160 imm 4855C8A4
0x8231D170 imm
0x8234372C 39600000
0x82343AD0 60000000
0x8234891C 485310E4
0x82348920 60000000
0x8234D094 imm
0x8234D0A4 imm
0x8234E374 imm
0x8234E37C imm
0x82395914 60000000
0x82395948 imm
0x823ABA54 imm
0x823ABA64 imm
0x823C95D0 imm
0x823C95E0 imm
0x823CBD5C tu 2F09FFFF
0x8240252C imm
0x82406048 48000020
0x8240E064 imm
0x8240EA30 imm
0x82428AE8 714AFFFE
0x8242B768 imm
0x8242F2CC imm
0x824346FC 48000014
0x824451CC imm
0x8244BDA4 imm
0x82467C04 39400004
0x8247193C imm
0x82478C90 imm
0x8248AA58 imm
0x8248AA68 imm
0x8248F238 imm
0x8248F248 imm
0x824A148C imm
0x824A1490 imm
0x824A149C imm
0x824A17A4 imm
0x824A91A4 imm
0x824B23BC imm
0x824BDABC 4800004C
0x824D79DC imm
0x824D79E8 imm
0x824DB49C imm
0x824DB4A0 60000000
0x824DB4A4 imm
0x824DB4A8 409A003C
"""

# The core's code blobs, where it writes them and the sites it branches into
# them from. The game modes share one cave.
CAVES = [
    ("BountyHunter_CodeBlock", 0x82879A00, [0x8234891C]),
    ("BatMaster_CodeBlock_AirBrake", 0x82879A00, [0x822CDB2C, 0x822D75F0]),
    ("LegDay_CodeBlock", 0x82879A00, [0x822C3D64, 0x8231D160]),
    ("Richter_CodeBlock", 0x82879A00, [0x822AB3FC, 0x822AB540, 0x822ABAD8]),
    ("MapTracker_CodeBlock", 0x8287FE70, [0x8224035C]),
    ("RLBC_CodeBlock", 0x8287FF90, [0x8223DC14, 0x8223FC80]),
]

# Bounty Hunter's item drop (b cave at 0x8234891C, nop at 0x82348920) leaves
# to 0x82348970 or 0x8234893C, and the original pair to 0x8234893C or
# 0x82348924. The first hook takes 0x82348970 and hands 0x8234893C to the
# second one, which is the original bne's own target.
FORWARD = {0x8234891C: (0x82348970, 0x8234893C)}  # site: (its own exit, handed on)

RET = "return"  # an exit that returns from the hooked function


def die(msg):
    print(f"gen-rando-hooks: error: {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# PPC -> C++, mirroring the SDK codegen's output for each instruction.
# ---------------------------------------------------------------------------

CR_BITS = ("lt", "gt", "eq", "so")


def r(n):
    return f"ctx.r{n}"


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def mask64(mb, me):
    x = (1 << 64) - 1 >> mb
    y = (1 << 64) - 1 if me == 63 else ~((1 << 64) - 1 >> (me + 1)) & ((1 << 64) - 1)
    return (x & y) if mb <= me else (x | y)


class Imm:
    """The 16-bit immediate of an instruction: a constant, or the runtime value
    in `imm` (an "imm" site)."""

    def __init__(self, word, runtime):
        self.word = word
        self.runtime = runtime

    def s(self):  # sign-extended, as int32
        return "int32_t(int16_t(imm))" if self.runtime else str(sext16(self.word & 0xFFFF))

    def u(self):  # zero-extended
        return "uint32_t(imm)" if self.runtime else str(self.word & 0xFFFF)

    def shifted(self):  # addis: simm << 16
        if self.runtime:
            return "int32_t(uint32_t(imm) << 16)"
        return str(struct.unpack(">i", struct.pack(">I", (self.word & 0xFFFF) << 16))[0])

    def u_shifted(self):  # oris/andis.
        return "(uint32_t(imm) << 16)" if self.runtime else str((self.word & 0xFFFF) << 16)


class Flow:
    """Where translated code goes: `jump(target)` and `ret()` return C++."""

    def jump(self, target):
        raise NotImplementedError

    def ret(self):
        raise NotImplementedError


def is_imm_form(word):
    """D-form instructions whose low half is a 16-bit immediate or displacement."""
    return (word >> 26) in (7, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29,
                            32, 34, 36, 38, 40, 42, 44)


def translate(word, pc, flow, funcs, runtime_imm=False):
    """C++ statements for one instruction. Returns (lines, falls_through)."""
    op = word >> 26
    rt, ra, rb = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    imm = Imm(word, runtime_imm)
    rc = word & 1
    out = []

    def record(reg):
        if rc:
            out.append(f"ctx.cr0.compare<int32_t>({r(reg)}.s32, 0, ctx.xer);")

    if word == 0x60000000:  # nop
        return out, True
    if runtime_imm and not is_imm_form(word):
        die(f"{pc:08X}: {word:08X} has no immediate to patch")

    if op == 7:  # mulli
        out.append(f"{r(rt)}.s64 = static_cast<int64_t>({r(ra)}.u64 * static_cast<uint64_t>({imm.s()}));")
    elif op in (10, 11):  # cmpli / cmpi
        crf = rt >> 2
        wide = (rt & 1) != 0
        if op == 10:
            t, f, v = ("uint64_t", "u64", imm.u()) if wide else ("uint32_t", "u32", imm.u())
        else:
            t, f, v = ("int64_t", "s64", imm.s()) if wide else ("int32_t", "s32", imm.s())
        out.append(f"ctx.cr{crf}.compare<{t}>({r(ra)}.{f}, {v}, ctx.xer);")
    elif op in (12, 13):  # addic / addic.
        if runtime_imm:
            out.append(f"ctx.xer.ca = {r(ra)}.u32 > ~uint32_t({imm.s()});")
        else:
            out.append(f"ctx.xer.ca = {r(ra)}.u32 > {(~sext16(word & 0xFFFF)) & 0xFFFFFFFF};")
        out.append(f"{r(rt)}.s64 = {r(ra)}.s64 + {imm.s()};")
        if op == 13:
            out.append(f"ctx.cr0.compare<int32_t>({r(rt)}.s32, 0, ctx.xer);")
    elif op == 14:  # addi / li
        src = f"{r(ra)}.s64 + " if ra else ""
        out.append(f"{r(rt)}.s64 = {src}{imm.s()};")
    elif op == 15:  # addis / lis
        src = f"{r(ra)}.s64 + " if ra else ""
        out.append(f"{r(rt)}.s64 = {src}{imm.shifted()};")
    elif op == 16:  # bc
        bo, bi = rt, ra
        target = (pc + sext16(word & 0xFFFC)) & 0xFFFFFFFF
        conds = []
        if not bo & 0x04:
            out.append("--ctx.ctr.u64;")
            conds.append("ctx.ctr.u32 == 0" if bo & 0x02 else "ctx.ctr.u32 != 0")
        if not bo & 0x10:
            bit = f"ctx.cr{bi >> 2}.{CR_BITS[bi & 3]}"
            conds.append(bit if bo & 0x08 else f"!{bit}")
        if word & 3:
            die(f"{pc:08X}: {word:08X}: bcl/bca not supported")
        go = flow.jump(target)
        if conds:
            out.append(f"if ({' && '.join(conds)}) {{ {go} }}")
            return out, True
        out.append(go)
        return out, False
    elif op == 18:  # b / bl
        li = word & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        target = (pc + li) & 0xFFFFFFFF
        if word & 2:
            die(f"{pc:08X}: absolute branch not supported")
        if word & 1:
            if target not in funcs:
                die(f"{pc:08X}: bl to {target:08X}, which isn't a function")
            out.append(f"ctx.lr = 0x{pc + 4:X};")
            out.append(f"{funcs[target]}(ctx, base);")
            return out, True
        if target in funcs:  # tail call
            out.append(f"{funcs[target]}(ctx, base);")
            out.append(flow.ret())
        else:
            out.append(flow.jump(target))
        return out, False
    elif op == 19 and ((word >> 1) & 0x3FF) == 16 and rt == 20 and not word & 1:  # blr
        out.append(flow.ret())
        return out, False
    elif op == 21:  # rlwinm
        sh, mb, me = rb, (word >> 6) & 31, (word >> 1) & 31
        out.append(f"{r(ra)}.u64 = __builtin_rotateleft64({r(rt)}.u32 | ({r(rt)}.u64 << 32), {sh}) & "
                   f"0x{mask64(mb + 32, me + 32):X};")
        record(ra)
    elif op == 24:  # ori
        out.append(f"{r(ra)}.u64 = {r(rt)}.u64 | {imm.u()};")
    elif op == 25:  # oris
        out.append(f"{r(ra)}.u64 = {r(rt)}.u64 | {imm.u_shifted()};")
    elif op == 26:  # xori
        out.append(f"{r(ra)}.u64 = {r(rt)}.u64 ^ {imm.u()};")
    elif op == 27:  # xoris
        out.append(f"{r(ra)}.u64 = {r(rt)}.u64 ^ {imm.u_shifted()};")
    elif op == 28:  # andi.
        out.append(f"{r(ra)}.u64 = {r(rt)}.u64 & {imm.u()};")
        out.append(f"ctx.cr0.compare<int32_t>({r(ra)}.s32, 0, ctx.xer);")
    elif op == 29:  # andis.
        out.append(f"{r(ra)}.u64 = {r(rt)}.u64 & {imm.u_shifted()};")
        out.append(f"ctx.cr0.compare<int32_t>({r(ra)}.s32, 0, ctx.xer);")
    elif op == 31:
        xo = (word >> 1) & 0x3FF
        if xo in (0, 32):  # cmp / cmpl (L as the operand width)
            crf, wide = rt >> 2, (rt & 1) != 0
            if xo == 0:
                t, f = ("int64_t", "s64") if wide else ("int32_t", "s32")
            else:
                t, f = ("uint64_t", "u64") if wide else ("uint32_t", "u32")
            out.append(f"ctx.cr{crf}.compare<{t}>({r(ra)}.{f}, {r(rb)}.{f}, ctx.xer);")
        elif xo & 0x1FF == 266 and not word & 0x400:  # add
            out.append(f"{r(rt)}.u64 = {r(ra)}.u64 + {r(rb)}.u64;")
            record(rt)
        elif xo & 0x1FF == 40 and not word & 0x400:  # subf
            out.append(f"{r(rt)}.u64 = {r(rb)}.u64 - {r(ra)}.u64;")
            record(rt)
        elif xo == 444:  # or / mr
            out.append(f"{r(ra)}.u64 = {r(rt)}.u64 | {r(rb)}.u64;")
            record(ra)
        elif xo == 28:  # and
            out.append(f"{r(ra)}.u64 = {r(rt)}.u64 & {r(rb)}.u64;")
            record(ra)
        else:
            die(f"{pc:08X}: unsupported instruction {word:08X}")
    elif op in (32, 34, 40, 42):  # lwz / lbz / lhz / lha
        macro, field = {32: ("REX_LOAD_U32", "u64"), 34: ("REX_LOAD_U8", "u64"),
                        40: ("REX_LOAD_U16", "u64"), 42: ("REX_LOAD_U16", "s64")}[op]
        ea = f"{r(ra)}.u32 + {imm.s()}" if ra else imm.s()
        load = f"{macro}({ea})"
        if op == 42:
            load = f"int16_t({load})"
        out.append(f"{r(rt)}.{field} = {load};")
    elif op in (36, 38, 44):  # stw / stb / sth
        macro, field = {36: ("REX_STORE_U32", "u32"), 38: ("REX_STORE_U8", "u8"),
                        44: ("REX_STORE_U16", "u16")}[op]
        ea = f"{r(ra)}.u32 + {imm.s()}" if ra else imm.s()
        out.append(f"{macro}({ea}, {r(rt)}.{field});")
    else:
        die(f"{pc:08X}: unsupported instruction {word:08X}")
    return out, True


def static_exits(word, pc, funcs):
    """Where one instruction can go next: addresses, RET, or a cave branch."""
    op = word >> 26
    if op == 16:
        target = (pc + sext16(word & 0xFFFC)) & 0xFFFFFFFF
        always = ((word >> 21) & 0x14) == 0x14
        return {target} if always else {target, pc + 4}
    if op == 18 and not word & 1:
        li = word & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        target = (pc + li) & 0xFFFFFFFF
        return {RET} if target in funcs else {target}
    if op == 19 and ((word >> 1) & 0x3FF) == 16:
        return {RET}
    return {pc + 4}


# ---------------------------------------------------------------------------
# Caves
# ---------------------------------------------------------------------------


class Cave:
    def __init__(self, name, addr, data, sites):
        self.name, self.addr, self.data, self.sites = name, addr, data, sites
        self.end = addr + len(data)
        self.entries = {}  # entry -> (function name, exits)

    def word(self, a):
        return struct.unpack_from(">I", self.data, a - self.addr)[0]

    def contains(self, a):
        return self.addr <= a < self.end


def translate_cave(cave, entry, funcs):
    """A function running the cave from `entry`; returns where it leaves to
    (0: returns from the hooked function)."""
    reach, work = set(), [entry]
    targets = set()
    while work:
        pc = work.pop()
        while cave.contains(pc) and pc not in reach:
            reach.add(pc)
            w = cave.word(pc)
            nxt = static_exits(w, pc, funcs)
            inside = [t for t in nxt if t != RET and cave.contains(t)]
            for t in inside:
                if t != pc + 4:
                    targets.add(t)
                    work.append(t)
            if pc + 4 not in nxt:
                break
            pc += 4
        if not cave.contains(pc) and pc not in reach:
            die(f"cave {cave.name} runs off its end at {pc:08X}")

    exits = set()

    class CaveFlow(Flow):
        def jump(self, target):
            if cave.contains(target):
                return f"goto L_{target:08X};"
            exits.add(target)
            return f"return 0x{target:08X};"

        def ret(self):
            exits.add(RET)
            return "return 0;"

    name = f"Cave_{cave.name}_{entry:08X}"
    lines = [f"uint32_t {name}(PPCContext& ctx, uint8_t* base) {{"]
    if entry not in targets:
        lines.append(f"  goto L_{entry:08X};")
        targets.add(entry)
    prev = None
    for pc in sorted(reach):
        if prev is not None and prev_falls and pc != prev + 4:
            die(f"cave {cave.name}: fall-through gap at {prev:08X}")
        if pc in targets:
            lines.append(f"L_{pc:08X}:")
        body, prev_falls = translate(cave.word(pc), pc, CaveFlow(), funcs)
        lines.append(f"  // {pc:08X}: {cave.word(pc):08X}")
        lines += ["  " + l for l in body]
        if prev_falls and pc + 4 not in reach:
            lines.append(f"  goto L_{pc + 4:08X};")
            if not cave.contains(pc + 4):
                die(f"cave {cave.name} falls off its end at {pc:08X}")
        prev = pc
    lines.append("}")
    return name, exits, lines


# ---------------------------------------------------------------------------
# Sites and hook units
# ---------------------------------------------------------------------------


class Variant:
    def __init__(self, kind, word=None, cave=None):
        self.kind = kind    # "imm", "word" or "cave"
        self.word = word
        self.cave = cave    # (Cave, entry) for "cave"


class Site:
    def __init__(self, index, addr, orig, imm, words):
        self.index, self.addr, self.orig = index, addr, orig
        self.variants = []
        for w in words:
            self.variants.append(Variant("word", w))
        if imm:
            if not is_imm_form(orig):
                die(f"site {addr:08X}: {orig:08X} has no immediate")
            self.variants.append(Variant("imm", orig))


def load_caves(funcs):
    src = open(CORE_PATH, encoding="latin-1").read()
    caves = []
    for name, addr, sites in CAVES:
        m = re.search(r"unsigned char " + name + r"\[\]\s*=\s*\{([^}]*)\}", src)
        if not m:
            die(f"{name} not found in {CORE_PATH}")
        data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})", m.group(1)))
        caves.append(Cave(name, addr, data, sites))
    return caves


def pdata_functions(img):
    """(start, end) of every .pdata entry, sorted."""
    pe = img.data
    e_lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec = struct.unpack_from("<H", pe, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", pe, e_lfanew + 20)[0]
    sec = e_lfanew + 24 + opt_size
    for i in range(nsec):
        name = pe[sec + 40 * i:sec + 40 * i + 8].rstrip(b"\0")
        vsize, rva = struct.unpack_from("<II", pe, sec + 40 * i + 8)
        if name == b".pdata":
            funcs = []
            for off in range(rva, rva + vsize - 7, 8):
                begin, info = struct.unpack_from(">II", pe, off)
                if begin:
                    funcs.append((begin, begin + ((info >> 8) & 0x3FFFFF) * 4))
            return sorted(funcs)
    die("no .pdata in default.xex")


def function_names(img, functions):
    """Callable addresses and their names in the generated code: the .pdata
    functions, and the register save/restore helpers codegen finds by pattern
    (phase_register.cpp: 18 entry points per run, 4 bytes apart)."""
    names = {b: f"sub_{b:08X}" for b, _ in functions}
    text = [(b, e) for b, e in functions]
    lo, hi = text[0][0], text[-1][1]
    for pc in range(lo, hi, 4):
        w = img.u32(pc)
        for pattern, prefix in ((0xE9C1FF68, "__restgprlr_"), (0xF9C1FF68, "__savegprlr_")):
            if w == pattern:
                for n in range(14, 32):
                    names[pc + (n - 14) * 4] = f"{prefix}{n}"
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xex", default=XEX_PATH)
    ap.add_argument("--write-toml", action="store_true",
                    help="rewrite nocturnerecomp_rando_hooks.toml instead of checking it")
    args = ap.parse_args()

    img = XexImage.load(args.xex)
    functions = pdata_functions(img)
    funcs = function_names(img, functions)
    caves = load_caves(funcs)

    sites = []
    for line in SITES.split("\n"):
        p = line.split()
        if not p:
            continue
        addr = int(p[0], 16)
        site = Site(len(sites), addr, img.u32(addr), "imm" in p[1:],
                    [int(x, 16) for x in p[1:] if x not in ("imm", "tu")])
        site.tu = "tu" in p[1:]
        sites.append(site)
    by_addr = {s.addr: s for s in sites}

    # Branches into a cave become one variant per blob with code there.
    cave_funcs = []
    for s in sites:
        expanded = []
        for v in s.variants:
            t = None
            if v.kind == "word" and v.word >> 26 == 18 and not v.word & 3:
                t = next(iter(static_exits(v.word, s.addr, funcs)))
            hits = [c for c in caves if s.addr in c.sites]
            if t is not None and t != RET and any(c.contains(t) for c in caves) and                     not all(c.contains(t) for c in hits):
                die(f"site {s.addr:08X} branches to cave {t:08X}, but CAVES doesn't list it there")
            if not hits or t is None:
                expanded.append(v)
                continue
            for c in hits:
                if t not in c.entries:
                    name, exits, lines = translate_cave(c, t, funcs)
                    c.entries[t] = (name, exits)
                    cave_funcs.append(lines)
                expanded.append(Variant("cave", v.word, (c, t)))
        s.variants = expanded

    def variant_exits(s, v):
        if v.kind == "cave":
            return set(v.cave[0].entries[v.cave[1]][1])
        return static_exits(v.word, s.addr, funcs)

    # Exits handed to the hook on the next word.
    incoming = {site + 4: {handed} for site, (_, handed) in FORWARD.items()}

    # Hooks: one per site, or one for a run of contiguous sites, at its start.
    # Each is (sites, true exit, false exit, runs the original, handed on).
    def alone(s):
        """The site's own hook, or None if it needs its run hooked as one."""
        exits = set().union(*(variant_exits(s, v) for v in s.variants))
        if s.addr in FORWARD:
            own, handed = FORWARD[s.addr]
            both = exits | static_exits(s.orig, s.addr, funcs)
            if both != {own, handed, s.addr + 4}:
                die(f"{s.addr:08X}: FORWARD doesn't match its exits")
            return ([s], own, s.addr + 4, True, {handed})
        exits |= incoming.get(s.addr, set())
        if len(exits) == 1:
            return ([s], exits.pop(), None, False, set())
        both = sorted(exits | static_exits(s.orig, s.addr, funcs), key=str)
        if len(both) <= 2:
            return ([s], both[0], both[1] if len(both) > 1 else None, True, set())
        return None

    hooks = []
    hooked = [s for s in sites if not s.tu]
    i = 0
    while i < len(hooked):
        j = i
        while j + 1 < len(hooked) and hooked[j + 1].addr == hooked[j].addr + 4:
            j += 1
        singles = [alone(s) for s in hooked[i:j + 1]]
        if all(singles):
            hooks += singles
            i = j + 1
            continue
        # Hook the contiguous run as one unit.
        run = hooked[i:j + 1]
        start, end = run[0].addr, run[-1].addr + 4
        if any(rs.addr in incoming or rs.addr in FORWARD for rs in run):
            die(f"run {start:08X}-{end:08X} mixes with FORWARD")
        out = set()
        for rs in run:
            for e in set().union(static_exits(rs.orig, rs.addr, funcs),
                                 *(variant_exits(rs, v) for v in rs.variants)):
                if e == RET or not start <= e < end:
                    out.add(e)
        if len(out) > 2:
            die(f"run {start:08X}-{end:08X} leaves to {len(out)} places")
        # Code elsewhere must not branch into the middle of the run: it would
        # skip the hook.
        fn = next(((fb, fe) for fb, fe in functions if fb <= start < fe), None)
        if not fn:
            die(f"run {start:08X}-{end:08X} isn't in a .pdata function")
        for pc in range(fn[0], fn[1], 4):
            if start <= pc < end:
                continue
            w = img.u32(pc)
            if w >> 26 in (16, 18) and not w & 1:
                for t in static_exits(w, pc, funcs):
                    if t != RET and start < t < end:
                        die(f"{pc:08X} branches into the middle of run {start:08X}-{end:08X}")
        out = sorted(out, key=str)
        hooks.append((run, out[0], out[1] if len(out) > 1 else None, True, set()))
        i = j + 1

    write_cpp(sites, caves, cave_funcs, hooks, funcs)
    toml = make_toml(hooks)
    if args.write_toml:
        open(TOML_PATH, "w", newline="\n").write(toml)
    else:
        current = open(TOML_PATH).read() if os.path.exists(TOML_PATH) else ""
        if current != toml:
            die(f"{os.path.relpath(TOML_PATH, ROOT)} is out of date; run "
                "python scripts/gen-rando-hooks.py --write-toml and re-run codegen")
    print(f"gen-rando-hooks: {len(sites)} sites, {len(hooks)} hooks, "
          f"{sum(len(c.entries) for c in caves)} cave entries")


def hook_name(addr):
    return f"RandoSite_{addr:08X}"


def make_toml(hooks):
    lines = [
        "# Randomizer code patches: one hook per patched instruction (or run of",
        "# instructions). Written by scripts/gen-rando-hooks.py --write-toml; see",
        "# src/rando_hooks.h.",
    ]
    for run, t, f, _, _ in hooks:
        lines += ["", "[[midasm_hook]]", f"address = 0x{run[0].addr:08X}",
                  f'name = "{hook_name(run[0].addr)}"', 'registers = ["r3"]']
        for key, e in (("true", t), ("false", f)):
            if e == RET:
                lines.append(f"return_on_{key} = true")
            elif e is not None:
                lines.append(f"jump_address_on_{key} = 0x{e:08X}")
    return "\n".join(lines) + "\n"


def write_cpp(sites, caves, cave_funcs, hooks, funcs):
    o = []
    o.append("// Generated by scripts/gen-rando-hooks.py. Do not edit.")
    o.append('#include "rando_hooks.h"')
    o.append("")
    o.append("#include <atomic>")
    o.append("#include <cstring>")
    o.append("#include <utility>")
    o.append("")
    o.append('#include "generated/nocturnerecomp_init.h"')
    o.append("")
    o.append('#include "function_hooks.h"')
    o.append("")
    o.append("namespace nocturne::rando_hooks {")
    o.append("namespace {")
    o.append("")
    o.append(f"constexpr size_t kSiteCount = {len(sites)};")
    o.append("// Per site: 0 while the world in memory leaves it alone, else")
    o.append("// (variant + 1) << 16 | the immediate of an \"imm\" variant.")
    o.append("std::atomic<uint32_t> g_state[kSiteCount];")
    o.append("uint8_t* g_base = nullptr;")
    o.append("// An exit a hook hands to the hook on the next word (see FORWARD in the generator).")
    o.append("thread_local uint32_t g_forward = 0;")
    o.append("")
    o.append("struct SiteInfo {")
    o.append("  uint32_t addr;")
    o.append("  uint32_t imm_high;  // high half of the original instruction; 0: no imm variant")
    o.append("  uint32_t words[8];  // whole-word variants, in variant order")
    o.append("  const uint8_t* caves[8];  // the blob a cave variant runs (null: none)")
    o.append("  uint32_t cave_addr[8];")
    o.append("  uint32_t cave_size[8];")
    o.append("  uint32_t count;")
    o.append("};")
    o.append("")
    for c in caves:
        if not c.entries:
            continue
        o.append(f"constexpr uint8_t k{c.name}[] = {{")
        for i in range(0, len(c.data), 16):
            o.append("    " + ", ".join(f"0x{b:02X}" for b in c.data[i:i + 16]) + ",")
        o.append("};")
    o.append("")
    o.append("const SiteInfo kSites[kSiteCount] = {")
    for s in sites:
        words, cv, ca, cs = [], [], [], []
        imm_high = 0
        for v in s.variants:
            if v.kind == "imm":
                imm_high = (s.orig >> 16) | 0x10000  # bit 16: present
                words.append("0")
                cv.append("nullptr"); ca.append("0"); cs.append("0")
            else:
                words.append(f"0x{v.word:08X}")
                if v.kind == "cave":
                    c = v.cave[0]
                    cv.append(f"k{c.name}"); ca.append(f"0x{c.addr:08X}"); cs.append(str(len(c.data)))
                else:
                    cv.append("nullptr"); ca.append("0"); cs.append("0")
        if len(words) > 8:
            die(f"site {s.addr:08X} has more than 8 variants")
        o.append(f"    {{0x{s.addr:08X}, 0x{imm_high:X}, {{{', '.join(words)}}}, {{{', '.join(cv)}}}, "
                 f"{{{', '.join(ca)}}}, {{{', '.join(cs)}}}, {len(s.variants)}}},")
    o.append("};")
    o.append("")
    for lines in cave_funcs:
        o += lines
        o.append("")
    o.append("}  // namespace")
    o.append("")
    o.append(ACTIVATE_CPP)
    o.append("}  // namespace nocturne::rando_hooks")
    o.append("")
    o.append("using nocturne::rando_hooks::g_state;")
    o.append("using nocturne::rando_hooks::g_base;")
    o.append("using nocturne::rando_hooks::g_forward;")
    o.append("")

    site_index = {s.addr: s.index for s in sites}
    incoming = {site + 4: {handed} for site, (_, handed) in FORWARD.items()}
    for run, t, f, run_original, fwd in hooks:
        o += emit_hook(run, t, f, run_original, fwd, funcs, site_index,
                       incoming.get(run[0].addr, set()))
        o.append("")
    open(CPP_PATH, "w", newline="\n").write("\n".join(o))


def emit_hook(run, t, f, run_original, fwd, funcs, site_index, incoming):
    start, end = run[0].addr, run[-1].addr + 4
    labels = set()

    class HookFlow(Flow):
        def jump(self, target):
            if target != RET and start <= target < end:
                labels.add(target)
                return f"goto W_{target:08X};"
            if target == t:
                return "return true;"
            if target == f:
                return "return false;"
            if target in fwd:
                return f"g_forward = 0x{target:08X}; return false;"
            die(f"hook {start:08X}: no way out to {target}")

        def ret(self):
            return self.jump(RET)

    flow = HookFlow()
    words = []  # (addr, lines)
    for s in run:
        idx = site_index[s.addr]
        b = [f"{{  // site {idx}, originally {s.orig:08X}",
             f"  const uint32_t st = g_state[{idx}].load(std::memory_order_acquire);",
             "  switch (st >> 16) {"]
        for n, v in enumerate(s.variants):
            b.append(f"    case {n + 1}: {{")
            if v.kind == "imm":
                b.append("      const uint16_t imm = uint16_t(st);")
                lines, falls = translate(s.orig, s.addr, flow, funcs, runtime_imm=True)
            elif v.kind == "cave":
                cave, entry = v.cave
                name, exits = cave.entries[entry]
                lines = [f"const uint32_t exit = nocturne::rando_hooks::{name}(ctx, base);"]
                ordered = sorted(exits, key=str)
                for e in ordered[:-1]:
                    lines.append(f"if (exit == {'0' if e == RET else f'0x{e:08X}'}) {{ {flow.jump(e)} }}")
                lines.append(flow.jump(ordered[-1]))
                falls = False
            else:
                lines, falls = translate(v.word, s.addr, flow, funcs)
            b += ["      " + l for l in lines]
            if falls:
                b.append("      " + flow.jump(s.addr + 4))
            b.append("    }")
        b.append("    default: {")
        if run_original:
            lines, falls = translate(s.orig, s.addr, flow, funcs)
            b += ["      " + l for l in lines]
            if falls:
                b.append("      " + flow.jump(s.addr + 4))
        else:
            b.append("      return false;")
        b.append("    }")
        b.append("  }")
        b.append("}")
        words.append((s.addr, b))

    o = [f"bool {hook_name(start)}(PPCRegister& r3) {{"]
    if incoming:
        o.append("  // The previous hook left to here on its way to `to`.")
        o.append("  if (const uint32_t to = std::exchange(g_forward, 0)) {")
        for e in sorted(incoming):
            o.append(f"    if (to == 0x{e:08X}) {flow.jump(e)}")
        o.append("  }")
    o.append("  PPCContext& ctx = nocturne::fnhook::ContextFromR3(r3);")
    o.append("  [[maybe_unused]] uint8_t* base = g_base;")
    for addr, b in words:
        if addr in labels:
            o.append(f"W_{addr:08X}:")
        o += ["  " + l for l in b]
    o.append("}")
    return o


ACTIVATE_CPP = r"""
void Init(uint8_t* base) { g_base = base; }

void Activate(uint8_t* base, const std::function<bool(uint32_t)>& patched, Report* report) {
  *report = {};
  for (size_t i = 0; i < kSiteCount; ++i) {
    const SiteInfo& site = kSites[i];
    uint32_t state = 0;
    if (patched(site.addr)) {
      const uint32_t word = REX_LOAD_U32(site.addr);
      for (uint32_t v = 0; v < site.count && !state; ++v) {
        if (site.words[v] == 0) {
          // The same instruction with a new immediate. With the original
          // instructions blanked out of the image, only the immediate is there.
          const uint32_t high = word >> 16;
          if (high == (site.imm_high & 0xFFFF) || high == 0) {
            state = ((v + 1) << 16) | (word & 0xFFFF);
          }
        } else if (word == site.words[v] &&
                   (!site.caves[v] || std::memcmp(base + site.cave_addr[v], site.caves[v],
                                                  site.cave_size[v]) == 0)) {
          state = (v + 1) << 16;
        }
      }
      if (state) {
        report->active_sites.emplace_back(site.addr, state);
      } else {
        report->unknown.push_back(site.addr);
      }
    }
    g_state[i].store(state, std::memory_order_release);
  }
}

void Deactivate() {
  for (auto& state : g_state) {
    state.store(0, std::memory_order_release);
  }
}

bool IsSite(uint32_t addr) {
  for (const SiteInfo& site : kSites) {
    if (addr >= site.addr && addr < site.addr + 4) {
      return true;
    }
  }
  return false;
}
"""

if __name__ == "__main__":
    main()
