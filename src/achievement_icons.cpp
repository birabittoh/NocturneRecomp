// nocturnerecomp - build guest textures for the achievement icons and register
// them in the game's UI image bank. See achievement_icons.h for why.

#include "achievement_icons.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/achievement_store.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xmemory.h>
#include <rex/ui/image_decode.h>

namespace nocturne {

namespace {

// ---------------------------------------------------------------------------
// Guest structures
// ---------------------------------------------------------------------------
//
// Image bank (built by sub_825CE9F8, searched by sub_825CEB68). A flat array
// with the count in the first dword; each entry is 4 dwords, and only two of
// them matter to the lookup:
//
//   bank+0                 entry count
//   bank+4  + 16*i         (copied out of the .xpr; unused by the lookup)
//   bank+8  + 16*i         name pointer (plain ASCII, NUL-terminated)
//   bank+12 + 16*i         zero
//   bank+16 + 16*i         image object -- what the lookup returns
//
// The bank is allocated at 16388 bytes = 4 + 16 * 1024, so it has room for 1024
// entries and the game only ever fills it with what uiresource.xpr contains.
// Appending is therefore just "write the next slot, bump the count".
constexpr uint32_t kBankEntryStride = 16;
constexpr uint32_t kBankNameOffset = 8;
constexpr uint32_t kBankImageOffset = 16;
constexpr uint32_t kBankCapacity = 1024;

// Image object (built by sub_825D01E8), 20 bytes:
//   +0  vtable   +4 width   +8 height   +12 "is 8888" flag   +16 D3D texture
// Rather than hardcode the vtable, every field but these is cloned from an
// image the game itself built -- which also keeps this working under the title
// update without a second set of addresses.
constexpr uint32_t kImageObjectSize = 20;
constexpr uint32_t kImageWidthOffset = 4;
constexpr uint32_t kImageHeightOffset = 8;
constexpr uint32_t kImageIs8888Offset = 12;
constexpr uint32_t kImageTextureOffset = 16;

// D3DTexture: the standard Xbox 360 header (Common, ReferenceCount, Fence,
// ReadFence, Identifier, BaseFlush, MipFlush) followed by the six-dword GPU
// texture fetch constant. sub_82522838 reads the description straight out of
// that fetch constant, at exactly these dword indices, which is what pins the
// offset down.
constexpr uint32_t kTextureHeaderSize = 0x34;
constexpr uint32_t kTextureFetchOffset = 0x1C;

// Linear (untiled) texture rows must be 256-byte aligned; the fetch constant's
// pitch field is that row pitch in *pixels*, shifted right by 5.
constexpr uint32_t kLinearRowAlignment = 256;
constexpr uint32_t kBytesPerPixel = 4;

// Texture pixels are allocated from the 0xA0000000 physical heap (64KB pages)
// rather than the default one SystemHeapAlloc picks.
//
// This is not arbitrary. The 0xE0000000 physical heap is initialized with a
// host_address_offset of 0x1000 -- it is the one heap whose guest-virtual to
// physical mapping is *not* a plain mask, and the offset is exactly one 4KB
// page. Pixels written through the virtual address and sampled through the
// physical address in the fetch constant then disagree by a page, which is 16
// rows of a 64x64 RGBA icon and reads on screen as the top of every image being
// chopped off. Allocating where host_address_offset is 0 removes the ambiguity
// instead of trying to compensate for it.
constexpr uint32_t kPhysicalHeapPageSize = 64 * 1024;
constexpr uint32_t kTexturePageAlignment = 0x1000;

// Swizzle is 12 bits, 3 per component, selecting source components 0..3 for
// destination R/G/B/A. Straight RGBA is 0, 1, 2, 3.
constexpr uint32_t kSwizzleRGBA = 0u | (1u << 3) | (2u << 6) | (3u << 9);

uint32_t Read32BE(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

void Write32BE(uint8_t* base, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(base + address, value);
}

// Replaces [lsb, lsb+width) of a guest dword, leaving every other bit alone.
void PatchField(uint8_t* base, uint32_t address, uint32_t lsb, uint32_t width, uint32_t value) {
  const uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : (((1u << width) - 1u) << lsb);
  const uint32_t current = Read32BE(base, address);
  Write32BE(base, address, (current & ~mask) | ((value << lsb) & mask));
}

// The achievement's icon as tightly-packed RGBA8, straight out of the title's
// XDBF metadata -- the same source the SDK's AchievementIconCache uses for the
// ImGui overlay, so anything that shows up there shows up here.
std::vector<uint8_t> DecodeIcon(const rex::system::AchievementInfo& achievement, int& width,
                                int& height) {
  auto* ks = rex::system::kernel_state();
  if (!ks || achievement.image_id == 0) {
    return {};
  }
  const auto db = ks->title_xdbf();
  if (!db.is_valid()) {
    return {};
  }
  const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage,
                                 static_cast<uint64_t>(achievement.image_id));
  if (!block) {
    return {};
  }
  return rex::ui::DecodeImageRGBA(block.buffer, block.size, width, height);
}

// Copies the decoded icon into guest memory as a linear k_8_8_8_8 surface.
//
// The GPU reads each texel as one dword with the fetch constant's endianness
// applied, so storing big-endian (as everything guest-side is) and asking for
// k8in32 leaves the GPU with component 0 in the low byte. With a straight RGBA
// swizzle that makes component 0 red, hence the ABGR dword packed below.
void WritePixels(uint8_t* base, uint32_t data_address, const std::vector<uint8_t>& rgba,
                 uint32_t width, uint32_t height, uint32_t row_pitch_bytes) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* texel = rgba.data() + (y * width + x) * 4;
      const uint32_t packed = (static_cast<uint32_t>(texel[3]) << 24) |
                              (static_cast<uint32_t>(texel[2]) << 16) |
                              (static_cast<uint32_t>(texel[1]) << 8) |
                              static_cast<uint32_t>(texel[0]);
      Write32BE(base, data_address + y * row_pitch_bytes + x * kBytesPerPixel, packed);
    }
  }
}

