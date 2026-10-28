#!/usr/bin/env python3
"""The guest image's data as source.

The image's data decompiled, for reverse engineering: it is World's, so it
lives in assets/guest_data_src/ (gitignored, like everything taken from the
game) and isn't built into anything. scripts/gen-guest-image.py builds the
image from it, which is what scripts/guest_recipe.py derives the recipe the
executable ships against; `check` proves the sources reproduce the retail
data.

The data is the PlayStation game's, big endian with 32 bit pointers, in the
layouts sotn-decomp (github.com/Xeeynamo/sotn-decomp) documents. Each stage
starts with an `Overlay` header whose `tileLayers` is a list of RoomDefs, a
foreground and a background LayerDef each:

    LayerDef        { u16* layout; TileDefinition* tileDef; u32 rect;
                      u16 zPriority; u16 flags; }
    TileDefinition  { u8* gfxPage; u8* gfxIndex; u8* clut; u8* collision; }

`rect` packs left, top, right and bottom (6 bits each, from the top bit; the
compiler fills bitfields from the most significant bit) in screens of 16x16
tiles, then 8 bits of flags.

Grids: a room's tile map (`layout`, a u16 tile id per cell, row major over
the whole room) and a tile set's four arrays (a u8 per tile id, the
TileDefinition right after them) are grayscale PNGs, one pixel per cell, 16
bit for tile maps and 8 bit for tile sets. They are extracted from the user's
own default.xex into assets/guest_data/ (gitignored, like everything taken
from the game) and named after the guest address they are written at:

    assets/guest_data/tilemaps/<address>.png
    assets/guest_data/tilesets/<address>.png
    assets/guest_data/graphics/<address>.png

Graphics: GfxBanks (a stage's `gfxBanks` and others) are a kind word then
{u32 y << 16 | x; u32 width << 16 | height; u8* data} entries up to a -1.
Kind 4 entries are 4 bit images (a pixel per nibble, the low nibble first)
compressed with the PlayStation game's nibble coder (sotn-decomp's
sotn.Inflate). They are 16 color grayscale PNGs, the gray level a pixel's
color index, compressed again at build time by `deflate`, which reproduces
the retail stream byte for byte. Streams no bank points at are kept too,
128 pixels wide.

CLUTs: the color data of PAL_BULK descriptors (a stage's `cluts`: 5, then
{u32 destination; u32 colors; u16* data} entries up to a -1) are RGBA PNGs,
16 colors to a row, in assets/guest_data/cluts/, the PlayStation colors
mapped as the catalog's palettes are.

Catalog: data found by hand is listed in assets/guest_data_src/assets.txt, one
`address kind name args` per line; its images (`rawgfx`, 1, 4 or 8 bits per
pixel, 4 bit ones as 16 color grayscale PNGs) and palettes (`palette`, 16 bit
PlayStation colors as RGBA, 16 to a row, the semi-transparency bit as alpha
127) are extracted next to the grids, into images/ and palettes/.

Sprites: SpriteParts {u16 count; parts of 11 u16: flags, x, y, width,
height, clut, tileset, left, top, right, bottom} and the arrays of pointers to
them (sprite sets, 0 for no frame, the frames stored before the array) are
text, assets/guest_data_src/sprites/<array address>.txt, one file per set
holding the array and the frames it points at. `extract-sprites` finds the
sets as runs of pointers to valid SpriteParts.

Layouts: a room's entities, LayoutEntity {s16 x; s16 y; u16 id; u16 slot;
u16 params} from a {-2, -2} entry to a {-1, -1} one, are text,
assets/guest_data_src/layouts.txt, the sentinels left out but for the start one's
params (0 or 1). `extract-layouts` finds them by the sentinels.

Stages: the structures an Overlay header leads to (its RoomHeaders, RoomDefs,
LayerDefs, TileDefinitions, GfxBanks, CLUT descriptors and the pointer arrays
listing them) are text, assets/guest_data_src/stages.txt, pointers as
addresses.

Strings: NUL terminated text, Shift-JIS, `address<TAB>text` lines in
assets/guest_data_src/strings.txt.

Tables: whatever nonzero data no other source holds is kept as words,
assets/guest_data_src/tables.txt (ES style): `@address` blocks of ints, floats, 0x
hex and hi:lo halfword pairs, with x-prefixed hex for the bytes before the
first and after the last whole word. `extract-tables` writes it from what the
other sources leave out, so it runs after them.

XDBF: the title's XDBF (0x831E0000, which the XEX resource header points
the SDK at for achievements, strings and the XLAST) is text,
assets/guest_data_src/xdbf.txt: its entry and free tables, metadata sections as
words and string tables as `id<TAB>text`. Its PNG images and the XLAST's gzip
stream (a .NET GZipStream, which zlib can't reproduce) are extracted into
assets/guest_data/xdbf/.

Copies: on stage load, sub_8225FF58 memcpy()s each of the stage's tile maps
from .rdata over its .data buffer, whose image contents are the same bytes.
assets/guest_data_src/copies.txt lists them, `destination<TAB>source<TAB>size`, the
.rdata block being written from the tile map it copies.

    python scripts/guest_data.py extract assets/default.xex
    python scripts/guest_data.py check assets/default.xex
    python scripts/guest_data.py extract-copies assets/default.xex   # one time
    python scripts/guest_data.py extract-sprites assets/default.xex  # one time
    python scripts/guest_data.py extract-layouts assets/default.xex  # one time
    python scripts/guest_data.py extract-strings assets/default.xex  # one time
    python scripts/guest_data.py extract-stages assets/default.xex   # one time
    python scripts/guest_data.py extract-xdbf assets/default.xex     # one time
    python scripts/guest_data.py extract-tables assets/default.xex   # after the others
"""
import argparse
import os
import re
import struct
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "ext", "KryptonRecomp"))
from xex_image import XexImage  # noqa: E402

SOURCE_DIR = os.path.join(ROOT, "assets", "guest_data_src")
GRID_DIR = os.path.join(ROOT, "assets", "guest_data")
COPIES_FILE = os.path.join(SOURCE_DIR, "copies.txt")
CATALOG_FILE = os.path.join(SOURCE_DIR, "assets.txt")
SPRITES_DIR = os.path.join(SOURCE_DIR, "sprites")
SPRITES_HEADER = ("# Sprite set (see scripts/guest_data.py): `@address frames` is the array of\n"
                  "# SpriteParts pointers, each `@address` after it a SpriteParts, one part per\n"
                  "# line: flags x y width height clut tileset left top right bottom.")

# .rdata and .data, as (start, end) guest addresses.
DATA_RANGES = [(0x82000400, 0x822178B0), (0x82880000, 0x831DA5EF)]
# Everything in the image the sources must hold: the data sections, the PE
# header, .XBMOVIE and the XDBF.
SOURCE_RANGES = [(0x82000000, 0x82000400)] + DATA_RANGES + [(0x831DA600, 0x831DA60C),
                                                             (0x831E0000, 0x83206C54)]

# Overlay header words every stage shares: Update, UpdateRoomPosition and
# InitRoomEntities (HitDetection differs in one).
OVERLAY_SIGNATURE = {0x00: 0x8231DBB0, 0x08: 0x822F1A08, 0x0C: 0x822F1880}
OVERLAY_TILE_LAYERS = 0x20
# A stage's LayerDefs lie just below its RoomDef list.
LAYER_REACH = 0x80000
LAYER_DEF_SIZE = 0x10

# The stage loader and the memcpy it calls.
STAGE_LOADER = 0x8225FF58
MEMCPY = 0x825E3300

GFX_BANK_COMPRESSED = 4
GFX_BANK_END = 0xFFFFFFFF

TILEMAP_KIND = "tilemaps"
TILESET_KIND = "tilesets"
GRAPHICS_KIND = "graphics"
CLUT_KIND = "cluts"
GRID_KINDS = (TILEMAP_KIND, TILESET_KIND, GRAPHICS_KIND, CLUT_KIND)

# A stage's `cluts` descriptors: PAL_BULK, {destination, colors, data}
# entries, -1.
PAL_BULK = 5
PAL_TERMINATE = 0xFFFFFFFF
PAL_MAX_COLORS = 0x2000
CLUT_ROW = 16

RAWGFX_KIND = "rawgfx"
PALETTE_KIND = "palette"
CATALOG_DIRS = {RAWGFX_KIND: "images", PALETTE_KIND: "palettes"}


