#include "text/font_patch.h"

#include <algorithm>
#include <cstring>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace nocturne {
namespace {

// XPR2 header.
constexpr uint32_t kXpr2Magic = 0x58505232u;  // 'XPR2'
constexpr size_t kHeaderSizeOffset = 0x04;
constexpr size_t kDataSizeOffset = 0x08;

// D3DTexture for "FontTexture"; its GPUTEXTURE_FETCH_CONSTANT sits at +0x1C.
constexpr size_t kTextureHeaderOffset = 0x4C;
constexpr size_t kFetchConstantOffset = kTextureHeaderOffset + 0x1C;

// "FontData" blob.
constexpr size_t kFontDataOffset = 0x80;
constexpr size_t kVersionOffset = kFontDataOffset + 0x00;
constexpr size_t kCellHeightOffset = kFontDataOffset + 0x04;
constexpr size_t kMaxCodepointOffset = kFontDataOffset + 0x14;
constexpr size_t kTranslatorOffset = kFontDataOffset + 0x16;

constexpr uint32_t kSupportedVersion = 5;
constexpr size_t kGlyphAttrSize = 16;

// A DXT2/3 block is 4x4 texels in 16 bytes: 8 bytes of 4-bit alpha, then a
// DXT1 colour block (two RGB565 endpoints and 2-bit indices).
constexpr uint32_t kBlockDim = 4;
constexpr uint32_t kBlockBytes = 16;
constexpr uint32_t kBlockBytesLog2 = 4;

// The texture data does not start at header_size: the stock files put 12 bytes
// of zero padding there first, so the atlas occupies
// [header_size + 12, header_size + 12 + data_size). font_en.xpr shows this
// plainly -- its first real 4444 texels (0f ff 0f ff) sit at +12.
constexpr size_t kAtlasDataOffset = 12;

// Halfwidth/Fullwidth Forms: U+FF01..U+FF5E map onto ASCII 0x21..0x7E.
constexpr uint32_t kFullwidthFirst = 0xFF01;
constexpr uint32_t kFullwidthLast = 0xFF5E;
constexpr uint32_t kFullwidthToAscii = 0xFEE0;

// One texel of horizontal breathing room between packed glyphs, matching the
// gutter the stock atlas leaves.
constexpr uint32_t kGutter = 1;

uint16_t ReadU16(std::span<const uint8_t> d, size_t off) {
  return static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
}
uint32_t ReadU32(std::span<const uint8_t> d, size_t off) {
  return (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
         (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
}
void WriteU16(std::vector<uint8_t>& d, size_t off, uint16_t v) {
  d[off] = static_cast<uint8_t>(v >> 8);
  d[off + 1] = static_cast<uint8_t>(v);
}
void WriteU32(std::vector<uint8_t>& d, size_t off, uint32_t v) {
  d[off] = static_cast<uint8_t>(v >> 24);
  d[off + 1] = static_cast<uint8_t>(v >> 16);
  d[off + 2] = static_cast<uint8_t>(v >> 8);
  d[off + 3] = static_cast<uint8_t>(v);
}

// What we need out of the texture's fetch constant. The dwords are stored
// big-endian in the file and the bitfields are the little-endian-value layout
// rex::graphics::xenos::xe_gpu_texture_fetch_t describes, so reading each
// dword big-endian yields the field values directly.
struct AtlasInfo {
  uint32_t pitch = 0;   // texels
  uint32_t width = 0;   // texels
  uint32_t height = 0;  // texels
  uint32_t format = 0;
  uint32_t endianness = 0;
  bool tiled = false;
};

AtlasInfo ReadAtlasInfo(std::span<const uint8_t> d) {
  const uint32_t dword0 = ReadU32(d, kFetchConstantOffset + 0);
  const uint32_t dword1 = ReadU32(d, kFetchConstantOffset + 4);
  const uint32_t dword2 = ReadU32(d, kFetchConstantOffset + 8);

  AtlasInfo info;
  info.pitch = ((dword0 >> 22) & 0x1FF) * 32;
  info.tiled = (dword0 >> 31) != 0;
  info.format = dword1 & 0x3F;
  info.endianness = (dword1 >> 6) & 0x3;
  info.width = (dword2 & 0x1FFF) + 1;
  info.height = ((dword2 >> 13) & 0x1FFF) + 1;
  return info;
}

// The guest stores the atlas with an 8-in-16 endian swap (endianness == 1 in
// both stock fonts), so every 16-bit word is byte-reversed relative to the
// values the GPU sees. We build blocks in natural order and swap on the way
// out.
void SwapEndian8In16(uint8_t* p, size_t bytes) {
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    std::swap(p[i], p[i + 1]);
  }
}

// Encodes one 4x4 texel tile of coverage into a DXT2/3 block.
//
// The glyph is written into *both* channels on purpose. The stock atlas keeps
// shape information in the colour block as well as the alpha nibbles, and the
// title's text shader is not something we control, so a block that reads as
// white-with-coverage whether it is sampled through alpha or through luminance
// is the safe encoding: alpha carries 4-bit coverage, and the colour indices
// pick between white and black on a mid-grey threshold.
void EncodeBlock(const uint8_t* coverage, uint32_t stride, uint32_t valid_w, uint32_t valid_h,
                 uint8_t* out) {
  std::memset(out, 0, kBlockBytes);

  // Alpha: 4 bits per texel, two texels per byte, low nibble first.
  for (uint32_t y = 0; y < kBlockDim; ++y) {
    for (uint32_t x = 0; x < kBlockDim; ++x) {
      const uint8_t a =
          (x < valid_w && y < valid_h) ? static_cast<uint8_t>(coverage[y * stride + x]) : 0;
      const uint8_t nibble = static_cast<uint8_t>(a >> 4);
      uint8_t& dst = out[y * 2 + (x >> 1)];
      dst = static_cast<uint8_t>(dst | (nibble << ((x & 1) * 4)));
    }
  }

  // Colour: endpoint 0 white, endpoint 1 black, index 0 or 1 per texel.
  out[8] = 0xFF;
  out[9] = 0xFF;  // colour0 = 0xFFFF (white)
  out[10] = 0x00;
  out[11] = 0x00;  // colour1 = 0x0000 (black)
  for (uint32_t y = 0; y < kBlockDim; ++y) {
    uint8_t indices = 0;
    for (uint32_t x = 0; x < kBlockDim; ++x) {
      const uint8_t a =
          (x < valid_w && y < valid_h) ? static_cast<uint8_t>(coverage[y * stride + x]) : 0;
      if (a >= 0x80) {
        indices = static_cast<uint8_t>(indices | (0u << (x * 2)));  // white
      } else {
        indices = static_cast<uint8_t>(indices | (1u << (x * 2)));  // black
      }
    }
    out[12 + y] = indices;
  }
}

}  // namespace

FontPatchResult PatchFont(std::span<const uint8_t> original,
                          std::span<const SupplementalGlyph> glyphs) {
  FontPatchResult result;

  if (original.size() < kFontDataOffset + 0x20 || ReadU32(original, 0) != kXpr2Magic) {
    result.error = "not an XPR2 image";
    return result;
  }
  if (ReadU32(original, kVersionOffset) != kSupportedVersion) {
    result.error = "unsupported FontData version";
    return result;
  }

  const uint32_t header_size = ReadU32(original, kHeaderSizeOffset);
  const uint32_t data_size = ReadU32(original, kDataSizeOffset);
  if (static_cast<size_t>(header_size) + data_size > original.size()) {
    result.error = "truncated XPR2 image";
    return result;
  }

  const AtlasInfo atlas = ReadAtlasInfo(original);
  if (atlas.format != static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT2_3) ||
      !atlas.tiled) {
    // font_en.xpr is k_4_4_4_4 and would need a different encoder; the Latin
    // font is not missing anything, so there is nothing to do for it.
    result.error = "atlas is not a tiled DXT2/3 texture";
    return result;
  }

