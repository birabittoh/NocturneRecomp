#!/usr/bin/env python3
"""Map vanilla guest function/label addresses to their title-update (TU)
equivalents by matching recompiled function bodies between two codegen
output trees.

The TU relocates code by *regional* deltas (observed: +0x1D8 around
0x82525xxx, +0x200 around 0x825B6xxx-0x825C1xxx), so a single constant
offset cannot be assumed -- every address must be mapped individually.

Workflow (see nocturnerecomp_tu_config.toml's midasm_hook comments for a
worked example):

  1. Generate VANILLA output. codegen auto-applies a sibling
     assets/default.xexp if present, so move it away first:
       mv assets/default.xexp /tmp/          (or anywhere)
       sdk/bin/rexglue codegen nocturnerecomp_manifest.toml
       cp -r generated /tmp/generated_vanilla
  2. Generate TU output (restore the xexp; .tu_build.toml points codegen at
     nocturnerecomp_tu_config.toml):
       mv /tmp/default.xexp assets/
       sdk/bin/rexglue codegen .tu_build.toml
       cp -r generated /tmp/generated_tu
  3. Match:
       python scripts/match_tu_functions.py /tmp/generated_vanilla \
           /tmp/generated_tu sub_825252C8 sub_825B6AB0 ...

For each vanilla function this prints its TU counterpart -- EXACT when the
normalized bodies hash identically, otherwise the best fuzzy match among TU
functions within +0x2000 of the vanilla address (relocation deltas observed
so far are small and positive). Fuzzy ratios >= ~0.85 with a plausible
regional delta have been reliable; confirm by eyeballing a distinctive
instruction sequence in both trees before shipping an address.

To map a *label* (loc_XXXX, e.g. a midasm hook site or jump target) inside a
matched function: grep the TU tree for the matched function's body around the
corresponding instructions -- the TU labels are the relocated addresses (the
generated code comments carry the original PPC instruction text, which is
identical between builds for unmodified code).

Normalization strips everything address-bearing: labels, sub_XXXX callee
names, 0x82xxxxxx literals, decimal-encoded guest-address constants (lis
materializations like `ctx.r30.s64 = -2098921472`), and this project's
injected midasm-hook call lines (present only in trees generated from
configs that declare them).
"""
import re, sys, glob, hashlib, os, difflib
from collections import defaultdict

FUNC_RE = re.compile(r"DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)")
ADDR = re.compile(r"0x8[0-9a-fA-F]{7}")
LABEL = re.compile(r"^loc_[0-9A-F]+:")
SUB = re.compile(r"sub_[0-9A-F]{8}")
NUM = re.compile(r"-?\d{5,}")


def parse_functions(d):
    funcs = {}
    paths = glob.glob(os.path.join(d, "*_recomp.*.cpp"))
    if not paths:
        sys.exit(f"error: no *_recomp.*.cpp files under {d}")
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            cur, body = None, []
            for line in f:
                m = FUNC_RE.search(line)
                if m:
                    if cur:
                        funcs[cur] = body
                    cur, body = m.group(1), []
                elif cur is not None:
                    body.append(line.rstrip("\n"))
            if cur:
                funcs[cur] = body
    return funcs


def _num_norm(m):
    v = int(m.group(0)) & 0xFFFFFFFF
    return "GADDR" if 0x80000000 <= v < 0x94000000 else m.group(0)


def norm_line(l):
    l = l.strip()
    if not l:
        return None
    if LABEL.match(l):
        return "LABEL"
    # midasm-hook lines exist only in trees whose config declares them
    if "Leaderboard" in l or "HeadlessRingWaitBypass" in l or l.startswith("extern "):
        return None
    l = ADDR.sub("ADDR", l)
    l = SUB.sub("SUB", l)
    l = NUM.sub(_num_norm, l)
    return l


def norm_body(body):
    return [n for n in (norm_line(l) for l in body) if n is not None]


def digest(lines):
    return hashlib.sha1("\n".join(lines).encode()).hexdigest()


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: match_tu_functions.py <vanilla_generated_dir> <tu_generated_dir> sub_XXXXXXXX...")
    van_dir, tu_dir, targets = sys.argv[1], sys.argv[2], sys.argv[3:]

    print("parsing vanilla...", flush=True)
    van = parse_functions(van_dir)
    print(f"  {len(van)} funcs", flush=True)
    print("parsing TU...", flush=True)
    tu = parse_functions(tu_dir)
    print(f"  {len(tu)} funcs", flush=True)

    tu_index = defaultdict(list)
    tu_norm = {name: norm_body(body) for name, body in tu.items()}
    for name, nb in tu_norm.items():
        tu_index[digest(nb)].append(name)

    for t in targets:
        body = van.get(t)
        if body is None:
            print(f"{t}: NOT FOUND in vanilla output")
            continue
        nb = norm_body(body)
        matches = tu_index.get(digest(nb), [])
        if matches:
            print(f"{t} -> EXACT {matches}")
            continue
        va = int(t[4:], 16)
        best = (0.0, None)
        for name, ntb in tu_norm.items():
            ta = int(name[4:], 16)
            if not (va <= ta <= va + 0x2000):
                continue
            if abs(len(ntb) - len(nb)) > max(20, len(nb) // 10):
                continue
            r = difflib.SequenceMatcher(None, nb, ntb, autojunk=False).ratio()
            if r > best[0]:
                best = (r, name)
        if best[1]:
            delta = int(best[1][4:], 16) - va
            print(f"{t} -> FUZZY best {best[1]} (ratio {best[0]:.4f}, delta +0x{delta:X})")
        else:
            print(f"{t} -> NO MATCH")


if __name__ == "__main__":
    main()
