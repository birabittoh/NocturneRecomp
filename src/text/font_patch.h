// nocturnerecomp - compose the player's font_ja.xpr with our supplemental
// glyph pack, in memory.
//
// See font_glyph_pack.h for why this is needed. The composition never touches
// disk: font_overlay.cpp serves the result to the guest through the VFS, so
// the player's own font file is read but never rewritten.
//
// The .xpr layout this works against (verified against both font_en.xpr and
// font_ja.xpr) is the stock XDK/ATG bitmap-font resource:
//
//   0x00  XPR2 header: magic, header_size, data_size, resource_count
//   0x10  resource table, then the resource name strings
//   0x4C  D3DTexture for "FontTexture" (0x34 bytes); its
//         GPUTEXTURE_FETCH_CONSTANT at +0x1C carries pitch, tiled flag,
//         format and the 2D size
//   0x80  "FontData" blob:
//           0x80  uint32  version (5)
//           0x84  float   cell height (28.0 for JA, 29.0 for EN)
//           0x94  uint16  max mapped codepoint (0xFF1A for JA)
//           0x96  uint16  translator[max_codepoint + 1] -- glyph index per
//                         UTF-16 codepoint, 0 meaning "no glyph"
//           then  uint32  glyph_count
//           then  GLYPH_ATTR[glyph_count]
//   header_size + 12 .. + data_size         the texture atlas
//
// Note the 12: the texture data does *not* start at header_size. Both stock
// files put 12 zero bytes there first, which is why they are 12 bytes longer
// than header_size + data_size. font_en.xpr shows it directly -- its first
// k_4_4_4_4 texels (0f ff 0f ff) sit at header_size + 12.
//
// GLYPH_ATTR is 16 bytes of big-endian fields:
//   uint16 tu1, tv1, tu2, tv2   ink box in *pixels* (not normalised)
//   int16  offset               left sidebearing
//   int16  width                ink width
//   int16  advance              pen advance
//   uint16 mask                 channel mask (0 throughout both stock fonts)
//
// Two properties of font_ja.xpr make this patch cheap:
//   * Every codepoint we need is <= 0xFF1A, so the translator table already
//     has a (zero) slot for each -- it never has to grow.
//   * The last stock glyph ends at y=522 of a 1024-tall atlas, so the bottom
//     half is free real estate. The atlas never has to grow either.
// Only the glyph array grows, which pushes header_size out and shifts the
// texture data; both are declared in the header, so the guest loader follows.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "text/font_glyph_pack.h"

namespace nocturne {

struct FontPatchResult {
  // The composed .xpr image, empty if patching failed for any reason. Callers
  // must fall back to the original bytes rather than treating this as fatal:
  // a missing glyph renders blank, which is exactly today's behaviour, but a
  // malformed font would take the whole screen down.
  std::vector<uint8_t> image;

  // Number of glyphs actually added (entries with no art yet are skipped).
  uint32_t glyphs_added = 0;

  // Number of fullwidth codepoints pointed at an existing stock ASCII glyph
  // rather than at art of ours. Costs no atlas space.
  uint32_t glyphs_aliased = 0;

  // Set when the font parsed but could not fit the pack -- worth logging
  // loudly, since it means the atlas packing needs revisiting.
  bool out_of_space = false;

  // Human-readable reason when `image` is empty.
  const char* error = nullptr;
};

// Builds a patched copy of `original` (a whole font_*.xpr file) carrying the
// glyphs in `glyphs`. Glyphs whose `coverage` is null, or whose codepoint the
// font already has a glyph for, are skipped.
FontPatchResult PatchFont(std::span<const uint8_t> original,
                          std::span<const SupplementalGlyph> glyphs);

}  // namespace nocturne