  const uint32_t max_codepoint = ReadU16(original, kMaxCodepointOffset);
  const size_t glyph_count_offset =
      kTranslatorOffset + static_cast<size_t>(max_codepoint + 1) * 2;
  if (glyph_count_offset + 4 > header_size) {
    result.error = "translator table overruns the header";
    return result;
  }
  const uint32_t glyph_count = ReadU32(original, glyph_count_offset);
  const size_t glyph_array_offset = glyph_count_offset + 4;
  if (glyph_array_offset + static_cast<size_t>(glyph_count) * kGlyphAttrSize > header_size) {
    result.error = "glyph array overruns the header";
    return result;
  }

  // The cell height is a float at 0x84 (28.0 for JA). Every stock glyph box is
  // exactly this tall, so truncating to an integer is safe.
  const uint32_t cell_height_bits = ReadU32(original, kCellHeightOffset);
  float cell_height_f = 0.0f;
  std::memcpy(&cell_height_f, &cell_height_bits, sizeof(cell_height_f));
  const uint32_t cell_height = static_cast<uint32_t>(cell_height_f);
  if (cell_height == 0 || cell_height > atlas.height) {
    result.error = "implausible cell height";
    return result;
  }

  // Fullwidth forms (U+FF01..U+FF5E) are just the ASCII characters shifted by
  // kFullwidthToAscii, and the stock font already carries the ASCII ones. The
  // achievement strings mix the two -- "200.6%" is halfwidth, "＄１００，０００"
  // is fullwidth -- so drawing our own art for the fullwidth digits would put
  // two visibly different zeroes on the same screen. Aliasing the translator
  // entry onto the stock glyph instead reuses the game's own artwork, which
  // matches exactly and costs nothing. This is a table entry we compute in
  // memory, not font art we ship.
  std::vector<uint16_t> aliases(max_codepoint + 1, 0);
  for (uint32_t cp = kFullwidthFirst; cp <= kFullwidthLast && cp <= max_codepoint; ++cp) {
    if (ReadU16(original, kTranslatorOffset + static_cast<size_t>(cp) * 2) != 0) {
      continue;  // the font already has the fullwidth form
    }
    const uint16_t ascii_glyph =
        ReadU16(original, kTranslatorOffset + static_cast<size_t>(cp - kFullwidthToAscii) * 2);
    aliases[cp] = ascii_glyph;
  }

