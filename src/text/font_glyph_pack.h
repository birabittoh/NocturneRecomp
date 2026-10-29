// nocturnerecomp - the supplemental Japanese glyphs we ship ourselves.
//
// assets/MEDIA/font_ja.xpr is a subset font built only from the characters
// strings_ja.bin uses. The achievement strings live in default.xex and were
// never part of that input, so 33 characters -- 23 kanji, 4 kana and 6
// fullwidth forms -- have no glyph. On real hardware the dashboard drew the
// achievement list with its own font and the gap never showed; now that
// achievements_screen.cpp renders them in-game with the title's font, they
// come out blank.
//
// font_ja.xpr is the player's own extracted copy of a copyrighted asset, so
// the fix cannot be "ship a patched font". Instead this pack carries artwork
// we drew ourselves, and font_patch.cpp composes it with the player's file in
// memory at load time. Nothing derived from their font is ever written out.
//
// The art and its metrics live in src/fonts/glyphs_ja/; scripts/gen-glyphs.py
// bakes them into glyph_pack.generated.h, which defines kSupplementalGlyphs.
#pragma once

#include <cstdint>

namespace nocturne {

// One supplemental glyph: an 8-bit coverage bitmap plus the placement metrics
// the font's GLYPH_ATTR needs.
//
// `coverage` is `width * height` bytes, row-major, 0 = transparent and 255 =
// solid. It is null for a codepoint whose art has not been drawn yet, in which
// case font_patch.cpp skips it and the character keeps rendering blank -- the
// same as today, never worse.
struct SupplementalGlyph {
  char32_t codepoint;

  // Bitmap dimensions in pixels. Zero when there is no art yet.
  uint16_t width;
  uint16_t height;

  // Left sidebearing: pixels between the pen position and the ink box. Maps to
  // GLYPH_ATTR::offset.
  int16_t left;

  // Pixels between the top of the cell and the top of the ink box. The stock
  // font's glyph boxes are all full cell height, so the bitmap is blitted at
  // this offset inside a kCellHeight-tall box to keep baselines aligned.
  int16_t top;

  // Pen advance in pixels. Maps to GLYPH_ATTR::advance.
  int16_t advance;

  const unsigned char* coverage;
};

}  // namespace nocturne
