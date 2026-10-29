#!/usr/bin/env python3
"""Bakes the supplemental Japanese glyph art in src/fonts/glyphs_ja/ into
src/glyph_pack.generated.h.

Why this exists
---------------
assets/MEDIA/font_ja.xpr is a *subset* font: it only contains the characters
used by strings_ja.bin (the in-game text). The achievement strings live in
default.xex and were never part of that subsetting input -- on real hardware
the dashboard drew them with its own font, so the gap never showed. Now that
the achievements screen is rendered in-game (see achievements_screen.cpp) the
missing characters come out as blank boxes.

font_ja.xpr is the user's own extracted copy of a copyrighted asset, so we
cannot modify and reship it. Instead we ship *our own* glyph art here and
compose the two in memory at load time (see font_patch.cpp, font_overlay.cpp).
Only the artwork in src/fonts/glyphs_ja/ is redistributed; the user's font
never leaves their machine.

Source art format
-----------------
One PNG per codepoint in src/fonts/glyphs_ja/, named "U+XXXX.png" (uppercase
hex, no zero padding beyond 4 digits -- e.g. U+53D7.png for the kanji 'shou').

  * 8-bit greyscale or RGBA. Coverage is taken from the alpha channel when the
    image has one, otherwise from luminance. White-on-transparent and
    white-on-black both work.
  * The bitmap is the glyph's ink box only -- do not pad it out to the full
    cell. Horizontal placement comes from the sidebearing metadata below.
  * Height must not exceed the font's own cell height (kCellHeight). The JA
    font is 28px tall with a 28px cell; matching that keeps the baseline
    aligned with the stock glyphs.

Per-glyph metrics live in metrics.toml alongside the art. Anything absent
falls back to a sensible default derived from the bitmap, which is usually
right for full-width CJK.

Every codepoint in REQUIRED_CODEPOINTS gets an entry in the generated header
whether or not its PNG exists yet. Missing art emits a zero-size placeholder
and a build-time warning, so an incomplete set is visible rather than silently
dropping characters.
"""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ART_DIR = os.path.join(ROOT, "src", "fonts", "glyphs_ja")
OUT_PATH = os.path.join(ROOT, "src", "glyph_pack.generated.h")
METRICS_PATH = os.path.join(ART_DIR, "metrics.toml")

# The font_ja.xpr cell height, from the FontData header at 0x84 (28.0). Glyph
# boxes in the stock font are all exactly this tall.
CELL_HEIGHT = 28

# The characters the Japanese achievement strings use that font_ja.xpr has no
# glyph for and that need art of our own. Produced by scripts/font_coverage.py
# -- rerun it after any change to the achievement text or to the set of strings
# drawn natively.
#
# font_coverage.py reports 33 missing characters; only these 27 are listed. The
# other 6 are fullwidth forms (U+FF04 ＄, U+FF0C ，, U+FF10 ０, U+FF11 １,
# U+FF15 ５, U+FF18 ８), which font_patch.cpp covers by aliasing the translator
# entry onto the stock ASCII glyph at cp - 0xFEE0. Drawing our own would put two
# visibly different zeroes on the same screen, since "200.6%" is written with
# halfwidth ASCII that the stock font already has.
REQUIRED_CODEPOINTS = [
    # Kana. Note the font stores every glyph precomposed -- there is no dakuten
    # combining -- so these need their own bitmaps even though the font already
    # has HE/KA/KI and the other members of their rows.
    0x3079,  # be  (hiragana)
    0x30AC,  # GA  (katakana)
    0x30AE,  # GI  (katakana)
    0x30D2,  # HI  (katakana)
    # Kanji.
    0x53D7, 0x53EF, 0x5E2B, 0x5E38, 0x5EFB, 0x5FA9, 0x5FC3, 0x6240,
    0x6301, 0x6575, 0x72FC, 0x7A2E, 0x8005, 0x8840, 0x888B, 0x8B90,
    0x8F2A, 0x9006, 0x901A, 0x9054, 0x91D1, 0x9727, 0x985E,
    # Kanji for the rows native_options.cpp adds to the game's own options
    # screen: 垂直同期 (VSync), of which the font has only 直 and 期.
    0x540C, 0x5782,
    # Kana and kanji for the enhancements page and the Help & Options item
    # (native_enhancements.cpp, help_options_menu.cpp).
    0x30B6, 0x30BC, 0x30EF,
    0x4F11, 0x4FEE, 0x50CF, 0x5272, 0x5316, 0x53BB, 0x56F3, 0x5730,
    0x5893, 0x5B9D, 0x5C4B, 0x5D29, 0x5F15, 0x5F37, 0x6249, 0x6271,
    0x6307, 0x64A4, 0x653E, 0x6599, 0x6687, 0x66F8, 0x6817, 0x6A19,
    0x6A4B, 0x6B62, 0x6B63, 0x7121, 0x7565, 0x7701, 0x77F3, 0x7814,
    0x7A76, 0x7DD1, 0x8272, 0x8A08, 0x8D64, 0x8DB3, 0x90E8, 0x91CD,
    0x9752, 0x9928,
    # Kana and kanji for the randomizer preset descriptions on the native
    # randomizer page (res/lang [rando_presets]), and for page titles and the
    # page-switch prompt (快適性, その他, ページ切替).
    0x3058, 0x305D, 0x3069, 0x307B, 0x307C,
    0x4E00, 0x5143, 0x52B9, 0x540D, 0x57FA, 0x5916, 0x5B88, 0x5C11,
    0x5FEB, 0x6027, 0x66FF, 0x6709, 0x672C, 0x679A, 0x6B8B, 0x6D88,
    0x6E1B, 0x7531, 0x7684, 0x81EA, 0x843D, 0x8A73, 0x8ABF, 0x8CBB,
    0x8FD1, 0x9069, 0x907A, 0x9577, 0x9806, 0x98DB, 0x98DF,
]