class GuestImage:
    """Big endian reads over the decrypted image."""

    def __init__(self, data, base):
        self.data = data
        self.base = base

    def u32(self, addr):
        return struct.unpack_from(">I", self.data, addr - self.base)[0]

    def read(self, addr, size):
        return bytes(self.data[addr - self.base:addr - self.base + size])


def load(path):
    xex = XexImage.load(path)
    return GuestImage(xex.data, xex.base)


def in_data(addr):
    return any(start <= addr < end for start, end in DATA_RANGES)


def overlays(img):
    for start, end in DATA_RANGES:
        for addr in range(start, end - 0x40, 4):
            if all(img.u32(addr + off) == word for off, word in OVERLAY_SIGNATURE.items()):
                yield addr


def layer_defs(img):
    """Every LayerDef the stages' RoomDef lists point at."""
    found = set()
    for overlay in overlays(img):
        rooms = img.u32(overlay + OVERLAY_TILE_LAYERS)
        addr = rooms
        while all(is_layer(img, rooms, layer) for layer in (img.u32(addr), img.u32(addr + 4))):
            found.update((img.u32(addr), img.u32(addr + 4)))
            addr += 8
    return sorted(found)


def is_layer(img, rooms, layer):
    """Below the RoomDef list, or the empty layer in .data's zero tail. The
    list ends where neither holds."""
    return (rooms - LAYER_REACH <= layer < rooms
            or in_data(layer) and not any(img.read(layer, LAYER_DEF_SIZE)))


def rect_size(rect):
    """(width, height) in tiles of a LayerDef's packed rect."""
    left, top, right, bottom = rect >> 26, (rect >> 20) & 63, (rect >> 14) & 63, (rect >> 8) & 63
    return (right - left + 1) * 16, (bottom - top + 1) * 16


