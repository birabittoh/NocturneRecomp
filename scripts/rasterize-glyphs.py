#!/usr/bin/env python3
"""Rasterizes the missing Japanese glyphs from a TrueType font into the PNG art
that scripts/gen-glyphs.py bakes into the build.

This is the starting point for filling src/fonts/glyphs_ja/. Drawing 23 kanji
by hand is not the job -- picking an openly-licensed Japanese font whose gothic
matches the title's, rasterizing at the right size, and nudging the metrics is.

Font licensing
--------------
We redistribute the *rendered bitmaps*, so the source font's licence has to
permit that. Any SIL Open Font License face is fine (the OFL explicitly allows
redistributing rendered output, and bitmaps are not "the Font Software"):

  * Noto Sans JP        https://github.com/notofonts/noto-cjk        (OFL)
  * M PLUS 1p / M PLUS 1 https://github.com/coz-m/MPLUS_FONTS        (OFL)
  * Source Han Sans      https://github.com/adobe-fonts/source-han-sans (OFL)
  * IPAexGothic          https://moji.or.jp/ipafont/                 (IPA licence)

Do NOT point this at a Windows system font (MS Gothic, Meiryo, Yu Gothic) --
those are not redistributable and the whole point of this pack is that we ship
only art we are allowed to ship.

Matching the stock font
-----------------------
font_ja.xpr's own metrics give exact targets (all boxes are the full 28px cell
height; the numbers below are ink-box widths and pen advances):

    kanji       box 17..21, median 20   advance 19..21, median 21
    hiragana    box 11..20, median 18   advance 11..21, median 18
    katakana    box  6..21, median 18   advance  9..21, median 18
    fullwidth   proportional, NOT full-em -- U+FF12 '2' is box 11, advance 12

That last one matters: the Japanese font draws fullwidth forms proportionally,
so the fullwidth digits we need should match U+FF12 rather than filling a cell.
TARGETS below encodes a model glyph for each missing character.

Usage
-----
    python scripts/rasterize-glyphs.py --font NotoSansJP-Medium.ttf
    python scripts/rasterize-glyphs.py --font ... --size 23 --baseline 23
    python scripts/rasterize-glyphs.py --font ... --preview U+53D7

Writes src/fonts/glyphs_ja/U+XXXX.png plus a metrics.toml stanza for each
glyph. Re-run freely; it overwrites its own output. Tune --size until the
reported box widths land on the targets, then --baseline until the glyphs sit
right next to the stock text in game.
"""
import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit("this script needs Pillow:  pip install Pillow")

