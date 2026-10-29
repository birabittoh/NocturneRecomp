#!/usr/bin/env python3
"""The guest image as a recipe over the player's own default.xex.

The executable runs World's code, so the image it loads has World's layout.
None of World's data is shipped to build it: the image is put together at
startup (src/core/guest_image.cpp) from whichever release the player installed.
With World's default.xex the data is already where it belongs; JP's holds
almost all of it too, at other addresses, since JP is a separate build of
the same game. The recipe says where:

  src/guest_data/recipe.txt      `copy world_address jp_address size`: the
                                 bytes at jp_address in JP's image (at
                                 world_address in World's) go to world_address.
  src/guest_data/fixups.txt      `address size`: pointers inside the copied
                                 data, World's addresses for the slots where
                                 JP's own differ.
  src/guest_data/world_only.txt  `address size`: the little JP doesn't hold
                                 at all.

Those two only list ranges: their bytes are World's own, which `pack` reads
from World's default.xex at build time, so none of them are committed.

Everything else is zero, code included (codegen already compiled it), except
the import records, which come from xex_header.txt.

    python scripts/gen-guest-image.py out/guest-image.xex
    python scripts/guest_recipe.py derive WORLD.xex JP.xex out/guest-image.xex
        One time: write the recipe so it rebuilds the target image (that
        file, or a flat image as long as it) from either release.
    python scripts/guest_recipe.py check WORLD.xex JP.xex [TARGET]
        Rebuild the image from each release and compare them (and TARGET).
    python scripts/guest_recipe.py pack WORLD.xex OUT.bin
        The blob the executable links: the XEX header, the recipe and
        World's bytes for the ranges above.
"""
import array
import bisect
import collections
import os
import struct
import sys

from xex_header import PAGE_SIZE, build_header, import_words, read_header

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIR = os.path.join(ROOT, "src", "guest_data")
RECIPE_FILE = os.path.join(SOURCE_DIR, "recipe.txt")
FIXUPS_FILE = os.path.join(SOURCE_DIR, "fixups.txt")
WORLD_ONLY_FILE = os.path.join(SOURCE_DIR, "world_only.txt")

# PE timestamps (big-endian, as the image holds them) of the two releases.
WORLD_TIMESTAMP = 0x5DAEED45
JP_TIMESTAMP = 0x5B4B7B46

PACK_MAGIC = b"NRGR"
PACK_VERSION = 1

# A copy shorter than this isn't JP's data any more, only bytes that happen
# to be equal: those are World's and go to world_only.txt.
MIN_COPY = 8
# k-mer sizes of the matching passes, longest first.
PASSES = (24, 16, 12, 8)
POINTER_LO, POINTER_HI = 0x82000000, 0x84000000


def load_image(path):
    sys.path.insert(0, os.path.join(ROOT, "ext", "KryptonRecomp"))
    from xex_image import XexImage
    return XexImage.load(path).data


def load_target(path):
    data = open(path, "rb").read()
    return load_image(path) if data[:4] == b"XEX2" else data


def timestamp(image):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    return struct.unpack_from(">I", image, pe + 8)[0]


def is_pointer(value):
    return POINTER_LO <= value < POINTER_HI


