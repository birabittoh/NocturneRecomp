// nocturnerecomp - ReXGlue Recompiled Project
//
// See native_renderer_texture.h.

#include "native_renderer_texture.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace nocturne {
namespace {

using namespace plume;

// A bound on what is believed out of a fetch constant: the extent fields are
// read from guest memory, and their product is what gets allocated.
constexpr uint64_t kMaxTextureBytes = 64ull << 20;

// D3D12 wants a placed footprint's rows 256 byte aligned.
constexpr uint32_t kUploadRowAlignment = 256;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// ---------------------------------------------------------------------------
// Guest memory

// How many bytes from `start` are readable, capped at `bytes`. A fetch
// constant can point at memory the guest never committed (a stage left over
// from a freed resource), so every read is checked first.
uint64_t ReadableBytes(const uint8_t* start, uint64_t bytes) {
  const uint8_t* cursor = start;
  const uint8_t* end = start + bytes;
  while (cursor < end) {
    size_t length = 0;
    rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
    if (!rex::memory::QueryProtect(const_cast<uint8_t*>(cursor), length, access))
      break;
    if (access == rex::memory::PageAccess::kNoAccess || length == 0)
      break;
    cursor += length;
  }
  return cursor >= end ? bytes : uint64_t(cursor - start);
}

// The guest's 0xE0000000 view sits one 4 KB page above physical memory. The
// host keeps that page only where the allocation granularity is 4 KB; at 64 KB
// (Windows) the E view aliases the others byte for byte. See Eternal Sonata's
// mirror for the derivation.
const uint32_t kEApertureShift = rex::memory::allocation_granularity() > 0x1000 ? 0u : 0x1000u;

// A host pointer to `bytes` of guest physical memory at `physical`. The bare
// physical address is not mapped on the host, only the three virtual
// apertures that alias it, and they are not all committed over the same
// range, so the first that spans the read wins. Null when none does.
const uint8_t* PhysicalPointer(uint8_t* base, uint32_t physical, uint64_t bytes) {
  const uint32_t offset = physical & 0x1FFFFFFFu;
  for (uint32_t aperture : {0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    const uint32_t shifted = aperture == 0xE0000000u ? offset - kEApertureShift : offset;
    const uint8_t* candidate = base + (aperture | shifted);
    if (ReadableBytes(candidate, bytes) >= bytes)
      return candidate;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Formats

// How a guest unit becomes host texels. Everything that is not already in a
// host format is widened to R8G8B8A8 with X in R, Y in G, Z in B and W in A,
// which leaves the channel order to the fetch swizzle.
enum class Expand {
  kNone,
  k1_5_5_5,
  k5_6_5,
  k4_4_4_4,
};

struct FormatInfo {
  RenderFormat host = RenderFormat::UNKNOWN;
  uint32_t block_bytes = 0;  // one addressable unit of guest memory
  uint32_t block = 1;        // its edge in texels: 4 for DXT
  Expand expand = Expand::kNone;
};

uint32_t HostBlockBytes(const FormatInfo& info) {
  return info.expand == Expand::kNone ? info.block_bytes : 4;
}

bool MapTextureFormat(uint32_t format, FormatInfo& out) {
  switch (format) {
    case 3:  // k_1_5_5_5
      out = {RenderFormat::R8G8B8A8_UNORM, 2, 1, Expand::k1_5_5_5};
      return true;
    case 4:  // k_5_6_5
      out = {RenderFormat::R8G8B8A8_UNORM, 2, 1, Expand::k5_6_5};
      return true;
    case 6:  // k_8_8_8_8
      out = {RenderFormat::R8G8B8A8_UNORM, 4, 1};
      return true;
    case 15:  // k_4_4_4_4
      out = {RenderFormat::R8G8B8A8_UNORM, 2, 1, Expand::k4_4_4_4};
      return true;
    case 18:  // k_DXT1
      out = {RenderFormat::BC1_UNORM, 8, 4};
      return true;
    case 19:  // k_DXT2_3
      out = {RenderFormat::BC2_UNORM, 16, 4};
      return true;
    case 20:  // k_DXT4_5
      out = {RenderFormat::BC3_UNORM, 16, 4};
      return true;
    default:
      return false;
  }
}

// The fetch constant's endian mode, applied uniformly over a whole unit (a DXT
// block included, index bytes and all), as the texture hardware does.
void EndianSwap(uint8_t* data, uint32_t bytes, uint32_t endianness) {
  switch (endianness) {
    case 1:  // k_8in16
      for (uint32_t i = 0; i + 1 < bytes; i += 2)
        std::swap(data[i], data[i + 1]);
      break;
    case 2:  // k_8in32
      for (uint32_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case 3:  // k_16in32
      for (uint32_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

// n bits widened to eight, replicating the top bits down so all ones stays 255.
uint8_t Widen(uint32_t value, uint32_t bits) {
  value <<= 8 - bits;
  return uint8_t(value | (value >> bits));
}

void ExpandTexel(uint16_t value, Expand expand, uint8_t* out) {
  switch (expand) {
    case Expand::k1_5_5_5:
      out[0] = Widen(value & 0x1Fu, 5);
      out[1] = Widen((value >> 5) & 0x1Fu, 5);
      out[2] = Widen((value >> 10) & 0x1Fu, 5);
      out[3] = (value & 0x8000u) != 0 ? 255 : 0;
      break;
    case Expand::k5_6_5:
      out[0] = Widen(value & 0x1Fu, 5);
      out[1] = Widen((value >> 5) & 0x3Fu, 6);
      out[2] = Widen((value >> 11) & 0x1Fu, 5);
      out[3] = 255;
      break;
    case Expand::k4_4_4_4:
      out[0] = uint8_t((value & 0xFu) * 17);
      out[1] = uint8_t(((value >> 4) & 0xFu) * 17);
      out[2] = uint8_t(((value >> 8) & 0xFu) * 17);
      out[3] = uint8_t(((value >> 12) & 0xFu) * 17);
      break;
    case Expand::kNone:
      break;
  }
}

// ---------------------------------------------------------------------------
// Tiling: the Xenos 2D tiled address in units of one block, from xenia's
// texture_util.cc (as Eternal Sonata's mirror carries it).

uint32_t TiledBlockOffset(uint32_t bx, uint32_t by, uint32_t pitch_macro_tiles,
                          uint32_t block_bytes_log2) {
  const uint32_t outer = ((by >> 5) * pitch_macro_tiles + (bx >> 5)) << 6;
  const uint32_t inner = (((by >> 1) & 7) << 3) | (bx & 7);
  const uint32_t oib = (outer | inner) << block_bytes_log2;
  const uint32_t bank = (by >> 4) & 1;
  const uint32_t pipe = ((bx >> 3) & 3) ^ (((by >> 3) & 1) << 1);
  return ((by & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}

uint32_t Log2(uint32_t value) {
  uint32_t bits = 0;
  while ((1u << bits) < value)
    ++bits;
  return bits;
}

struct Layout {
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
  uint32_t pitch_blocks = 0;
  uint64_t source_bytes = 0;  // what the read covers, and the hash with it
};

Layout ComputeLayout(const TextureFetch& fetch, const FormatInfo& info) {
  Layout layout;
  layout.width_blocks = (fetch.width + info.block - 1) / info.block;
  layout.height_blocks = (fetch.height + info.block - 1) / info.block;
  const uint32_t pitch = fetch.pitch > fetch.width ? fetch.pitch : fetch.width;
  layout.pitch_blocks = (pitch + info.block - 1) / info.block;
  if (fetch.tiled) {
    // Whole 32x32 block macro tiles: the swizzle's highest offset is not the
    // last block of the last row.
    const uint32_t macro_x = layout.pitch_blocks / 32 > 1 ? layout.pitch_blocks / 32 : 1;
    const uint32_t macro_y = (layout.height_blocks + 31) / 32;
    layout.source_bytes = uint64_t(macro_x) * macro_y * 32 * 32 * info.block_bytes;
  } else {
    layout.source_bytes = uint64_t(layout.pitch_blocks) * info.block_bytes * layout.height_blocks;
  }
  return layout;
}

// Untile and byte swap level 0 into rows of `row_bytes`, widening on the way
// when the format asks.
void ReadTexels(const uint8_t* source, const TextureFetch& fetch, const FormatInfo& info,
                const Layout& layout, uint32_t row_bytes, uint8_t* out) {
  const uint32_t pitch_macro_tiles = layout.pitch_blocks / 32 > 1 ? layout.pitch_blocks / 32 : 1;
  const uint32_t block_log2 = Log2(info.block_bytes);
  uint8_t unit[16];
  for (uint32_t by = 0; by < layout.height_blocks; ++by) {
    uint8_t* row = out + size_t(by) * row_bytes;
    for (uint32_t bx = 0; bx < layout.width_blocks; ++bx) {
      const uint64_t offset =
          fetch.tiled ? TiledBlockOffset(bx, by, pitch_macro_tiles, block_log2)
                      : (uint64_t(by) * layout.pitch_blocks + bx) * info.block_bytes;
      if (offset + info.block_bytes > layout.source_bytes)
        continue;
      std::memcpy(unit, source + offset, info.block_bytes);
      EndianSwap(unit, info.block_bytes, fetch.endianness);
      if (info.expand == Expand::kNone) {
        std::memcpy(row + size_t(bx) * info.block_bytes, unit, info.block_bytes);
      } else {
        uint16_t value;
        std::memcpy(&value, unit, 2);
        ExpandTexel(value, info.expand, row + size_t(bx) * 4);
      }
    }
  }
}

// FNV-1a over four lanes, so the multiplies do not wait on each other. Covers
// every byte: a sprite sheet the game rewrites a cell at a time differs from
// its last version in a few hundred bytes.
uint64_t HashSource(const uint8_t* source, uint64_t bytes) {
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t lanes[4] = {1469598103934665603ull, 1469598103934665603ull ^ bytes,
                       1469598103934665603ull, 1469598103934665603ull ^ (bytes << 17)};
  uint64_t offset = 0;
  for (; offset + 32 <= bytes; offset += 32) {
    uint64_t chunk[4];
    std::memcpy(chunk, source + offset, sizeof(chunk));
    for (uint32_t i = 0; i < 4; ++i)
      lanes[i] = (lanes[i] ^ chunk[i]) * kPrime;
  }
  uint64_t hash = bytes;
  for (uint32_t i = 0; i < 4; ++i)
    hash = (hash ^ lanes[i]) * kPrime;
  for (; offset < bytes; ++offset)
    hash = (hash ^ source[offset]) * kPrime;
  return hash;
}

// ---------------------------------------------------------------------------
// The cache

struct MirroredTexture {
  std::unique_ptr<RenderTexture> texture;
  uint64_t hash = 0;
  // The frame serial the source was last hashed in: at most once a frame.
  uint64_t hashed_frame = ~0ull;
  bool failed = false;
  // Whether the host texture is a mod's image, which has its own extent and
  // format. See the replacement section.
  bool replaced = false;
  uint32_t replaced_width = 0;
  uint32_t replaced_height = 0;
};

// Everything that changes how the bytes are read is in the key, so a texture
// whose contents change at an unchanged address keeps its host texture and is
// refreshed in place.
struct TextureKey {
  uint32_t address, format, width, height, pitch, flags;
  bool operator==(const TextureKey&) const = default;
};

struct TextureKeyHash {
  size_t operator()(const TextureKey& k) const {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t v : {k.address, k.format, k.width, k.height, k.pitch, k.flags})
      h = (h ^ v) * 0x100000001B3ull;
    return size_t(h);
  }
};

std::unordered_map<TextureKey, MirroredTexture, TextureKeyHash> g_textures;
std::unordered_map<uint32_t, std::unique_ptr<RenderSampler>> g_samplers;

// Staging buffers, per frame slot: the copies read them until the frame retires.
std::vector<std::unique_ptr<RenderBuffer>> g_staging[kFramesInFlight];
uint32_t g_slot = 0;

bool g_format_reported[64] = {};
uint64_t g_uploads = 0;
uint64_t g_refreshes = 0;

bool Upload(RenderCommandList* commands, const TextureFetch& fetch, const FormatInfo& info,
            const Layout& layout, const uint8_t* source, MirroredTexture& entry) {
  RenderDevice* device = PlumeDevice();
  const uint32_t row_bytes =
      AlignUp(layout.width_blocks * HostBlockBytes(info), kUploadRowAlignment);
  const uint64_t bytes = uint64_t(row_bytes) * layout.height_blocks;
  auto staging = device->createBuffer(RenderBufferDesc::UploadBuffer(bytes));
  if (!staging)
    return false;
  auto* mapped = static_cast<uint8_t*>(staging->map());
  if (mapped == nullptr)
    return false;
  std::memset(mapped, 0, bytes);
  ReadTexels(source, fetch, info, layout, row_bytes, mapped);
  staging->unmap();

  if (!entry.texture) {
    entry.texture =
        device->createTexture(RenderTextureDesc::Texture2D(fetch.width, fetch.height, 1, info.host));
    if (!entry.texture)
      return false;
  }
  // The row length is in texels, which for a block format is blocks times four.
  const uint32_t row_texels = row_bytes / HostBlockBytes(info) * info.block;
  commands->barriers(RenderBarrierStage::COPY,
                     RenderTextureBarrier(entry.texture.get(), RenderTextureLayout::COPY_DEST));
  commands->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(entry.texture.get()),
      RenderTextureCopyLocation::PlacedFootprint(staging.get(), info.host, fetch.width,
                                                 fetch.height, 1, row_texels));
  commands->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(entry.texture.get(), RenderTextureLayout::SHADER_READ));
  g_staging[g_slot].push_back(std::move(staging));
  return true;
}

// ---------------------------------------------------------------------------
// Replacement
//
// The SDK's content hash scheme, as Eternal Sonata's mirror carries it:
// mods/<name>/textures/<hash16>.png|.dds, keyed by an XXH3 of level 0's guest
// bytes (tiled, big endian, as the game loaded them), so the key follows a
// texture's contents rather than its address. A match replaces the host
// texture at the image's own extent. The guest keeps its fetch constant and
// the shaders sample with normalized coordinates, so nothing else has to know.
//
// Colour formats only, and a single mip level (the sampler never leaves level
// 0). There is no dump path here: the SDK's own dumper names files the same
// way.

using rex::graphics::TextureReplacement;
using rex::graphics::TextureReplacementData;

constexpr uint32_t kMaxReplacementExtent = 16384;

std::unique_ptr<TextureReplacement> g_replacement;
bool g_replacement_initialized = false;
uint64_t g_replaced = 0;

// Textures swapped out for a different extent or format. Descriptor sets the
// draw path cached still point at them, so they live until shutdown.
std::vector<std::unique_ptr<RenderTexture>> g_retired;

bool ReplaceableFormat(uint32_t format) {
  return format == 3 || format == 4 || format == 6 || (format >= 18 && format <= 20);
}

// Null when no enabled mod ships textures, so the hash is only paid for when
// something can use it.
TextureReplacement* ActiveReplacement() {
  if (!g_replacement_initialized) {
    g_replacement_initialized = true;
    if (rex::Runtime* runtime = rex::Runtime::instance())
      g_replacement = std::make_unique<TextureReplacement>(runtime->ModOverlayRoots("textures"),
                                                           runtime->ModDumpRoot());
  }
  if (!g_replacement || g_replacement->mod_roots().empty())
    return nullptr;
  return g_replacement.get();
}

// Upload a replacement's RGBA8 pixels, into the entry's texture when it already has
// that extent and format, as after a refresh.
bool UploadReplacement(RenderCommandList* commands, const TextureReplacementData& data,
                       MirroredTexture& entry) {
  RenderDevice* device = PlumeDevice();
  const uint32_t row_bytes = AlignUp(data.width * 4, kUploadRowAlignment);
  const uint64_t bytes = uint64_t(row_bytes) * data.height;
  auto staging = device->createBuffer(RenderBufferDesc::UploadBuffer(bytes));
  if (!staging)
    return false;
  auto* mapped = static_cast<uint8_t*>(staging->map());
  if (mapped == nullptr)
    return false;
  for (uint32_t y = 0; y < data.height; ++y)
    std::memcpy(mapped + size_t(y) * row_bytes, data.pixels.data() + size_t(y) * data.width * 4,
                size_t(data.width) * 4);
  staging->unmap();

  const bool fits = entry.texture && entry.replaced && entry.replaced_width == data.width &&
                    entry.replaced_height == data.height;
  if (!fits) {
    auto created = device->createTexture(RenderTextureDesc::Texture2D(
        data.width, data.height, 1, RenderFormat::R8G8B8A8_UNORM));
    if (!created)
      return false;
    if (entry.texture)
      g_retired.push_back(std::move(entry.texture));
    entry.texture = std::move(created);
  }
  commands->barriers(RenderBarrierStage::COPY,
                     RenderTextureBarrier(entry.texture.get(), RenderTextureLayout::COPY_DEST));
  commands->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(entry.texture.get()),
      RenderTextureCopyLocation::PlacedFootprint(staging.get(), RenderFormat::R8G8B8A8_UNORM,
                                                 data.width, data.height, 1, row_bytes / 4));
  commands->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(entry.texture.get(), RenderTextureLayout::SHADER_READ));
  g_staging[g_slot].push_back(std::move(staging));
  entry.replaced = true;
  entry.replaced_width = data.width;
  entry.replaced_height = data.height;
  return true;
}

// The mod image for `source`'s level 0, or null.
const TextureReplacementData* FindReplacementImage(const TextureFetch& fetch,
                                                   const uint8_t* source, uint64_t bytes,
                                                   uint64_t& hash) {
  if (!ReplaceableFormat(fetch.format))
    return nullptr;
  TextureReplacement* replacement = ActiveReplacement();
  if (replacement == nullptr)
    return nullptr;
  hash = TextureReplacement::HashGuestData(source, size_t(bytes));
  const TextureReplacementData* image = replacement->FindReplacement(hash);
  if (image == nullptr || image->width == 0 || image->height == 0 ||
      image->width > kMaxReplacementExtent || image->height > kMaxReplacementExtent ||
      image->pixels.size() < size_t(image->width) * image->height * 4)
    return nullptr;
  return image;
}

RenderTextureAddressMode MapClamp(uint32_t clamp) {
  switch (clamp) {
    case 0:
      return RenderTextureAddressMode::WRAP;
    case 1:
      return RenderTextureAddressMode::MIRROR;
    case 2:  // clamp to the last texel
      return RenderTextureAddressMode::CLAMP;
    case 3:
      return RenderTextureAddressMode::MIRROR_ONCE;
    default:  // the half way and border modes
      return RenderTextureAddressMode::BORDER;
  }
}

}  // namespace

TextureFetch DecodeTextureFetch(const uint32_t words[6]) {
  TextureFetch out;
  out.type = words[0] & 0x3u;
  out.clamp_x = (words[0] >> 10) & 0x7u;
  out.clamp_y = (words[0] >> 13) & 0x7u;
  out.pitch = ((words[0] >> 22) & 0x1FFu) << 5;
  out.tiled = ((words[0] >> 31) & 1u) != 0;
  out.format = words[1] & 0x3Fu;
  out.endianness = (words[1] >> 6) & 0x3u;
  out.raw_base_address = ((words[1] >> 12) & 0xFFFFFu) << 12;
  out.base_address = (out.raw_base_address & 0x1FFFFFFFu) +
                     (((out.raw_base_address >> 20) + 512u) & 0x1000u);
  out.width = (words[2] & 0x1FFFu) + 1u;
  out.height = ((words[2] >> 13) & 0x1FFFu) + 1u;
  out.swizzle = (words[3] >> 1) & 0xFFFu;
  out.mag_filter = (words[3] >> 19) & 0x3u;
  out.min_filter = (words[3] >> 21) & 0x3u;
  out.packed_mips = ((words[5] >> 11) & 1u) != 0;
  return out;
}

bool ReadTextureFetch(const uint8_t* base, uint32_t address, TextureFetch& out) {
  uint32_t words[6];
  for (uint32_t i = 0; i < 6; ++i)
    words[i] = Load32(base, address + 4 * i);
  out = DecodeTextureFetch(words);
  return out.type == 2 && out.raw_base_address != 0;
}

RenderTexture* MirrorTexture(RenderCommandList* commands, uint8_t* base,
                             const TextureFetch& fetch) {
  FormatInfo info;
  if (!MapTextureFormat(fetch.format, info)) {
    if (!g_format_reported[fetch.format]) {
      g_format_reported[fetch.format] = true;
      REXLOG_WARN("native_renderer: texture format {} is not mirrored ({}x{} at {:08X})",
                  fetch.format, fetch.width, fetch.height, fetch.base_address);
    }
    return nullptr;
  }

  const TextureKey key{fetch.base_address, fetch.format | fetch.endianness << 8,
                       fetch.width,        fetch.height,
                       fetch.pitch,        uint32_t(fetch.tiled) | uint32_t(fetch.packed_mips) << 1};
  auto [it, inserted] = g_textures.try_emplace(key);
  MirroredTexture& entry = it->second;
  if (entry.failed)
    return nullptr;

  // Hashed at most once a frame. Writes the guest makes between two draws of
  // the same frame are not seen until the next one.
  const uint64_t frame = PlumeFrameSerial();
  if (!inserted && entry.hashed_frame == frame)
    return entry.texture.get();
  entry.hashed_frame = frame;

  const Layout layout = ComputeLayout(fetch, info);
  const uint8_t* source = layout.source_bytes <= kMaxTextureBytes
                              ? PhysicalPointer(base, fetch.raw_base_address, layout.source_bytes)
                              : nullptr;
  if (source == nullptr) {
    entry.failed = true;
    REXLOG_WARN("native_renderer: texture {}x{} format {} at {:08X} ({} bytes) is not readable",
                fetch.width, fetch.height, fetch.format, fetch.base_address, layout.source_bytes);
    return nullptr;
  }
  if (fetch.packed_mips && (fetch.width <= 16 || fetch.height <= 16)) {
    // Level 0 lives inside the packed mip tail there, at an offset this does
    // not compute (Eternal Sonata's PackedLevel0Blocks). Reported, then read
    // from the base like any other.
    static bool reported = false;
    if (!reported) {
      reported = true;
      REXLOG_WARN("native_renderer: {}x{} texture at {:08X} has packed mips; level 0 may be offset",
                  fetch.width, fetch.height, fetch.base_address);
    }
  }

  const uint64_t hash = HashSource(source, layout.source_bytes);
  if (entry.texture && hash == entry.hash)
    return entry.texture.get();
  const bool refresh = entry.texture != nullptr;

  uint64_t content_hash = 0;
  if (const TextureReplacementData* image =
          FindReplacementImage(fetch, source, layout.source_bytes, content_hash)) {
    if (UploadReplacement(commands, *image, entry)) {
      entry.hash = hash;
      if (++g_replaced <= 32)
        REXLOG_INFO("native_renderer: replaced {}x{} fmt {} texture {:016x} with a {}x{} image",
                    fetch.width, fetch.height, fetch.format, content_hash, image->width,
                    image->height);
      return entry.texture.get();
    }
  }
  // The decode only refreshes in place a texture it made itself.
  if (entry.replaced) {
    if (entry.texture)
      g_retired.push_back(std::move(entry.texture));
    entry.replaced = false;
  }
  if (!Upload(commands, fetch, info, layout, source, entry)) {
    entry.failed = true;
    REXLOG_WARN("native_renderer: could not upload texture {}x{} format {} at {:08X}", fetch.width,
                fetch.height, fetch.format, fetch.base_address);
    return nullptr;
  }
  entry.hash = hash;
  if (refresh) {
    ++g_refreshes;
  } else if (++g_uploads <= 32) {
    REXLOG_INFO(
        "native_renderer: texture {}x{} format {} {} pitch {} endian {} swizzle {:03X} at {:08X}",
        fetch.width, fetch.height, fetch.format, fetch.tiled ? "tiled" : "linear", fetch.pitch,
        fetch.endianness, fetch.swizzle, fetch.base_address);
  }
  return entry.texture.get();
}

RenderSampler* MirrorSampler(const TextureFetch& fetch) {
  const uint32_t key = fetch.clamp_x | fetch.clamp_y << 3 | fetch.mag_filter << 6 |
                       fetch.min_filter << 8;
  std::unique_ptr<RenderSampler>& sampler = g_samplers[key];
  if (!sampler) {
    // Filter 0 is point and 1 linear; 2 (basemap) and 3 (use the fetch
    // constant, which this is) are taken as linear.
    RenderSamplerDesc desc;
    desc.magFilter = fetch.mag_filter == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
    desc.minFilter = fetch.min_filter == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
    desc.mipmapMode = RenderMipmapMode::NEAREST;
    desc.addressU = MapClamp(fetch.clamp_x);
    desc.addressV = MapClamp(fetch.clamp_y);
    desc.addressW = RenderTextureAddressMode::CLAMP;
    desc.borderColor = RenderBorderColor::TRANSPARENT_BLACK;
    desc.maxLOD = 0.0f;
    sampler = PlumeDevice()->createSampler(desc);
  }
  return sampler.get();
}

void TextureMirrorBeginFrame(uint32_t slot) {
  g_slot = slot;
  g_staging[slot].clear();
}

void ShutdownTextureMirror() {
  REXLOG_INFO("native_renderer: texture mirror: {} textures, {} uploads, {} refreshes",
              g_textures.size(), g_uploads, g_refreshes);
  g_textures.clear();
  g_retired.clear();
  g_replacement.reset();
  g_samplers.clear();
  for (auto& staging : g_staging)
    staging.clear();
}

}  // namespace nocturne