def find_grids(img):
    """{(kind, address): (width, height)}."""
    grids = {}
    for layer in layer_defs(img):
        layout, tile_def, rect = img.u32(layer), img.u32(layer + 4), img.u32(layer + 8)
        if in_data(layout):
            w, h = rect_size(rect)
            old = grids.get((TILEMAP_KIND, layout), (0, 0))
            grids[(TILEMAP_KIND, layout)] = max(old, (w, h), key=lambda s: s[0] * s[1])
        if in_data(tile_def):
            arrays = [img.u32(tile_def + 4 * i) for i in range(4)]
            size = tile_def - arrays[3]
            for array in arrays:
                if in_data(array) and any(img.read(array, size)):
                    grids[(TILESET_KIND, array)] = (64, size // 64) if size >= 64 else (size, 1)
    for data, w, h in compressed_graphics(img):
        grids[(GRAPHICS_KIND, data)] = (w, h)
    for data, w, h in loose_compressed_graphics(img, grids):
        grids[(GRAPHICS_KIND, data)] = (w, h)
    for data, colors in palettes(img, grids):
        grids[(CLUT_KIND, data)] = (CLUT_ROW, colors // CLUT_ROW)
    return grids


def clut_entries(img, addr):
    """A PAL_BULK descriptor's (destination, colors, data), or None."""
    if img.u32(addr) != PAL_BULK:
        return None
    out = []
    addr += 4
    while img.u32(addr) != PAL_TERMINATE:
        dest, colors, data = img.u32(addr), img.u32(addr + 4), img.u32(addr + 8)
        if not (in_data(data) and 0 < colors <= PAL_MAX_COLORS):
            return None
        out.append((dest, colors, data))
        addr += 12
    return out


def palettes(img, grids):
    """The color data PAL_BULK descriptors point at, as (address, colors),
    overlapping ranges merged, clear of `grids`."""
    ranges = set()
    for start, end in DATA_RANGES:
        for addr in range((start + 3) & ~3, end - 16, 4):
            for _, colors, data in clut_entries(img, addr) or ():
                ranges.add((data, data + 2 * colors))
    merged = []
    for start, end in sorted(ranges):
        if merged and start < merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    taken = sorted((a, a + len(grid_bytes(img, kind, a, w, h))) for (kind, a), (w, h) in grids.items())
    out = []
    for start, end in merged:
        colors = (end - start) // 2
        if start & 1 or colors % CLUT_ROW or any(a < end and start < e for a, e in taken):
            continue
        out.append((start, colors))
    return out


LOOSE_WIDTH = 128


def loose_compressed_graphics(img, grids):
    """Compressed images nothing points at, between the other grids: a
    dictionary of valid entries, then a stream of whole 128 pixel rows that
    `deflate` reproduces exactly."""
    claimed = bytearray(len(img.data))
    for (kind, address), (w, h) in grids.items():
        size = len(grid_bytes(img, kind, address, w, h))
        claimed[address - img.base:address - img.base + size] = b"\1" * size
    view = memoryview(img.data)
    found = []
    for start, end in DATA_RANGES:
        addr = (start + 3) & ~3
        while addr < end - CMP_DICTIONARY:
            off = addr - img.base
            if claimed[off] or not all(b >> 4 in (1, 2, 6) for b in img.data[off:off + CMP_DICTIONARY]):
                addr += 4
                continue
            try:
                pixels, size = inflate(view[off:])
            except (IndexError, KeyError):
                pixels, size = (), 0
            if (pixels and len(pixels) % (2 * LOOSE_WIDTH) == 0 and not any(claimed[off:off + size])
                    and deflate(pixels) == img.read(addr, size)):
                found.append((addr, LOOSE_WIDTH, len(pixels) // LOOSE_WIDTH))
                addr = (addr + size + 3) & ~3
            else:
                addr += 4
    return found


def compressed_graphics(img):
    """(data, width, height) of the entries of every compressed GfxBank: the
    kind word, then entries up to the end marker, each kept if its stream
    inflates to exactly width x height pixels. Not every bank hangs off a
    stage's `gfxBanks`, so they are searched for."""
    found = set()
    view = memoryview(img.data)
    for start, end in DATA_RANGES:
        for bank in range((start + 3) & ~3, end - 16, 4):
            if img.u32(bank) != GFX_BANK_COMPRESSED:
                continue
            entry = bank + 4
            while img.u32(entry) != GFX_BANK_END:
                size, data = img.u32(entry + 4), img.u32(entry + 8)
                w, h = size >> 16, size & 0xFFFF
                if not in_data(data) or not 0 < w * h <= 0x40000:
                    break
                try:
                    pixels, _ = inflate(view[data - img.base:])
                except (IndexError, KeyError):
                    pixels = ()
                if len(pixels) == w * h:
                    found.add((data, w, h))
                entry += 12
    return sorted(found)


CMP_DICTIONARY = 8
CMP_LONG_ZEROS, CMP_SHORT_ZEROS, CMP_RUN = 0x13, 3, 3
CMP_SINGLE, CMP_PAIR, CMP_ZEROS = 0x10, 0x20, 0x60
CMP_LITERALS = {1: 1, 3: 2, 4: 3}
CMP_END = 15


def inflate(data):
    """(nibbles, bytes taken) of the compressed stream at the start of
    `data`."""
    dictionary = data[:CMP_DICTIONARY]
    pos = CMP_DICTIONARY * 2
    out = []

    def nibble():
        nonlocal pos
        v = data[pos >> 1] >> 4 if pos & 1 == 0 else data[pos >> 1] & 15
        pos += 1
        return v

    while True:
        op = nibble()
        if op == 0:
            out += [0] * ((nibble() << 4) + nibble() + CMP_LONG_ZEROS)
        elif op in CMP_LITERALS:
            out += [nibble() for _ in range(CMP_LITERALS[op])]
        elif op == 2:
            out += [nibble()] * 2
        elif op == 5:
            v = nibble()
            out += [v] * (nibble() + CMP_RUN)
        elif op == 6:
            out += [0] * (nibble() + CMP_SHORT_ZEROS)
        elif op == CMP_END:
            return out, (pos + 1) >> 1
        else:
            entry = dictionary[op - 7]
            v = entry & 15
            out += {CMP_SINGLE: [v], CMP_PAIR: [v, v],
                    CMP_ZEROS: [0] * (v + CMP_SHORT_ZEROS)}[entry & 0xF0]


def cmp_tokens(nibbles, dictionary):
    """The retail coder's choices, as (op, operand nibbles...): the longest
    zero run (a dictionary entry when one has its exact length), a run of 3
    or more of one nibble, a pair (from the dictionary if it has it), else
    literals in threes, a lone one from the dictionary if it has it."""
    singles = {e & 15: i for i, e in enumerate(dictionary) if e & 0xF0 == CMP_SINGLE}
    pairs = {e & 15: i for i, e in enumerate(dictionary) if e & 0xF0 == CMP_PAIR}
    zeros = {(e & 15) + CMP_SHORT_ZEROS: i for i, e in enumerate(dictionary)
             if e & 0xF0 == CMP_ZEROS}
    tokens = []
    literals = []

    def flush():
        while literals:
            if len(literals) == 1 and literals[0] in singles:
                tokens.append((7 + singles[literals.pop()],))
                continue
            chunk = literals[:3]
            del literals[:3]
            tokens.append(({1: 1, 2: 3, 3: 4}[len(chunk)], *chunk))

    i = 0
    while i < len(nibbles):
        v = nibbles[i]
        run = 1
        while i + run < len(nibbles) and nibbles[i + run] == v:
            run += 1
        if v == 0 and run >= CMP_LONG_ZEROS:
            run = min(run, CMP_LONG_ZEROS + 0xFF)
            flush()
            tokens.append((0, (run - CMP_LONG_ZEROS) >> 4, (run - CMP_LONG_ZEROS) & 15))
        elif v == 0 and run in zeros:
            flush()
            tokens.append((7 + zeros[run],))
        elif run >= CMP_RUN:
            run = min(run, CMP_RUN + 15)
            flush()
            tokens.append((6, run - CMP_SHORT_ZEROS) if v == 0 else (5, v, run - CMP_RUN))
        elif run == 2:
            flush()
            tokens.append((7 + pairs[v],) if v in pairs else (2, v))
        else:
            literals.append(v)
            run = 1
            if len(literals) == 3:
                flush()
        i += run
    flush()
    return tokens


def deflate(nibbles):
    """The retail compressed stream of `nibbles`. Its dictionary is the eight
    lone nibbles, pairs and short zero runs a pass without one uses most, by
    count and then by value."""
    counts = {}
    for token in cmp_tokens(nibbles, b""):
        kind = {1: CMP_SINGLE, 2: CMP_PAIR, 6: CMP_ZEROS}.get(token[0])
        if kind is not None:
            counts[kind | token[-1]] = counts.get(kind | token[-1], 0) + 1
    dictionary = bytes(sorted(counts, key=lambda e: (-counts[e], e))[:CMP_DICTIONARY])
    dictionary += bytes(CMP_DICTIONARY - len(dictionary))
    stream = [n for token in cmp_tokens(nibbles, dictionary) for n in token] + [CMP_END]
    stream += [0] * (len(stream) & 1)
    return dictionary + bytes(stream[i] << 4 | stream[i + 1] for i in range(0, len(stream), 2))


def grid_path(kind, address):
    return os.path.join(GRID_DIR, kind, f"{address:08X}.png")


def read_grids():
    """[(kind, address, bytes)] from assets/guest_data/."""
    out = []
    for kind in GRID_KINDS:
        folder = os.path.join(GRID_DIR, kind)
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".png"):
                continue
            with Image.open(os.path.join(folder, name)) as im:
                if kind == TILEMAP_KIND:
                    raw = (im if im.mode == "I;16" else im.convert("I;16")).tobytes()
                    data = struct.pack(f">{len(raw) // 2}H", *struct.unpack(f"<{len(raw) // 2}H", raw))
                elif kind == TILESET_KIND:
                    data = (im if im.mode == "L" else im.convert("L")).tobytes()
                elif kind == CLUT_KIND:
                    raw = im.convert("RGBA").tobytes()
                    colors = [psx_color(raw[i:i + 4]) for i in range(0, len(raw), 4)]
                    data = struct.pack(f">{len(colors)}H", *colors)
                else:
                    data = deflate(color_indices(im))
            out.append((kind, int(name[:-4], 16), data))
    return out


GRAY16 = [i * 17 for i in range(16) for _ in range(3)]


def color_indices(im):
    """A 16 color PNG's pixels: palette indices, or gray levels / 17 for an
    image an editor saved as grayscale."""
    if im.mode == "P":
        return list(im.tobytes())
    return [(v + 8) // 17 for v in im.convert("L").tobytes()]


def write_grid(path, kind, width, height, data):
    if kind == TILEMAP_KIND:
        im = Image.new("I;16", (width, height))
        im.putdata(struct.unpack(f">{width * height}H", data))
    elif kind == TILESET_KIND:
        im = Image.frombytes("L", (width, height), data)
    elif kind == CLUT_KIND:
        im = Image.new("RGBA", (width, height))
        im.putdata([rgba(c) for c in struct.unpack(f">{width * height}H", data)])
    else:
        im = Image.frombytes("P", (width, height), bytes(inflate(data)[0]))
        im.putpalette(GRAY16)
    im.save(path, optimize=True)


def grid_bytes(img, kind, address, width, height):
    """The retail bytes of a grid."""
    if kind == TILEMAP_KIND:
        return img.read(address, width * height * 2)
    if kind == TILESET_KIND:
        return img.read(address, width * height)
    if kind == CLUT_KIND:
        return img.read(address, width * height * 2)
    return img.read(address, inflate(memoryview(img.data)[address - img.base:])[1])


def overlay_grids(image, base):
    for _, address, data in read_grids():
        off = address - base
        image[off:off + len(data)] = data


def read_catalog():
    """[(address, kind, name, [args])] from assets/guest_data_src/assets.txt."""
    out = []
    with open(CATALOG_FILE, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            line = line.split("#")[0].split()
            if line:
                out.append((int(line[0], 16), line[1], line[2], [int(v, 0) for v in line[3:]]))
    return out


def asset_size(kind, args):
    if kind == RAWGFX_KIND:
        width, height, bpp = args
        return width * height * bpp // 8
    return args[0] * 2


def asset_path(kind, name):
    return os.path.join(GRID_DIR, CATALOG_DIRS[kind], name + ".png")


def write_asset(kind, args, name, data):
    if kind == RAWGFX_KIND:
        width, height, bpp = args
        if bpp == 4:
            im = Image.frombytes("P", (width, height), bytes(v for b in data for v in (b & 15, b >> 4)))
            im.putpalette(GRAY16)
        else:
            im = Image.frombytes({1: "1", 8: "L"}[bpp], (width, height), data)
    else:
        im = Image.new("RGBA", (16, (args[0] + 15) // 16))
        im.putdata([rgba(c) for c in struct.unpack(f">{args[0]}H", data)])
    im.save(asset_path(kind, name), optimize=True)


def read_asset(kind, args, name):
    with Image.open(asset_path(kind, name)) as im:
        if kind == RAWGFX_KIND:
            bpp = args[2]
            if bpp == 4:
                pixels = color_indices(im)
                return bytes(pixels[i] | pixels[i + 1] << 4 for i in range(0, len(pixels), 2))
            return im.convert({1: "1", 8: "L"}[bpp]).tobytes()
        raw = im.convert("RGBA").tobytes()
        colors = [psx_color(raw[i:i + 4]) for i in range(0, 4 * args[0], 4)]
        return struct.pack(f">{len(colors)}H", *colors)


def rgba(color):
    """A PlayStation color: 5 bits each of red, green and blue, then the
    semi-transparency bit, which is alpha 127 (sotn-decomp's mapping)."""
    r, g, b = (((color >> s) & 31) * 255 // 31 for s in (0, 5, 10))
    return r, g, b, 127 if color & 0x8000 else 255


def psx_color(rgba_):
    r, g, b, a = rgba_
    return (round(r * 31 / 255) | round(g * 31 / 255) << 5 | round(b * 31 / 255) << 10
            | (0x8000 if a < 255 else 0))


def overlay_catalog(image, base):
    for address, kind, name, args in read_catalog():
        data = read_asset(kind, args, name)
        image[address - base:address - base + len(data)] = data


SPRITE_PART = ">11H"
SPRITE_PART_SIZE = 22
SPRITE_MAX_PARTS = 64


def sprite_size(img, frame):
    return 2 + SPRITE_PART_SIZE * struct.unpack_from(">H", img.data, frame - img.base)[0]


def is_sprite(img, frame):
    """A SpriteParts: a part count, then that many parts whose sizes and
    texture rectangles fit a 256x256 page."""
    if not in_data(frame) or frame & 1:
        return False
    count, = struct.unpack_from(">H", img.data, frame - img.base)
    if not 0 < count <= SPRITE_MAX_PARTS:
        return False
    for i in range(count):
        flags, _, _, w, h, _, _, left, top, right, bottom = struct.unpack_from(
            SPRITE_PART, img.data, frame - img.base + 2 + SPRITE_PART_SIZE * i)
        if not (1 <= w <= 0x100 and 1 <= h <= 0x100 and left <= right <= 0x100
                and top <= bottom <= 0x100 and flags < 0x400):
            return False
    return True


def find_sprites(img, claimed):
    """Sprite sets: arrays of SpriteParts pointers (0 for no frame). They are
    runs of two or more pointers to SpriteParts, zero words allowed between,
    starting at a referenced zero word before the first pointer if there is
    one. An array whose frames overlap one another or `claimed` (a byte map
    of what other sources hold) is not one. Returns {array: [words]}."""
    pointers = {}
    for start, end in DATA_RANGES:
        for addr in range((start + 3) & ~3, end - 4, 4):
            v = img.u32(addr)
            if in_data(v):
                pointers[addr] = v
    referenced = set(pointers.values())
    valid = {}

    def sprite(v):
        if v not in valid:
            valid[v] = is_sprite(img, v)
        return valid[v]

    runs = []
    run = []

    def end_run():
        if len(run) >= 2:
            runs.append([(a, w) for a, w in run])
        run.clear()

    for addr in sorted(pointers):
        if not sprite(pointers[addr]):
            end_run()
            continue
        if run and (addr - run[-1][0] > 16 or any(img.u32(a) for a in range(run[-1][0] + 4, addr, 4))):
            end_run()
        if run:
            run += [(a, 0) for a in range(run[-1][0] + 4, addr, 4)]
        run.append((addr, pointers[addr]))
    end_run()

    while True:
        spans = sorted(((f, f + sprite_size(img, f)), i)
                       for i, run in enumerate(runs) for f in {w for _, w in run if w})
        bad = {i for (s, e), i in spans if any(claimed[s - img.base:e - img.base])}
        for ((s1, e1), i), ((s2, _), j) in zip(spans, spans[1:]):
            if s2 < e1:
                bad.update((i, j))
        if not bad:
            break
        runs = [run for i, run in enumerate(runs) if i not in bad]

    arrays = {}
    for run in runs:
        start = run[0][0]
        if not img.u32(start - 4) and start - 4 in referenced:
            run.insert(0, (start - 4, 0))
        arrays[run[0][0]] = [w for _, w in run]
    return arrays


def write_sprites(img, arrays):
    """One file per array in assets/guest_data_src/sprites/: the array, then the
    frames it points at."""
    os.makedirs(SPRITES_DIR, exist_ok=True)
    for array, words in sorted(arrays.items()):
        lines = [SPRITES_HEADER, f"@0x{array:08X} frames"]
        tokens = ["0" if not w else f"0x{w:08X}" for w in words]
        lines += [" ".join(tokens[i:i + 8]) for i in range(0, len(tokens), 8)]
        for frame in sorted({w for w in words if w}):
            lines.append(f"@0x{frame:08X}")
            count, = struct.unpack_from(">H", img.data, frame - img.base)
            for i in range(count):
                part = struct.unpack_from(SPRITE_PART, img.data, frame - img.base + 2 + SPRITE_PART_SIZE * i)
                lines.append(" ".join([f"0x{part[0]:X}", *(str(v - 0x10000 if v & 0x8000 and k in (1, 2) else v)
                                                          for k, v in enumerate(part[1:], 1))]))
        with open(os.path.join(SPRITES_DIR, f"{array:08X}.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")


def read_sprites():
    """[(address, bytes)]: frame arrays and SpriteParts."""
    out = []
    if not os.path.isdir(SPRITES_DIR):
        return out
    for name in sorted(os.listdir(SPRITES_DIR)):
        block = None
        with open(os.path.join(SPRITES_DIR, name), encoding="utf-8") as f:
            for n, line in enumerate(f.read().split("\n"), 1):
                tokens = line.split("#")[0].split()
                if not tokens:
                    continue
                if tokens[0].startswith("@"):
                    block = [int(tokens[0][1:], 16), tokens[1:] == ["frames"], bytearray()]
                    out.append(block)
                elif block[1]:
                    block[2] += b"".join(struct.pack(">I", int(t, 0)) for t in tokens)
                else:
                    if len(tokens) != 11:
                        raise ValueError(f"{name}:{n}: a sprite part has 11 fields")
                    block[2] += struct.pack(SPRITE_PART, *(int(t, 0) & 0xFFFF for t in tokens))
    result = []
    for address, is_array, data in out:
        if not is_array:
            data = struct.pack(">H", len(data) // SPRITE_PART_SIZE) + data
        result.append((address, bytes(data)))
    return result


def overlay_sprites(image, base):
    for address, data in read_sprites():
        image[address - base:address - base + len(data)] = data


LAYOUT_ENTITY = ">hhHHH"
LAYOUT_ENTITY_SIZE = 10
# The start sentinel's params vary (0 or 1).
LAYOUT_START = struct.pack(">hhHH", -2, -2, 0, 0)
LAYOUT_END = struct.pack(LAYOUT_ENTITY, -1, -1, 0, 0, 0)
LAYOUT_MAX_ENTITIES = 1000
LAYOUTS_FILE = os.path.join(SOURCE_DIR, "layouts.txt")


def find_layouts(img, claimed):
    """Entity layouts: word aligned runs of LayoutEntity between the -2 and
    -1 sentinel entries, clear of `claimed`. Returns {address: (start
    sentinel params, [entities])}."""
    out = {}
    for start, end in DATA_RANGES:
        off = start - img.base
        while True:
            off = img.data.find(LAYOUT_START, off, end - img.base)
            if off < 0:
                break
            if off & 3:
                off += 1
                continue
            entities = []
            cur = off + LAYOUT_ENTITY_SIZE
            while len(entities) < LAYOUT_MAX_ENTITIES and cur + LAYOUT_ENTITY_SIZE <= end - img.base:
                entry = bytes(img.data[cur:cur + LAYOUT_ENTITY_SIZE])
                if entry == LAYOUT_END:
                    break
                entities.append(struct.unpack(LAYOUT_ENTITY, entry))
                cur += LAYOUT_ENTITY_SIZE
            else:
                off += 4
                continue
            cur += LAYOUT_ENTITY_SIZE
            if any(claimed[off:cur]):
                off += 4
                continue
            out[img.base + off] = struct.unpack_from(">H", img.data, off + 8)[0], entities
            off = cur
    return out


def write_layouts(layouts):
    lines = ["# Entity layouts (see scripts/guest_data.py): `@address [start params]`,",
             "# then one LayoutEntity per line: x y id slot params. The -2 and -1",
             "# sentinel entries around each list are implicit."]
    for address, (params, entities) in sorted(layouts.items()):
        lines.append(f"@0x{address:08X}" + (f" 0x{params:X}" if params else ""))
        lines += [f"{x} {y} 0x{i:X} 0x{slot:X} 0x{params:X}" for x, y, i, slot, params in entities]
    with open(LAYOUTS_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def read_layouts():
    """[(address, bytes)], sentinels included."""
    out = []
    if not os.path.exists(LAYOUTS_FILE):
        return out
    with open(LAYOUTS_FILE, encoding="utf-8") as f:
        for n, line in enumerate(f.read().split("\n"), 1):
            tokens = line.split("#")[0].split()
            if not tokens:
                continue
            if tokens[0].startswith("@"):
                params = int(tokens[1], 0) if len(tokens) > 1 else 0
                out.append([int(tokens[0][1:], 16), bytearray(LAYOUT_START + struct.pack(">H", params))])
                continue
            if len(tokens) != 5:
                raise ValueError(f"layouts.txt:{n}: an entity has 5 fields")
            x, y, *rest = (int(t, 0) for t in tokens)
            out[-1][1] += struct.pack(LAYOUT_ENTITY, x, y, *rest)
    return [(address, bytes(data + LAYOUT_END)) for address, data in out]


def overlay_layouts(image, base):
    for address, data in read_layouts():
        image[address - base:address - base + len(data)] = data


STRINGS_FILE = os.path.join(SOURCE_DIR, "strings.txt")
STRING_CODEC = "cp932"
ESCAPES = {"\\": "\\\\", "\t": "\\t", "\n": "\\n", "\r": "\\r"}
UNESCAPES = {"\\": "\\", "t": "\t", "n": "\n", "r": "\r"}
JAPANESE = (("　", "ヿ"), ("一", "鿿"), ("！", "～"))


def is_text(raw):
    try:
        text = raw.decode(STRING_CODEC)
    except UnicodeDecodeError:
        return False
    return text.encode(STRING_CODEC) == raw and all(
        " " <= c < "\x7f" or c in "\t\n\r" or any(lo <= c <= hi for lo, hi in JAPANESE)
        for c in text)


def find_strings(img, claimed):
    """The image's narrow strings, guessed: 4 aligned after a zero byte, NUL
    terminated and zero padded to the next 4 bytes, at least 4 bytes of
    printable ASCII or Shift-JIS Japanese, clear of `claimed`. Shorter ones
    can't be told from other data. Returns [(address, bytes)]."""
    data = img.data
    found = []
    for start, end in DATA_RANGES:
        i, end = start - img.base, end - img.base
        while i < end:
            if i % 4 == 0 and data[i - 1] == 0 and data[i] and not claimed[i]:
                j = data.find(b"\0", i, end)
                raw = bytes(data[i:j])
                if (j >= 0 and j - i >= 4 and not any(claimed[i:(j + 4) & ~3])
                        and not any(data[j:(j + 4) & ~3]) and is_text(raw)):
                    found.append((img.base + i, raw))
                    i = j
                    continue
            i += 1
    return found


def escape(raw):
    return "".join(ESCAPES.get(c) or c for c in raw.decode(STRING_CODEC))


def unescape(text):
    out = bytearray()
    i = 0
    while i < len(text):
        if text[i] == "\\":
            out += UNESCAPES[text[i + 1]].encode(STRING_CODEC)
            i += 2
        else:
            out += text[i].encode(STRING_CODEC)
            i += 1
    return bytes(out)


def write_strings(found):
    with open(STRINGS_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("# NUL terminated strings, `address<TAB>text` (\\\\, \\t, \\n and \\r escaped).\n")
        f.writelines(f"0x{address:08X}\t{escape(raw)}\n" for address, raw in found)


def read_strings():
    """[(address, bytes)], the terminator included."""
    out = []
    if not os.path.exists(STRINGS_FILE):
        return out
    with open(STRINGS_FILE, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            if line and not line.startswith("#"):
                address, _, text = line.partition("\t")
                out.append((int(address, 16), unescape(text) + b"\0"))
    return out


def overlay_strings(image, base):
    for address, data in read_strings():
        image[address - base:address - base + len(data)] = data


STAGES_FILE = os.path.join(SOURCE_DIR, "stages.txt")
OVERLAY_WORDS = 11
OVERLAY_ROOMS, OVERLAY_SPRITE_BANKS, OVERLAY_CLUTS = 0x10, 0x14, 0x18
OVERLAY_LAYOUTS, OVERLAY_GFX_BANKS = 0x1C, 0x24
ROOM_HEADER = ">8B"
ROOMS_END = 0x40
MAX_ROOMS = 0x200
GFX_BANK_ENTRY = ">HHHHI"
MAX_GFX_BANK_ENTRIES = 0x100


def pointer_run(img, addr, nulls=False):
    """The words from `addr` on that point into the data (or are 0 if
    `nulls`), trailing zeros dropped."""
    words = []
    while in_data(img.u32(addr + 4 * len(words))) or nulls and img.u32(addr + 4 * len(words)) == 0:
        words.append(img.u32(addr + 4 * len(words)))
    while words and not words[-1]:
        words.pop()
    return words


def gfx_bank(img, addr):
    """A GfxBank's kind and entries, or None if it runs on."""
    kind = img.u32(addr)
    if kind == GFX_BANK_END:
        return kind, []
    entries = []
    entry = addr + 4
    while img.u32(entry) != GFX_BANK_END:
        if len(entries) == MAX_GFX_BANK_ENTRIES:
            return None
        entries.append(struct.unpack_from(GFX_BANK_ENTRY, img.data, entry - img.base))
        entry += 12
    return kind, entries


def find_stages(img, claimed):
    """The structures each stage's Overlay header leads to, as
    [(stage, [(address, kind, args, rows)])]: the header, its RoomHeaders,
    RoomDefs, LayerDefs and TileDefinitions, the GfxBanks, the CLUT
    descriptors and the pointer arrays that list them. A structure clear of
    `claimed` and of the others is kept the first time a stage reaches it;
    a pointer array is cut short where another structure starts."""
    stages = []
    seen = set()
    for overlay in overlays(img):
        blocks = []

        def add(addr, kind, args, rows):
            if addr not in seen and in_data(addr):
                seen.add(addr)
                blocks.append([addr, kind, args, rows])

        add(overlay, "overlay", [], [[img.u32(overlay + 4 * i) for i in range(OVERLAY_WORDS)]])
        rooms = img.u32(overlay + OVERLAY_ROOMS)
        if in_data(rooms):
            rows = []
            while img.data[rooms - img.base + 8 * len(rows)] != ROOMS_END and len(rows) < MAX_ROOMS:
                rows.append(list(struct.unpack_from(ROOM_HEADER, img.data, rooms - img.base + 8 * len(rows))))
            if len(rows) < MAX_ROOMS:
                add(rooms, "rooms", [], rows)
        add(img.u32(overlay + OVERLAY_SPRITE_BANKS), "pointers", [],
            [pointer_run(img, img.u32(overlay + OVERLAY_SPRITE_BANKS), nulls=True)])
        cluts = img.u32(overlay + OVERLAY_CLUTS)
        if in_data(cluts):
            add(cluts, "pointers", [], [pointer_run(img, cluts)])
            for desc in pointer_run(img, cluts):
                entries = clut_entries(img, desc)
                if entries is not None:
                    add(desc, "clut", [], [list(e) for e in entries])
        layouts = img.u32(overlay + OVERLAY_LAYOUTS)
        if in_data(layouts):
            add(layouts, "pointers", [], [pointer_run(img, layouts)])
        room_defs = img.u32(overlay + OVERLAY_TILE_LAYERS)
        if in_data(room_defs):
            rows = []
            addr = room_defs
            while all(is_layer(img, room_defs, layer) for layer in (img.u32(addr), img.u32(addr + 4))):
                rows.append([img.u32(addr), img.u32(addr + 4)])
                addr += 8
            add(room_defs, "roomdefs", [], rows)
            for layer in sorted({v for row in rows for v in row}):
                if any(img.read(layer, LAYER_DEF_SIZE)):
                    add(layer, "layer", [], [list(struct.unpack_from(">IIIHH", img.data, layer - img.base))])
                    tile_def = img.u32(layer + 4)
                    if in_data(tile_def):
                        add(tile_def, "tiledef", [], [[img.u32(tile_def + 4 * i) for i in range(4)]])
        banks = img.u32(overlay + OVERLAY_GFX_BANKS)
        if in_data(banks):
            add(banks, "pointers", [], [pointer_run(img, banks)])
            for bank in pointer_run(img, banks):
                found = gfx_bank(img, bank)
                if found:
                    add(bank, "gfxbank", [found[0]], [list(e) for e in found[1]])
        stages.append((overlay, blocks))

    # Cut pointer arrays at the next structure, drop what collides.
    spans = sorted((b[0], b[0] + len(stage_block_bytes(b[1], b[2], b[3])), id(b))
                   for _, blocks in stages for b in blocks)
    starts = [start for start, _, _ in spans]
    for _, blocks in stages:
        for b in blocks:
            if b[1] == "pointers":
                limit = min([a for a in starts if a > b[0]] + [b[0] + 4 * len(b[3][0])])
                b[3] = [b[3][0][:(limit - b[0]) // 4]]
    taken = bytearray(len(img.data))
    out = []
    for stage, blocks in stages:
        kept = []
        for b in blocks:
            data = stage_block_bytes(b[1], b[2], b[3])
            off = b[0] - img.base
            if not data or any(claimed[off:off + len(data)]) or any(taken[off:off + len(data)]):
                continue
            taken[off:off + len(data)] = b"\1" * len(data)
            kept.append(tuple(b))
        out.append((stage, kept))
    return out


def stage_block_bytes(kind, args, rows):
    if kind == "rooms":
        return b"".join(struct.pack(ROOM_HEADER, *row) for row in rows) + bytes([ROOMS_END])
    if kind == "layer":
        return b"".join(struct.pack(">IIIHH", *row) for row in rows)
    if kind == "clut":
        return (struct.pack(">I", PAL_BULK) + b"".join(struct.pack(">III", *row) for row in rows)
                + struct.pack(">I", PAL_TERMINATE))
    if kind == "gfxbank":
        if args[0] == GFX_BANK_END:
            return struct.pack(">I", GFX_BANK_END)
        return (struct.pack(">I", args[0]) + b"".join(struct.pack(GFX_BANK_ENTRY, *row) for row in rows)
                + struct.pack(">I", GFX_BANK_END))
    return b"".join(struct.pack(f">{len(row)}I", *row) for row in rows)


STAGE_ROW_FORMATS = {
    "overlay": None, "pointers": None, "roomdefs": None, "tiledef": None,
    "rooms": "{} {} {} {} {} {} {} {}",
    "layer": "0x{:08X} 0x{:08X} 0x{:08X} {} 0x{:X}",
    "clut": "0x{:X} 0x{:X} 0x{:08X}",
    "gfxbank": "{} {} {} {} 0x{:08X}",
}


def write_stages(stages):
    lines = ["# Stage structures (see scripts/guest_data.py), `@address kind [args]`",
             "# then its rows. overlay: Update HitDetection UpdateRoomPosition",
             "# InitRoomEntities rooms spriteBanks cluts layouts tileLayers gfxBanks",
             "# UpdateStageEntities. rooms: RoomHeaders, left top right bottom",
             "# tileLayoutId tilesetId objGfxId objLayoutId, the 0x40 end implicit.",
             "# roomdefs: foreground and background LayerDef. layer: layout tileDef",
             "# rect zPriority flags. tiledef: gfxPage gfxIndex clut collision.",
             "# gfxbank kind: y x width height data (the u16 pairs as stored), the -1",
             "# end implicit. clut: PAL_BULK destination colors data, -1 implicit.",
             "# pointers: words, 0 for none."]
    for stage, blocks in stages:
        lines.append(f"\n# Stage 0x{stage:08X}")
        for addr, kind, args, rows in blocks:
            lines.append(" ".join([f"@0x{addr:08X}", kind, *(str(a if a != GFX_BANK_END else -1)
                                                           for a in args)]))
            fmt = STAGE_ROW_FORMATS[kind]
            for row in rows:
                if fmt:
                    lines.append(fmt.format(*row))
                else:
                    tokens = ["0" if not w else f"0x{w:08X}" for w in row]
                    lines += [" ".join(tokens[i:i + 8]) for i in range(0, len(tokens), 8)]
    with open(STAGES_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def read_stages():
    """[(address, bytes)]."""
    out = []
    if not os.path.exists(STAGES_FILE):
        return out
    blocks = []
    with open(STAGES_FILE, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            tokens = line.split("#")[0].split()
            if not tokens:
                continue
            if tokens[0].startswith("@"):
                args = [int(t, 0) & 0xFFFFFFFF for t in tokens[2:]]
                blocks.append((int(tokens[0][1:], 16), tokens[1], args, []))
            else:
                blocks[-1][3].append([int(t, 0) for t in tokens])
    for addr, kind, args, rows in blocks:
        if STAGE_ROW_FORMATS[kind] is None:
            rows = [[w for row in rows for w in row]]
        out.append((addr, stage_block_bytes(kind, args, rows)))
    return out


def overlay_stages(image, base):
    for address, data in read_stages():
        image[address - base:address - base + len(data)] = data


def read_copies():
    """[(destination, source, size)]."""
    out = []
    with open(COPIES_FILE, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            line = line.split("#")[0].split()
            if line:
                out.append(tuple(int(v, 16) for v in line))
    return out


def overlay_copies(image, base):
    for dst, src, size in read_copies():
        image[dst - base:dst - base + size] = image[src - base:src - base + size]


TABLES_FILE = os.path.join(SOURCE_DIR, "tables.txt")
# Fewer zero bytes than this between two uncovered ones stay in one block.
TABLE_ZERO_GAP = 64
TABLE_ROW = 8
BYTES_TOKEN = re.compile(r"^x([0-9A-F]{2})+$")


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def float_token(word):
    """Shortest decimal that reads back as this float32, or None."""
    f = struct.unpack(">f", struct.pack(">I", word))[0]
    if f != f or f in (float("inf"), float("-inf")):
        return None
    for digits in range(1, 10):
        text = "%.*g" % (digits, f)
        if struct.pack(">f", float(text)) == struct.pack(">I", word):
            return text if any(c in text for c in ".e") else text + ".0"
    return None


def word_token(word, kind):
    if kind == "h":
        return "%d:%d" % (word >> 16, word & 0xFFFF)
    if word < 0x100000 or word >= 0xFFF00000:
        return str(signed(word))
    exponent = (word >> 23) & 0xFF
    if 0x60 <= exponent <= 0xA0 and float_token(word):
        return float_token(word)
    return "0x%08X" % word


def token_bytes(token):
    if BYTES_TOKEN.match(token):
        return bytes.fromhex(token[1:])
    if ":" in token:
        hi, lo = token.split(":")
        word = int(hi) << 16 | int(lo)
    elif token.startswith("0x"):
        word = int(token, 16)
    elif any(c in token for c in ".e") and not token.lstrip("-").isdigit():
        return struct.pack(">f", float(token))
    else:
        word = int(token) & 0xFFFFFFFF
    return struct.pack(">I", word)


def halfword_block(words):
    """True for tables of 16 bit pairs, which read as nonsense words."""
    used = [w for w in words if w]
    pairs = sum(0 < w >> 16 < 0x1000 and (w & 0xFFFF) < 0x1000 for w in used)
    return len(used) >= 4 and pairs * 10 >= len(used) * 6


def uncovered_blocks(img, claimed):
    """[(start, end)] of the nonzero data no other source holds, runs of
    fewer than TABLE_ZERO_GAP zero bytes kept inside a block, widened to
    whole words where that takes no claimed byte."""
    data = img.data
    blocks = []
    for start, end in SOURCE_RANGES:
        block = None
        for off in range(start - img.base, end - img.base):
            if claimed[off] or (block and off - block[1] >= TABLE_ZERO_GAP):
                if block:
                    blocks.append(block)
                block = None
            if data[off] and not claimed[off]:
                if block is None:
                    block = [off, off]
                block[1] = off + 1
        if block:
            blocks.append(block)
    out = []
    for a, b in blocks:
        # Whole words where the bytes around are free (they are zeros).
        a4, b4 = a & ~3, (b + 3) & ~3
        if not any(claimed[a4:a]):
            a = a4
        if not any(claimed[b:b4]):
            b = b4
        out.append((img.base + a, img.base + b))
    return out


def table_lines(img, start, end):
    """A block as `@address` and rows of tokens: bytes up to the first 4
    aligned address and after the last whole word, words in between."""
    data = img.read(start, end - start)
    head = -start % 4
    if head >= len(data):
        return [f"@0x{start:08X}", "x" + data.hex().upper()]
    body = (len(data) - head) // 4
    words = struct.unpack_from(f">{body}I", data, head)
    kind = "h" if halfword_block(words) else "a"
    tokens = [word_token(w, kind) for w in words]
    if head:
        tokens.insert(0, "x" + data[:head].hex().upper())
    tail = data[head + 4 * body:]
    if tail:
        tokens.append("x" + tail.hex().upper())
    lines = [f"@0x{start:08X}"]
    lines += [" ".join(tokens[i:i + TABLE_ROW]) for i in range(0, len(tokens), TABLE_ROW)]
    return lines


def write_tables(img, blocks):
    lines = ["# Guest image data no other source holds: `@address`, then words (ints,",
             "# floats, 0x hex, hi:lo halfword pairs) and x-prefixed hex bytes.",
             "# See scripts/guest_data.py.", ""]
    for start, end in blocks:
        lines += table_lines(img, start, end) + [""]
    with open(TABLES_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def read_tables():
    """[(address, bytes)]."""
    out = []
    if not os.path.exists(TABLES_FILE):
        return out
    with open(TABLES_FILE, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            line = line.split("#")[0].split()
            if not line:
                continue
            if line[0].startswith("@"):
                out.append((int(line[0][1:], 16), bytearray()))
                continue
            for token in line:
                out[-1][1].extend(token_bytes(token))
    return [(address, bytes(data)) for address, data in out]


def overlay_tables(image, base):
    for address, data in read_tables():
        image[address - base:address - base + len(data)] = data


# The title's XDBF (achievements, strings, images and the XLAST the SDK reads
# through the XEX resource header).
XDBF_FILE = os.path.join(SOURCE_DIR, "xdbf.txt")
XDBF_DIR = os.path.join(GRID_DIR, "xdbf")
XDBF_ADDRESS = 0x831E0000
XDBF_HEADER = ">4sIIIII"
XDBF_ENTRY = ">HQII"
XDBF_ENTRY_SIZE = 18
XDBF_FREE = ">II"
XSTR_MAGIC = b"XSTR"
XSRC_MAGIC = b"XSRC"
SECTION_VERSION = 1


def xdbf_path(name):
    return os.path.join(XDBF_DIR, name)


def xdbf_layout(img):
    """(version, entry slots, free slots, [(ns, id, offset, size)],
    [(offset, size)], data address) of the image's XDBF."""
    magic, version, slots, count, free_slots, free_count = struct.unpack(
        XDBF_HEADER, img.read(XDBF_ADDRESS, 24))
    assert magic == b"XDBF"
    table = XDBF_ADDRESS + 24
    entries = [struct.unpack(XDBF_ENTRY, img.read(table + XDBF_ENTRY_SIZE * i, XDBF_ENTRY_SIZE))
               for i in range(count)]
    free_table = table + XDBF_ENTRY_SIZE * slots
    free = [struct.unpack(XDBF_FREE, img.read(free_table + 8 * i, 8)) for i in range(free_count)]
    return version, slots, free_slots, entries, free, free_table + 8 * free_slots


def tokens(data):
    """Word tokens for `data`, x-prefixed hex for a last partial word."""
    whole = len(data) // 4
    out = [word_token(w, "a") for w in struct.unpack_from(f">{whole}I", data)]
    if len(data) % 4:
        out.append("x" + data[4 * whole:].hex().upper())
    return out


def xdbf_escape(text):
    return "".join(ESCAPES.get(c) or c for c in text)


def xdbf_unescape(text):
    out = []
    i = 0
    while i < len(text):
        if text[i] == "\\":
            out.append(UNESCAPES[text[i + 1]])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def write_xdbf(img):
    version, slots, free_slots, entries, free, data = xdbf_layout(img)
    lines = ["# The title's XDBF, rebuilt by scripts/guest_data.py. `xdbf address version",
             "# entry_slots free_slots`, `free offset size`, then each `entry namespace id",
             "# offset` with its contents: words, `png <file>` (extracted),",
             "# `xstr` and `id<TAB>text` lines, or `xsrc <file> <xlast name> <size>`",
             "# (the gzip stream extracted).",
             f"xdbf 0x{XDBF_ADDRESS:08X} 0x{version:X} {slots} {free_slots}"]
    lines += [f"free 0x{off:X} 0x{size:X}" for off, size in free]
    for ns, id_, off, size in entries:
        raw = img.read(data + off, size)
        head = f"entry {ns} 0x{id_:X} 0x{off:X}"
        if raw.startswith(b"\x89PNG"):
            lines.append(f"{head} png {id_:X}.png")
        elif raw.startswith(XSTR_MAGIC):
            lines.append(f"{head} xstr")
            count, = struct.unpack_from(">H", raw, 12)
            p = 14
            for _ in range(count):
                sid, n = struct.unpack_from(">HH", raw, p)
                lines.append(f"{sid}\t{xdbf_escape(raw[p + 4:p + 4 + n].decode('utf-8'))}")
                p += 4 + n
        elif raw.startswith(XSRC_MAGIC):
            n, = struct.unpack_from(">I", raw, 12)
            name = raw[16:16 + n].decode()
            plain, _ = struct.unpack_from(">II", raw, 16 + n)
            lines.append(f"{head} xsrc {name}.gz {name} {plain}")
        else:
            lines.append(head)
            row = tokens(raw)
            lines += [" ".join(row[i:i + TABLE_ROW]) for i in range(0, len(row), TABLE_ROW)]
    with open(XDBF_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def parse_xdbf():
    """(address, version, slots, free_slots, [free], [[ns, id, offset, kind,
    args, rows]])."""
    spec = None
    entries = []
    free = []
    with open(XDBF_FILE, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            if not line or line.startswith("#"):
                continue
            if entries and entries[-1][3] == "xstr" and "\t" in line:
                sid, _, text = line.partition("\t")
                entries[-1][5].append((int(sid), xdbf_unescape(text)))
                continue
            words = line.split()
            if words[0] == "xdbf":
                spec = [int(w, 0) for w in words[1:]]
            elif words[0] == "free":
                free.append((int(words[1], 0), int(words[2], 0)))
            elif words[0] == "entry":
                kind = words[4] if len(words) > 4 else "words"
                entries.append([int(words[1]), int(words[2], 0), int(words[3], 0), kind,
                                words[5:], []])
            else:
                entries[-1][5].extend(words)
    return spec[0], spec[1], spec[2], spec[3], free, entries


def xdbf_entry_bytes(kind, args, rows):
    if kind == "png":
        with open(xdbf_path(args[0]), "rb") as f:
            return f.read()
    if kind == "xstr":
        body = b"".join(struct.pack(">HH", sid, len(t.encode("utf-8"))) + t.encode("utf-8")
                        for sid, t in rows)
        return struct.pack(">4sIIH", XSTR_MAGIC, SECTION_VERSION, 10 + len(body),
                           len(rows)) + body
    if kind == "xsrc":
        with open(xdbf_path(args[0]), "rb") as f:
            stream = f.read()
        name = args[1].encode()
        body = (struct.pack(">I", len(name)) + name
                + struct.pack(">II", int(args[2]), len(stream)) + stream)
        return struct.pack(">4sII", XSRC_MAGIC, SECTION_VERSION, 8 + len(body)) + body
    return b"".join(token_bytes(t) for t in rows)


def read_xdbf():
    """[(address, bytes)]: the whole XDBF, or nothing without xdbf.txt."""
    if not os.path.exists(XDBF_FILE):
        return []
    address, version, slots, free_slots, free, entries = parse_xdbf()
    table = struct.pack(XDBF_HEADER, b"XDBF", version, slots, len(entries), free_slots,
                        len(free))
    table += b"".join(struct.pack(XDBF_ENTRY, ns, id_, off,
                                  len(xdbf_entry_bytes(kind, args, rows)))
                      for ns, id_, off, kind, args, rows in entries)
    table += bytes(XDBF_ENTRY_SIZE * (slots - len(entries)))
    table += b"".join(struct.pack(XDBF_FREE, off, size) for off, size in free)
    table += bytes(8 * (free_slots - len(free)))
    data = bytearray()
    for _, _, off, kind, args, rows in entries:
        raw = xdbf_entry_bytes(kind, args, rows)
        if len(data) < off + len(raw):
            data += bytes(off + len(raw) - len(data))
        data[off:off + len(raw)] = raw
    return [(address, table + bytes(data))]


def extract_xdbf(img):
    """Write the XDBF's images and XLAST stream into assets/guest_data/xdbf."""
    if not os.path.exists(XDBF_FILE):
        return 0
    os.makedirs(XDBF_DIR, exist_ok=True)
    *_, entries, _, data = xdbf_layout(img)
    sizes = {(ns, id_): (off, size) for ns, id_, off, size in entries}
    count = 0
    for ns, id_, _, kind, args, _ in parse_xdbf()[5]:
        off, size = sizes[ns, id_]
        raw = img.read(data + off, size)
        if kind == "xsrc":
            n, = struct.unpack_from(">I", raw, 12)
            raw = raw[16 + n + 8:]
        elif kind != "png":
            continue
        with open(xdbf_path(args[0]), "wb") as f:
            f.write(raw)
        count += 1
    return count


def overlay_xdbf(image, base):
    for address, data in read_xdbf():
        image[address - base:address - base + len(data)] = data


def function_end(img, addr):
    """End of the function at `addr`, from its .pdata entry."""
    pdata, count = img.base + 0x217A00, 0x16C78 // 8
    for i in range(count):
        begin, packed = img.u32(pdata + 8 * i), img.u32(pdata + 8 * i + 4)
        if begin == addr:
            return begin + 4 * ((packed >> 8) & 0x3FFFFF)
    raise ValueError(f"no .pdata entry for 0x{addr:08X}")


def memcpy_calls(img, start, end):
    """(r3, r4, r5) at every `bl MEMCPY` between `start` and `end`, from the
    lis/addi/ori/li that set them up."""
    regs = {}
    for addr in range(start, end, 4):
        insn = img.u32(addr)
        op, rd, ra, imm = insn >> 26, (insn >> 21) & 31, (insn >> 16) & 31, insn & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15:  # addis / lis
            regs[rd] = ((regs.get(ra, 0) if ra else 0) + (simm << 16)) & 0xFFFFFFFF
        elif op == 14:  # addi / li
            regs[rd] = ((regs.get(ra, 0) if ra else 0) + simm) & 0xFFFFFFFF
        elif op == 24:  # ori
            regs[ra] = regs.get(rd, 0) | imm
        elif op == 31 and (insn >> 1) & 0x3FF == 444 and rd == (insn >> 11) & 31:  # mr
            if rd in regs:
                regs[ra] = regs[rd]
            else:
                regs.pop(ra, None)
        elif op == 18 and insn & 1:  # bl
            target = (addr + ((insn & 0x3FFFFFC) ^ 0x2000000) - 0x2000000) & 0xFFFFFFFF
            if target == MEMCPY:
                yield regs.get(3), regs.get(4), regs.get(5)
            for r in (0, *range(3, 13)):
                regs.pop(r, None)
        else:
            regs.pop(rd, None)


def find_copies(img, grids):
    """The stage loader's copies whose .rdata source is the same bytes as a
    tile map it lands on, as (source, tile map address, size): the source is
    rebuilt from the tile map."""
    tilemaps = [(a, a + w * h * 2) for (kind, a), (w, h) in grids.items() if kind == TILEMAP_KIND]
    out = []
    for dst, src, size in memcpy_calls(img, STAGE_LOADER, function_end(img, STAGE_LOADER)):
        if None in (dst, src, size) or not any(a <= dst and dst + size <= e for a, e in tilemaps):
            continue
        if img.read(dst, size) == img.read(src, size):
            out.append((src, dst, size))
    return sorted(set(out))


def cmd_extract(args):
    img = load(args.xex)
    grids = find_grids(img)
    for kind in GRID_KINDS:
        os.makedirs(os.path.join(GRID_DIR, kind), exist_ok=True)
    for (kind, address), (w, h) in sorted(grids.items()):
        write_grid(grid_path(kind, address), kind, w, h, grid_bytes(img, kind, address, w, h))
    catalog = read_catalog()
    for folder in CATALOG_DIRS.values():
        os.makedirs(os.path.join(GRID_DIR, folder), exist_ok=True)
    for address, kind, name, args in catalog:
        write_asset(kind, args, name, img.read(address, asset_size(kind, args)))
    xdbf = extract_xdbf(img)
    print(f"{len(grids)} grids, {len(catalog)} cataloged assets and {xdbf} XDBF files "
          f"in {GRID_DIR}")
    return 0


def cmd_extract_copies(args):
    img = load(args.xex)
    copies = find_copies(img, find_grids(img))
    os.makedirs(SOURCE_DIR, exist_ok=True)
    with open(COPIES_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("# .rdata tile maps the stage loader copies over their .data buffer.\n")
        f.write("# destination\tsource\tsize. See scripts/guest_data.py.\n")
        f.writelines(f"0x{dst:08X}\t0x{src:08X}\t0x{size:X}\n" for dst, src, size in copies)
    print(f"{len(copies)} copies in {COPIES_FILE}")
    return 0


def claimed_map(img, *readers):
    """A byte map of what the grids and the catalog hold, and the text
    sources `readers` (read_sprites, ...) read."""
    claimed = bytearray(len(img.data))
    for (kind, address), (w, h) in find_grids(img).items():
        size = len(grid_bytes(img, kind, address, w, h))
        claimed[address - img.base:address - img.base + size] = b"\1" * size
    for address, kind, _, args in read_catalog():
        size = asset_size(kind, args)
        claimed[address - img.base:address - img.base + size] = b"\1" * size
    for reader in readers:
        for address, data in reader():
            claimed[address - img.base:address - img.base + len(data)] = b"\1" * len(data)
    return claimed


def cmd_extract_layouts(args):
    img = load(args.xex)
    layouts = find_layouts(img, claimed_map(img, read_sprites, read_strings))
    write_layouts(layouts)
    entities = sum(len(v[1]) for v in layouts.values())
    print(f"{len(layouts)} layouts, {entities} entities in {LAYOUTS_FILE}")
    return 0


def cmd_extract_strings(args):
    img = load(args.xex)
    found = find_strings(img, claimed_map(img, read_sprites, read_layouts))
    write_strings(found)
    print(f"{len(found)} strings in {STRINGS_FILE}")
    return 0


def cmd_extract_stages(args):
    img = load(args.xex)
    stages = find_stages(img, claimed_map(img, read_sprites, read_layouts, read_strings))
    write_stages(stages)
    print(f"{sum(len(b) for _, b in stages)} structures of {len(stages)} stages in {STAGES_FILE}")
    return 0


def cmd_extract_tables(args):
    img = load(args.xex)
    claimed = claimed_map(img, read_sprites, read_layouts, read_strings, read_stages,
                          read_xdbf)
    for dst, _, size in read_copies():
        claimed[dst - img.base:dst - img.base + size] = b"\1" * size
    blocks = uncovered_blocks(img, claimed)
    write_tables(img, blocks)
    print(f"{len(blocks)} blocks in {TABLES_FILE}")
    return 0


def cmd_extract_xdbf(args):
    write_xdbf(load(args.xex))
    print(f"XDBF in {XDBF_FILE}")
    return 0


def cmd_extract_sprites(args):
    img = load(args.xex)
    arrays = find_sprites(img, claimed_map(img))
    write_sprites(img, arrays)
    frames = sum(1 for words in arrays.values() for w in words if w)
    print(f"{len(arrays)} sprite sets, {frames} frames in {SPRITES_DIR}")
    return 0


def cmd_check(args):
    img = load(args.xex)
    base = img.base
    image = bytearray(len(img.data))
    covered = bytearray(len(img.data))
    failures = 0

    def claim(address, size, what):
        nonlocal failures
        off = address - base
        if any(covered[off:off + size]):
            print(f"OVERLAP {what} at 0x{address:08X}")
            failures += 1
        covered[off:off + size] = b"\1" * size

    for kind, address, data in read_grids():
        claim(address, len(data), kind[:-1])
        image[address - base:address - base + len(data)] = data
    for address, kind, name, args in read_catalog():
        data = read_asset(kind, args, name)
        claim(address, len(data), name)
        image[address - base:address - base + len(data)] = data
    for address, data in read_sprites():
        claim(address, len(data), "sprite data")
        image[address - base:address - base + len(data)] = data
    for address, data in read_layouts():
        claim(address, len(data), "layout")
        image[address - base:address - base + len(data)] = data
    for address, data in read_stages():
        claim(address, len(data), "stage structure")
        image[address - base:address - base + len(data)] = data
    for address, data in read_strings():
        claim(address, len(data), "string")
        image[address - base:address - base + len(data)] = data
    for address, data in read_tables():
        claim(address, len(data), "table")
        image[address - base:address - base + len(data)] = data
    for address, data in read_xdbf():
        claim(address, len(data), "xdbf")
        image[address - base:address - base + len(data)] = data
    for dst, src, size in read_copies():
        claim(dst, size, "copy")
        image[dst - base:dst - base + size] = image[src - base:src - base + size]
    total = done = 0
    for start, end in SOURCE_RANGES:
        for off in range(start - base, end - base):
            if covered[off] and image[off] != img.data[off]:
                print(f"MISMATCH at 0x{base + off:08X}")
                failures += 1
                break
            if img.data[off]:
                total += 1
                done += covered[off]
    print(f"{done} of {total} nonzero data bytes come from source "
          f"({100 * done / total:.1f}%), {failures} failures")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name, fn in (("check", cmd_check), ("extract", cmd_extract),
                     ("extract-copies", cmd_extract_copies),
                     ("extract-sprites", cmd_extract_sprites),
                     ("extract-layouts", cmd_extract_layouts),
                     ("extract-strings", cmd_extract_strings),
                     ("extract-stages", cmd_extract_stages),
                     ("extract-tables", cmd_extract_tables),
                     ("extract-xdbf", cmd_extract_xdbf)):
        p = sub.add_parser(name)
        p.add_argument("xex")
        p.set_defaults(fn=fn)
    args = parser.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