// Reserves GPU-visible guest memory for one icon's pixels. See the note on
// kPhysicalHeapPageSize for why this deliberately avoids SystemHeapAlloc's
// default physical heap.
uint32_t AllocatePixels(rex::memory::Memory* memory, uint32_t size) {
  auto* heap = memory->LookupHeapByType(/*physical=*/true, kPhysicalHeapPageSize);
  if (heap) {
    uint32_t address = 0;
    if (heap->Alloc(size, kTexturePageAlignment,
                    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                    /*top_down=*/false, &address)) {
      return address;
    }
  }
  REXLOG_WARN("[achievement_icons] 64KB physical heap unavailable; falling back to the default "
              "one, where the page offset may clip the top of each icon");
  return memory->SystemHeapAlloc(size, kTexturePageAlignment, rex::memory::kSystemHeapPhysical);
}

// Builds a guest D3DTexture for an icon by cloning `template_texture` and
// patching only what actually differs.
//
// Cloning rather than constructing from scratch is deliberate: the fetch
// constant carries a dozen fields (type, per-component signs, clamp modes, the
// filter set, border behaviour) that have nothing to do with which image this
// is, and inheriting them from a texture the game's own renderer is already
// drawing is far safer than deriving each one. Only the fields that describe
// *this* surface are overwritten.
uint32_t CreateTexture(uint8_t* base, rex::memory::Memory* memory, uint32_t template_texture,
                       const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  const uint32_t row_pitch_bytes =
      ((width * kBytesPerPixel + kLinearRowAlignment - 1) / kLinearRowAlignment) *
      kLinearRowAlignment;
  const uint32_t data_size = row_pitch_bytes * height;

  const uint32_t data_address = AllocatePixels(memory, data_size);
  if (!data_address) {
    return 0;
  }
  const uint32_t header = memory->SystemHeapAlloc(kTextureHeaderSize);
  if (!header) {
    return 0;
  }

  std::memcpy(base + header, base + template_texture, kTextureHeaderSize);
  WritePixels(base, data_address, rgba, width, height, row_pitch_bytes);

  const uint32_t fetch = header + kTextureFetchOffset;
  const uint32_t row_pitch_pixels = row_pitch_bytes / kBytesPerPixel;

  // dword_0: linear layout, and the row pitch this surface actually has.
  PatchField(base, fetch + 0, 22, 9, row_pitch_pixels >> 5);
  PatchField(base, fetch + 0, 31, 1, 0);  // tiled

  // dword_1: format, endianness, and where the pixels live.
  PatchField(base, fetch + 4, 0, 6, static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_8_8_8_8));
  PatchField(base, fetch + 4, 6, 2, static_cast<uint32_t>(rex::graphics::xenos::Endian::k8in32));
  PatchField(base, fetch + 4, 10, 1, 0);  // stacked
  const uint32_t physical_address = memory->GetPhysicalAddress(data_address);
  PatchField(base, fetch + 4, 12, 20, physical_address >> 12);

  // dword_2: size, stored with 1 subtracted from each component.
  Write32BE(base, fetch + 8, (width - 1) | ((height - 1) << 13));

  // dword_3: straight RGBA, normalized components.
  PatchField(base, fetch + 12, 0, 1, 0);  // num_format: fraction
  PatchField(base, fetch + 12, 1, 12, kSwizzleRGBA);

  // dword_4/5: no mips, and a plain 2D surface.
  PatchField(base, fetch + 16, 2, 4, 0);  // mip_min_level
  PatchField(base, fetch + 16, 6, 4, 0);  // mip_max_level
  PatchField(base, fetch + 20, 9, 2,
             static_cast<uint32_t>(rex::graphics::xenos::DataDimension::k2DOrStacked));
  PatchField(base, fetch + 20, 11, 1, 0);  // packed_mips
  PatchField(base, fetch + 20, 12, 20, 0);  // mip_address

  // The fetch constant is the one thing here that cannot be checked by reading
  // the code -- half of it is inherited from the template texture, and a single
  // wrong field shows up as a subtly mis-sampled image rather than an error. Log
  // both so the inherited fields are visible next to the patched ones.
  // Only for the first one -- they are all built identically, so twelve copies
  // would say nothing the first does not. Kept because the fetch constant is the
  // one thing here that cannot be checked by reading the code: half of it is
  // inherited from the template, and a wrong field shows up as a subtly
  // mis-sampled image rather than an error. Note guest and physical differing by
  // more than the heap base means the pixels and the sampler disagree -- that is
  // what the 0xE0000000 heap's page offset used to cause.
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_DEBUG("[achievement_icons] {}x{} guest {:08X} physical {:08X} pitch {}B", width, height,
                 data_address, physical_address, row_pitch_bytes);
    for (uint32_t i = 0; i < 6; ++i) {
      REXLOG_DEBUG("[achievement_icons]   fetch dword_{}: template {:08X} -> ours {:08X}", i,
                   Read32BE(base, template_texture + kTextureFetchOffset + i * 4),
                   Read32BE(base, fetch + i * 4));
    }
  }

  return header;
}