def normalized(data):
    """`data` with every aligned word that looks like a guest pointer blanked,
    so data that only differs by where it points still matches."""
    out = bytearray(data)
    words = array.array("I", data[:len(data) // 4 * 4])
    if sys.byteorder == "little":
        words.byteswap()
    for i, word in enumerate(words):
        if POINTER_LO <= word < POINTER_HI:
            out[4 * i:4 * i + 4] = b"\xfe\xfe\xfe\xfe"
    return bytes(out)


def match(target, jp):
    """(world_offset, jp_offset, size) runs of `target` found in `jp`."""
    tn = normalized(target)
    jn = normalized(jp)
    n, nj = len(tn), len(jn)
    covered = bytearray(n)
    runs = []

    def extend(w, j):
        back = 0
        while (w - back > 0 and j - back > 0 and not covered[w - back - 1]
               and tn[w - back - 1] == jn[j - back - 1]):
            back += 1
        end, jend = w, j
        while end < n and jend < nj and not covered[end] and tn[end] == jn[jend]:
            end += 1
            jend += 1
        return w - back, j - back, end

    for k in PASSES:
        index = collections.defaultdict(list)
        for off in range(0, n - k, 4 if k > 16 else 1):
            if covered[off] or not any(target[off:off + k]):
                continue
            key = tn[off:off + k]
            if key.count(0) > k * 2 // 3:
                continue
            slots = index[key]
            if len(slots) < 64:
                slots.append(off)
        anchors = collections.defaultdict(set)
        for j in range(nj - k):
            for off in index.get(jn[j:j + k], ()):
                if len(anchors[off]) < 16:
                    anchors[off].add(j)
        for w in sorted(anchors):
            if covered[w]:
                continue
            best = max((extend(w, j) for j in anchors[w]), key=lambda r: r[2] - r[0])
            start, jstart, end = best
            if end - start >= MIN_COPY:
                covered[start:end] = b"\1" * (end - start)
                runs.append((start, jstart, end - start))
        print(f"  k={k}: {len(runs)} runs, {sum(r[2] for r in runs)} bytes", flush=True)
    return sorted(runs)


def copies(target, world, jp, runs):
    """The runs cut down to the bytes JP really has: a zero the target keeps
    where either release has data splits a run, so nothing it dropped is
    copied from either."""
    out = []
    for start, jstart, size in runs:
        seg = None
        for i in range(size):
            t = target[start + i]
            if t == 0 and (jp[jstart + i] or world[start + i]):
                if seg is not None:
                    out.append(seg)
                    seg = None
                continue
            if seg is None:
                seg = [start + i, jstart + i, 0]
            seg[2] = start + i + 1 - seg[0]
        if seg is not None:
            out.append(seg)
    return [tuple(s) for s in out if s[2] >= MIN_COPY]


def apply_copies(size, source, recipe, from_jp):
    image = bytearray(size)
    for world, jp, n in recipe:
        src = jp if from_jp else world
        image[world:world + n] = source[src:src + n]
    return image


def spans(offsets):
    """Consecutive offsets as (start, end) pairs."""
    out = []
    for off in offsets:
        if out and out[-1][1] == off:
            out[-1][1] = off + 1
        else:
            out.append([off, off + 1])
    return out


def pointer_bytes(target, jp, recipe, wrong):
    """The offsets in `wrong` that are part of a pointer, in the target and in
    the JP data copied there."""
    starts = [r[0] for r in recipe]
    out = set()
    for off in wrong:
        i = bisect.bisect_right(starts, off) - 1
        copied = i >= 0 and off < recipe[i][0] + recipe[i][2]
        delta = recipe[i][1] - recipe[i][0] if copied else 0
        for a in range(off - 3, off + 1):
            if a < 0 or a + 4 > len(target):
                continue
            if not is_pointer(struct.unpack_from(">I", target, a)[0]):
                continue
            # An unaligned one only if JP has a pointer there too: Shift-JIS
            # text starts with 0x82 and 0x83 as well.
            if a % 4 == 0 or (copied and 0 <= a + delta <= len(jp) - 4 and
                              is_pointer(struct.unpack_from(">I", jp, a + delta)[0])):
                out.add(off)
                break
    return out


def write_ranges(path, header, base, ranges):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        for start, end in ranges:
            f.write(f"0x{base + start:08X} 0x{end - start:X}\n")


def read_bytes(path, base, world):
    """(offset, World's bytes) for each range listed in `path`."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            words = line.split("#")[0].split()
            if words:
                off, size = int(words[0], 16) - base, int(words[1], 16)
                out.append((off, bytes(world[off:off + size])))
    return out


def load_world(path):
    world = load_image(path)
    if timestamp(world) != WORLD_TIMESTAMP:
        sys.exit(f"error: {path} isn't World's XBLA default.xex")
    return world


def read_recipe(base):
    out = []
    with open(RECIPE_FILE, encoding="utf-8") as f:
        for line in f:
            words = line.split("#")[0].split()
            if words and words[0] == "copy":
                world, jp, size = (int(w, 16) for w in words[1:4])
                out.append((world - base, jp - base, size))
    return out


def derive(world_path, jp_path, target_path):
    spec = read_header()
    base = spec["load_address"][0]
    target = load_target(target_path)
    jp = load_image(jp_path)
    world = load_image(world_path)
    if timestamp(world) != WORLD_TIMESTAMP or timestamp(jp) != JP_TIMESTAMP:
        sys.exit("error: not the World and JP XBLA releases")

    imports = {a - base + i for a in import_words(spec) for i in range(4)}
    print("matching against JP...", flush=True)
    recipe = copies(target, world, jp, match(target, jp))
    built = apply_copies(len(target), jp, recipe, True)
    for off in imports:
        built[off] = target[off]
    wrong = [i for i in range(len(target)) if built[i] != target[i]]
    pointers = pointer_bytes(target, jp, recipe, wrong)
    others = [i for i in wrong if i not in pointers]

    with open(RECIPE_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The guest image's data, copied from JP's image (scripts/guest_recipe.py):\n"
                "# copy world_address jp_address size\n")
        for w, j, n in recipe:
            f.write(f"copy 0x{base + w:08X} 0x{base + j:08X} 0x{n:X}\n")
    write_ranges(FIXUPS_FILE, "# World's pointers inside the copied data (scripts/guest_recipe.py):\n"
                 "# address size\n", base, spans(sorted(pointers)))
    write_ranges(WORLD_ONLY_FILE, "# The guest image's data JP's image doesn't hold "
                 "(scripts/guest_recipe.py):\n# address size\n", base, spans(others))
    shipped = sum(1 for b in target if b)
    print(f"{len(recipe)} copies ({sum(r[2] for r in recipe)} bytes), "
          f"{len(pointers)} pointer bytes, {len(others)} World-only bytes "
          f"of {shipped} non-zero")


def build(source, spec, world):
    """The flat image, from either release's own image (`world` fills the
    fixups and World-only ranges)."""
    base = spec["load_address"][0]
    size = PAGE_SIZE * sum(c for _, c in spec["pages"])
    stamp = timestamp(source)
    if stamp not in (WORLD_TIMESTAMP, JP_TIMESTAMP):
        raise ValueError(f"unknown release (PE timestamp 0x{stamp:08X})")
    image = apply_copies(size, source, read_recipe(base), stamp == JP_TIMESTAMP)
    for path in (FIXUPS_FILE, WORLD_ONLY_FILE):
        for off, data in read_bytes(path, base, world):
            image[off:off + len(data)] = data
    for addr, word in import_words(spec).items():
        struct.pack_into(">I", image, addr - base, word)
    return image


def check(world_path, jp_path, target_path=None):
    spec = read_header()
    world = load_world(world_path)
    from_world = build(world, spec, world)
    from_jp = build(load_image(jp_path), spec, world)
    ok = from_world == from_jp
    print(f"World and JP {'build the same image' if ok else 'differ'}")
    if target_path:
        target = load_target(target_path)
        same = from_jp == target
        ok = ok and same
        diff = sum(1 for a, b in zip(from_jp, target) if a != b) if not same else 0
        print(f"target: {'identical' if same else f'{diff} bytes differ'}")
    return 0 if ok else 1


def pack(world_path, out_path):
    spec = read_header()
    world = load_world(world_path)
    base = spec["load_address"][0]
    size = PAGE_SIZE * sum(c for _, c in spec["pages"])
    header = build_header(spec, [(size, 0)])
    recipe = read_recipe(base)
    patches = read_bytes(FIXUPS_FILE, base, world) + read_bytes(WORLD_ONLY_FILE, base, world)
    patches += [(a - base, struct.pack(">I", w)) for a, w in import_words(spec).items()]
    pool = b"".join(d for _, d in patches)
    blob = PACK_MAGIC + struct.pack("<9I", PACK_VERSION, size, base, WORLD_TIMESTAMP,
                                    JP_TIMESTAMP, len(header), len(recipe), len(patches),
                                    len(pool))
    blob += header
    blob += b"".join(struct.pack("<3I", w, j, n) for w, j, n in recipe)
    blob += b"".join(struct.pack("<2I", off, len(d)) for off, d in patches)
    blob += pool
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"{out_path}: {len(blob)} bytes, {len(recipe)} copies, {len(patches)} patches")
    return 0


def main():
    args = sys.argv[1:]
    if len(args) == 4 and args[0] == "derive":
        derive(*args[1:])
        return 0
    if len(args) in (3, 4) and args[0] == "check":
        return check(*args[1:])
    if len(args) == 3 and args[0] == "pack":
        return pack(*args[1:])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