  // Work out where the stock glyphs stop so we can pack below them. Round the
  // start up to a DXT block row and leave a gutter, so no block we touch is
  // shared with an existing glyph.
  uint32_t used_bottom = 0;
  for (uint32_t i = 0; i < glyph_count; ++i) {
    const size_t attr = glyph_array_offset + static_cast<size_t>(i) * kGlyphAttrSize;
    used_bottom = std::max<uint32_t>(used_bottom, ReadU16(original, attr + 6));  // tv2
  }
  uint32_t pack_y = (used_bottom + kGutter + kBlockDim - 1) & ~(kBlockDim - 1);

  // Collect the glyphs that actually have art and that the font is missing.
  struct Pending {
    const SupplementalGlyph* glyph;
    uint32_t x, y;  // ink box origin in the atlas
  };
  std::vector<Pending> pending;
  uint32_t pen_x = 0;
  for (const SupplementalGlyph& g : glyphs) {
    if (!g.coverage || g.width == 0 || g.height == 0) {
      continue;  // not drawn yet -- leave the character rendering blank
    }
    if (g.codepoint > max_codepoint) {
      continue;  // outside the translator table; would need it to grow
    }
    if (ReadU16(original, kTranslatorOffset + static_cast<size_t>(g.codepoint) * 2) != 0) {
      continue;  // the font already has this one
    }
    if (aliases[g.codepoint] != 0) {
      continue;  // covered by a stock glyph; our art would look out of place
    }
    if (pen_x + g.width > atlas.width) {
      pen_x = 0;
      pack_y += cell_height + kGutter;
      pack_y = (pack_y + kBlockDim - 1) & ~(kBlockDim - 1);
    }
    if (pack_y + cell_height > atlas.height) {
      result.out_of_space = true;
      break;
    }
    pending.push_back({&g, pen_x, pack_y});
    // Start every glyph on a block boundary. Painting rounds outward to whole
    // DXT blocks and EncodeBlock clears the block it writes, so two glyphs
    // sharing a block column means the later one erases the tail of the
    // earlier one -- glyphs come out clipped on the right.
    pen_x += g.width + kGutter;
    pen_x = (pen_x + kBlockDim - 1) & ~(kBlockDim - 1);
  }

  const bool have_aliases = std::any_of(aliases.begin(), aliases.end(), [](uint16_t v) { return v != 0; });
  if (pending.empty() && !have_aliases) {
    result.error = "nothing to add";
    return result;
  }

  // Lay out the new image: header grows by the appended glyph attributes,
  // rounded to 4K so the atlas stays page-aligned the way the stock file has
  // it. The atlas itself is copied verbatim and then painted into.
  const uint32_t added = static_cast<uint32_t>(pending.size());
  const size_t new_glyph_array_end =
      glyph_array_offset + static_cast<size_t>(glyph_count + added) * kGlyphAttrSize;
  const uint32_t new_header_size =
      static_cast<uint32_t>((std::max<size_t>(new_glyph_array_end, header_size) + 0xFFF) & ~0xFFFu);

  // The stock files are 12 bytes longer than header_size + data_size, because
  // the atlas starts at header_size + kAtlasDataOffset (see above). Copying
  // everything past the header through verbatim keeps that trailing remainder.
  const size_t tail = original.size() - (static_cast<size_t>(header_size) + data_size);
  std::vector<uint8_t> out(static_cast<size_t>(new_header_size) + data_size + tail, 0);
  std::memcpy(out.data(), original.data(), header_size);
  std::memcpy(out.data() + new_header_size, original.data() + header_size, data_size);
  if (tail) {
    std::memcpy(out.data() + new_header_size + data_size,
                original.data() + header_size + data_size, tail);
  }