// Stages a NUL-terminated ASCII string in guest memory and returns its address.
// The bank holds these permanently, so each gets its own small allocation.
uint32_t AllocateName(uint8_t* base, rex::memory::Memory* memory, const std::string& name) {
  const uint32_t address = memory->SystemHeapAlloc(static_cast<uint32_t>(name.size()) + 1);
  if (!address) {
    return 0;
  }
  std::memcpy(base + address, name.data(), name.size());
  base[address + name.size()] = 0;
  return address;
}

// Whether the bank already carries a given name, so a second call is a no-op.
bool BankContains(const uint8_t* base, uint32_t bank, const std::string& name) {
  const uint32_t count = Read32BE(base, bank);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t name_ptr = Read32BE(base, bank + kBankNameOffset + i * kBankEntryStride);
    if (name_ptr && std::strncmp(reinterpret_cast<const char*>(base + name_ptr), name.c_str(),
                                 name.size() + 1) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

uint32_t RegisterAchievementIcons(uint8_t* base, uint32_t bank,
                                  std::span<const rex::system::AchievementInfo> achievements) {
  auto* ks = rex::system::kernel_state();
  if (!base || !bank || !ks || !ks->memory()) {
    return 0;
  }
  auto* memory = ks->memory();

  // Every new texture header is cloned from one the game built, so there has to
  // be at least one entry already in the bank to clone from. There always is by
  // the time any screen runs -- the bank is filled during UI manager init.
  const uint32_t existing_count = Read32BE(base, bank);
  if (existing_count == 0) {
    REXLOG_WARN("[achievement_icons] image bank is empty; nothing to model new textures on");
    return 0;
  }
  const uint32_t template_image = Read32BE(base, bank + kBankImageOffset);
  const uint32_t template_texture =
      template_image ? Read32BE(base, template_image + kImageTextureOffset) : 0;
  if (!template_image || !template_texture) {
    REXLOG_WARN("[achievement_icons] first bank entry has no texture to model new ones on");
    return 0;
  }

  uint32_t added = 0;
  for (size_t i = 0; i < achievements.size(); ++i) {
    const std::string name = "ACHIEVEMENT_" + std::to_string(i + 1);
    if (BankContains(base, bank, name)) {
      continue;
    }

    const uint32_t count = Read32BE(base, bank);
    if (count >= kBankCapacity) {
      REXLOG_WARN("[achievement_icons] image bank is full at {} entries", count);
      break;
    }

    int width = 0;
    int height = 0;
    const std::vector<uint8_t> rgba = DecodeIcon(achievements[i], width, height);
    if (rgba.empty() || width <= 0 || height <= 0) {
      REXLOG_WARN("[achievement_icons] no XDBF image for achievement {} (image_id {})",
                  achievements[i].id, achievements[i].image_id);
      continue;
    }

    const uint32_t texture = CreateTexture(base, memory, template_texture, rgba,
                                           static_cast<uint32_t>(width),
                                           static_cast<uint32_t>(height));
    if (!texture) {
      REXLOG_WARN("[achievement_icons] out of guest memory building {}", name);
      break;
    }

    const uint32_t image = memory->SystemHeapAlloc(kImageObjectSize);
    const uint32_t name_address = AllocateName(base, memory, name);
    if (!image || !name_address) {
      REXLOG_WARN("[achievement_icons] out of guest memory publishing {}", name);
      break;
    }
    // Clone a real image object so the vtable comes from the game rather than a
    // hardcoded address, then describe our own surface.
    std::memcpy(base + image, base + template_image, kImageObjectSize);
    Write32BE(base, image + kImageWidthOffset, static_cast<uint32_t>(width));
    Write32BE(base, image + kImageHeightOffset, static_cast<uint32_t>(height));
    base[image + kImageIs8888Offset] = 1;  // k_8_8_8_8, which is what we built
    Write32BE(base, image + kImageTextureOffset, texture);

    const uint32_t entry = bank + count * kBankEntryStride;
    Write32BE(base, entry + kBankNameOffset, name_address);
    Write32BE(base, entry + kBankNameOffset + 4, 0);
    Write32BE(base, entry + kBankImageOffset, image);
    Write32BE(base, bank, count + 1);

    REXLOG_INFO("[achievement_icons] published {} ({}x{}) as image {:08X} texture {:08X}", name,
                width, height, image, texture);
    ++added;
  }

  return added;
}

}  // namespace nocturne
