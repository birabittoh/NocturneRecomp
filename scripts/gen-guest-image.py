#!/usr/bin/env python3
"""Write the guest image from its decompiled data (scripts/guest_data.py).

This is the image the executable loads, which scripts/guest_recipe.py derives
its recipe from; the executable itself builds it at startup from the
player's own default.xex (src/guest_image.h). The image is built from
assets/guest_data_src with no retail file: the XEX
header (xex_header.txt, scripts/xex_header.py) and every byte of data
(scripts/guest_data.py; run its `extract` first for the grids, images and
XDBF files it takes from the user's own default.xex). Code sections are
empty: codegen already compiled them, and the game never reads them. The
randomizer's code patches are compiled hooks too (src/rando_hooks.h), and it
finds the instructions it patches by the bytes it writes, not by what the
image holds. The import thunk words in .text are the header's, since the SDK
reads them at load to pair imports. Zero runs are elided by the XEX "basic"
compression, so only the data takes space.

    python scripts/gen-guest-image.py out/guest-image.xex
"""
import os
import struct
import sys

from guest_data import (overlay_catalog, overlay_copies, overlay_grids, overlay_layouts,
                        overlay_sprites, overlay_stages, overlay_strings, overlay_tables,
                        overlay_xdbf)
from xex_header import PAGE_SIZE, build_header, import_words, read_header

# Shorter zero runs cost more as a block record than they save.
MIN_ZERO_RUN = 64


def blocks(image):
    """(data_size, zero_size) runs covering the whole image."""
    out = []
    data_start = 0
    pos = 0
    n = len(image)
    while pos < n:
        if image[pos]:
            pos += 1
            continue
        end = pos
        while end < n and not image[end]:
            end += 1
        if end - pos >= MIN_ZERO_RUN or end == n:
            out.append((pos - data_start, end - pos))
            data_start = end
        pos = end
    if data_start < n:
        out.append((n - data_start, 0))
    return out


def offsets(runs):
    off = 0
    for data, zero in runs:
        yield off, data
        off += data + zero


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    dst = sys.argv[1]
    spec = read_header()
    image = bytearray(PAGE_SIZE * sum(c for _, c in spec["pages"]))
    base = spec["load_address"][0]
    overlay_grids(image, base)
    overlay_catalog(image, base)
    overlay_sprites(image, base)
    overlay_layouts(image, base)
    overlay_strings(image, base)
    overlay_stages(image, base)
    overlay_tables(image, base)
    overlay_xdbf(image, base)
    overlay_copies(image, base)
    words = import_words(spec)
    for addr, word in words.items():
        struct.pack_into(">I", image, addr - base, word)

    runs = blocks(bytes(image))
    body = b"".join(image[off:off + data] for off, data in offsets(runs))
    header = build_header(spec, runs)
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, "wb") as f:
        f.write(header)
        f.write(body)
    print(f"{dst}: {len(header) + len(body)} bytes, {len(runs)} blocks, "
          f"{len(words)} import words")
    return 0


if __name__ == "__main__":
    sys.exit(main())