  WriteU32(out, kHeaderSizeOffset, new_header_size);
  WriteU32(out, glyph_count_offset, glyph_count + added);

  uint8_t* const atlas_bytes = out.data() + new_header_size + kAtlasDataOffset;
  const uint32_t pitch_blocks = atlas.pitch / kBlockDim;

  for (uint32_t i = 0; i < added; ++i) {
    const Pending& p = pending[i];
    const SupplementalGlyph& g = *p.glyph;
    const uint32_t index = glyph_count + i;

    // Paint the coverage bitmap into the atlas, block by block. The ink box is
    // placed `top` pixels down inside a full-height cell so the baseline lines
    // up with the stock glyphs.
    const uint32_t ink_y = p.y + static_cast<uint32_t>(std::max<int16_t>(g.top, 0));
    const uint32_t first_by = ink_y & ~(kBlockDim - 1);
    const uint32_t last_by = (ink_y + g.height + kBlockDim - 1) & ~(kBlockDim - 1);
    for (uint32_t by = first_by; by < last_by && by < atlas.height; by += kBlockDim) {
      for (uint32_t bx = p.x & ~(kBlockDim - 1); bx < p.x + g.width; bx += kBlockDim) {
        // Gather this block's 4x4 coverage window out of the glyph bitmap.
        uint8_t window[kBlockDim * kBlockDim] = {};
        for (uint32_t y = 0; y < kBlockDim; ++y) {
          for (uint32_t x = 0; x < kBlockDim; ++x) {
            const int32_t gx = static_cast<int32_t>(bx + x) - static_cast<int32_t>(p.x);
            const int32_t gy = static_cast<int32_t>(by + y) - static_cast<int32_t>(ink_y);
            if (gx >= 0 && gy >= 0 && gx < g.width && gy < g.height) {
              window[y * kBlockDim + x] = g.coverage[gy * g.width + gx];
            }
          }
        }
        uint8_t block[kBlockBytes];
        EncodeBlock(window, kBlockDim, kBlockDim, kBlockDim, block);
        if (atlas.endianness == 1) {
          SwapEndian8In16(block, kBlockBytes);
        }
        const int32_t offset = rex::graphics::texture_util::GetTiledOffset2D(
            static_cast<int32_t>(bx / kBlockDim), static_cast<int32_t>(by / kBlockDim),
            pitch_blocks, kBlockBytesLog2);
        if (offset < 0 || static_cast<size_t>(offset) + kBlockBytes > data_size) {
          continue;
        }
        std::memcpy(atlas_bytes + offset, block, kBlockBytes);
      }
    }

    // Append the GLYPH_ATTR and point the translator table at it.
    const size_t attr = glyph_array_offset + static_cast<size_t>(index) * kGlyphAttrSize;
    WriteU16(out, attr + 0, static_cast<uint16_t>(p.x));                 // tu1
    WriteU16(out, attr + 2, static_cast<uint16_t>(p.y));                 // tv1
    WriteU16(out, attr + 4, static_cast<uint16_t>(p.x + g.width));       // tu2
    WriteU16(out, attr + 6, static_cast<uint16_t>(p.y + cell_height));   // tv2
    WriteU16(out, attr + 8, static_cast<uint16_t>(g.left));              // offset
    WriteU16(out, attr + 10, static_cast<uint16_t>(g.width));            // width
    WriteU16(out, attr + 12, static_cast<uint16_t>(g.advance));          // advance
    WriteU16(out, attr + 14, 0);                                         // mask
    WriteU16(out, kTranslatorOffset + static_cast<size_t>(g.codepoint) * 2,
             static_cast<uint16_t>(index));
  }

  // Point every unmapped fullwidth form at its stock ASCII glyph. This reuses
  // the existing GLYPH_ATTR verbatim, so the character renders in the game's
  // own typeface -- at the ASCII advance, which is what the surrounding
  // halfwidth text uses anyway.
  uint32_t aliased = 0;
  for (uint32_t cp = kFullwidthFirst; cp <= kFullwidthLast && cp <= max_codepoint; ++cp) {
    if (aliases[cp] == 0) {
      continue;
    }
    WriteU16(out, kTranslatorOffset + static_cast<size_t>(cp) * 2, aliases[cp]);
    ++aliased;
  }

  result.image = std::move(out);
  result.glyphs_added = added;
  result.glyphs_aliased = aliased;
  return result;
}

}  // namespace nocturne