import importlib.util

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def _load_required_codepoints():
    """gen-glyphs.py is the single source of truth for which characters are
    missing; its filename is hyphenated, so import it by path."""
    path = os.path.join(ROOT, "scripts", "gen-glyphs.py")
    spec = importlib.util.spec_from_file_location("gen_glyphs", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.REQUIRED_CODEPOINTS


REQUIRED_CODEPOINTS = _load_required_codepoints()
ART_DIR = os.path.join(ROOT, "src", "fonts", "glyphs_ja")

CELL_HEIGHT = 28

# Per-character advance target, and the stock glyph it was read off. Anything
# not listed here is a kanji and uses KANJI_ADVANCE.
#
# The kana are all "base character plus dakuten", so they inherit the base's
# advance -- the dakuten rides above the ink and does not widen the pen step.
KANJI_ADVANCE = 21
TARGETS = {
    0x3079: (20, "modelled on へ he, box 19 advance 20"),
    0x30AC: (17, "modelled on カ KA, box 16 advance 17"),
    0x30AE: (19, "modelled on キ KI, box 19 advance 19"),
    0x30D2: (18, "modelled on ビ BI, box 17 advance 18"),
    0x30B6: (19, "modelled on サ SA, box 19 advance 19"),
    0x30BC: (19, "modelled on セ SE, box 18 advance 19"),
    0x30EF: (17, "modelled on ク KU, box 16 advance 16"),
    0x3058: (14, "modelled on し shi, box 14 advance 14"),
    0x305D: (18, "no stock そ; the hiragana median"),
    0x3069: (15, "modelled on と to, box 15 advance 15"),
    0x307B: (18, "modelled on は ha, box 17 advance 18"),
    0x307C: (18, "modelled on は ha, box 17 advance 18"),
}

# No fullwidth forms are listed: font_patch.cpp aliases U+FF01..U+FF5E onto the
# stock ASCII glyphs instead of using art from here. The --fullwidth-size
# handling below is kept for any fullwidth character that ever needs real art.

RAMP = " .:-=+*#%@"


def render(font, ch, baseline, pad=16):
    """Renders `ch` and returns (cropped L-mode image, left, top) where left/top
    are the ink box's offset from the pen position and the top of the cell."""
    canvas = Image.new("L", (CELL_HEIGHT + pad * 2, CELL_HEIGHT + pad * 2), 0)
    draw = ImageDraw.Draw(canvas)
    # anchor "ls" = left, baseline. The pen sits at (pad, pad + baseline).
    draw.text((pad, pad + baseline), ch, fill=255, font=font, anchor="ls")
    box = canvas.getbbox()
    if box is None:
        return None, 0, 0
    left, top, right, bottom = box
    return canvas.crop(box), left - pad, top - pad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="path to an OFL/IPA-licensed .ttf/.otf")
    ap.add_argument("--size", type=int, default=23,
                    help="pixel em size for kanji and kana; tune until kanji boxes "
                         "land near 20 (default 23)")
    ap.add_argument("--fullwidth-size", type=int, default=18,
                    help="pixel em size for the U+FFxx forms, which the stock font "
                         "draws proportionally -- tune until they land near box 11, "
                         "matching U+FF12 (default 18)")
    ap.add_argument("--baseline", type=int, default=23,
                    help="baseline position within the 28px cell (default 23)")
    ap.add_argument("--variation",
                    help="named instance to use from a variable font, e.g. 'Medium'. "
                         "Noto Sans JP ships as one axis with Thin..Black instances; "
                         "at this pixel size Medium/SemiBold read closest to the "
                         "stock bitmap font.")
    ap.add_argument("--preview", metavar="U+XXXX",
                    help="render one codepoint as ASCII art and exit, writing nothing")
    ap.add_argument("--out", default=ART_DIR)
    args = ap.parse_args()

    sys.stdout.reconfigure(encoding="utf-8")
    # The stock font draws the fullwidth forms proportionally rather than at a
    # full em, so they are rasterized a few pixels smaller than the kanji.
    def load(size):
        f = ImageFont.truetype(args.font, size)
        if args.variation:
            f.set_variation_by_name(args.variation)
        return f

    font = load(args.size)
    fullwidth_font = load(args.fullwidth_size)

    def font_for(cp):
        return fullwidth_font if 0xFF00 <= cp <= 0xFF60 else font

    if args.preview:
        cp = int(args.preview.lstrip("Uu+"), 16)
        img, left, top = render(font_for(cp), chr(cp), args.baseline)
        if img is None:
            raise SystemExit("U+%04X: the font has no glyph for this character" % cp)
        print("U+%04X %s   box %dx%d  left=%d top=%d"
              % (cp, chr(cp), img.width, img.height, left, top))
        px = img.load()
        for y in range(img.height):
            print("|" + "".join(RAMP[px[x, y] * 10 // 256] for x in range(img.width)) + "|")
        return

    os.makedirs(args.out, exist_ok=True)
    written, skipped = 0, []
    lines = []
    for cp in REQUIRED_CODEPOINTS:
        ch = chr(cp)
        img, left, top = render(font_for(cp), ch, args.baseline)
        if img is None:
            skipped.append(cp)
            continue
        if img.height > CELL_HEIGHT:
            skipped.append(cp)
            print("  ! U+%04X is %dpx tall, over the %dpx cell -- lower --size"
                  % (cp, img.height, CELL_HEIGHT))
            continue
        img.save(os.path.join(args.out, "U+%04X.png" % cp))
        written += 1
        advance, note = TARGETS.get(cp, (KANJI_ADVANCE, "kanji, stock median advance 21"))
        lines.append("U+%04X = { left = %d, top = %d, advance = %d }  # %s box %dx%d -- %s"
                     % (cp, left, top, advance, ch, img.width, img.height, note))

    print("wrote %d PNG(s) to %s" % (written, os.path.relpath(args.out, ROOT)))
    if skipped:
        print("no glyph in this font for: " + " ".join("U+%04X" % c for c in skipped))
    print("\nAppend to src/fonts/glyphs_ja/metrics.toml (review the box sizes first):\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