# Human-readable names, for the generated header's comments only.
NAMES = {
    0x3079: "be", 0x30AC: "GA", 0x30AE: "GI", 0x30D2: "HI",
    0x30B6: "ZA", 0x30BC: "ZE", 0x30EF: "WA",
}


def read_png_gray(path):
    """Minimal PNG reader -> (width, height, bytearray of 8-bit coverage).

    Supports the subset any image editor will produce for this kind of art:
    8-bit greyscale, greyscale+alpha, RGB and RGBA, non-interlaced. Coverage
    comes from alpha when present, else from luminance.
    """
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos = 8
    width = height = depth = color = None
    idat = bytearray()
    while pos < len(data):
        length, ctype = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8:
                raise ValueError("%s: only 8-bit PNGs are supported" % path)
            if interlace:
                raise ValueError("%s: interlaced PNGs are not supported" % path)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break

    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
    if channels is None:
        raise ValueError("%s: unsupported PNG colour type %d" % (path, color))
    raw = zlib.decompress(bytes(idat))

    # Undo per-scanline filtering.
    stride = width * channels
    out = bytearray(width * height)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
            elif ftype != 0:
                raise ValueError("%s: bad filter type %d" % (path, ftype))
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if channels == 2:
                out[y * width + x] = px[1]
            elif channels == 4:
                out[y * width + x] = px[3]
            elif channels == 3:
                out[y * width + x] = (px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8
            else:
                out[y * width + x] = px[0]
        prev = line
    return width, height, out


def load_metrics():
    """Parses the flat `U+XXXX = { left = N, advance = N, top = N }` metrics."""
    metrics = {}
    if not os.path.exists(METRICS_PATH):
        return metrics
    # A trailing "# ..." comment is allowed: rasterize-glyphs.py emits one per
    # line recording the rasterized box size and the stock glyph it was
    # modelled on.
    entry_re = re.compile(r"^\s*U\+([0-9A-Fa-f]{4,6})\s*=\s*\{([^}]*)\}\s*(?:#.*)?$")
    field_re = re.compile(r"(\w+)\s*=\s*(-?\d+)")
    with open(METRICS_PATH, encoding="utf-8") as f:
        for line in f:
            if line.lstrip().startswith("#"):
                continue
            m = entry_re.match(line)
            if m:
                metrics[int(m.group(1), 16)] = {
                    k: int(v) for k, v in field_re.findall(m.group(2))
                }
    return metrics


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    metrics = load_metrics()
    glyphs = []
    missing = []

    for cp in REQUIRED_CODEPOINTS:
        path = os.path.join(ART_DIR, "U+%04X.png" % cp)
        meta = metrics.get(cp, {})
        if os.path.exists(path):
            w, h, cov = read_png_gray(path)
            if h > CELL_HEIGHT:
                raise SystemExit(
                    "U+%04X: art is %dpx tall, cell height is %d" % (cp, h, CELL_HEIGHT))
        else:
            missing.append(cp)
            w = h = 0
            cov = bytearray()
        # Defaults suit full-width CJK: ink flush in a square cell, advancing
        # the full cell width. Kana and the fullwidth forms usually want the
        # same; anything narrower should override `advance` in metrics.toml.
        left = meta.get("left", 0)
        top = meta.get("top", 0)
        advance = meta.get("advance", w + left if w else CELL_HEIGHT)
        glyphs.append((cp, w, h, left, top, advance, cov))

    lines = [
        "#pragma once",
        "",
        "// Generated by scripts/gen-glyphs.py from the art in src/fonts/glyphs_ja/.",
        "// Do not edit by hand or commit this file - it is rebuilt on every CMake",
        "// configure. See that script for the source format and for why this pack",
        "// exists at all (font_ja.xpr is a subset font missing these characters,",
        "// and it is the user's own copyrighted asset, so it cannot be reshipped).",
        "",
        "#include <cstdint>",
        "",
        "#include \"font_glyph_pack.h\"",
        "",
        "namespace nocturne {",
        "",
    ]

    blobs = []
    for cp, w, h, left, top, advance, cov in glyphs:
        if not cov:
            continue
        ident = "kGlyph%04X" % cp
        blobs.append("inline constexpr unsigned char %s[] = {" % ident)
        for i in range(0, len(cov), 20):
            blobs.append("    " + ", ".join(str(b) for b in cov[i:i + 20]) + ",")
        blobs.append("};")
        blobs.append("")

    table = ["inline constexpr SupplementalGlyph kSupplementalGlyphs[] = {"]
    for cp, w, h, left, top, advance, cov in glyphs:
        name = NAMES.get(cp)
        comment = "  // U+%04X%s" % (cp, " %s" % name if name else "")
        if not cov:
            comment += " -- NO ART YET"
        data = "kGlyph%04X" % cp if cov else "nullptr"
        table.append("    {0x%04X, %d, %d, %d, %d, %d, %s},%s"
                     % (cp, w, h, left, top, advance, data, comment))
    table.append("};")

    with open(OUT_PATH, "w", newline="\n") as out:
        out.write("\n".join(lines))
        out.write("\n".join(blobs))
        out.write("\n")
        out.write("\n".join(table))
        out.write("\n\n}  // namespace nocturne\n")

    print("+ wrote %s (%d glyphs, %d still undrawn)"
          % (OUT_PATH, len(glyphs), len(missing)))
    if missing:
        print("  no art yet for: " + " ".join("U+%04X" % c for c in missing))


if __name__ == "__main__":
    main()
