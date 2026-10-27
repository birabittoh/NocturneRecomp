// nocturnerecomp - ReXGlue Recompiled Project
//
// Driver for the shared pack reader; see guest_shaders.h and
// ext/KryptonRecomp/guest_shader_pack.h.

#include "guest_shaders.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

extern "C" {
extern const uint8_t kGuestShaderPackData[];
extern const uint8_t kGuestShaderPackDataEnd[];
}

namespace nocturne {
namespace {

// This title's pack: magic 'NRGS', no renderer-owned extras, a hash section to
// resolve shader objects by, and right aligned literal blocks of 8 float4s
// (FrameworkPS carries eight).
krypton::PackTraits MakeTraits() {
  krypton::PackTraits traits{};
  traits.magic = 0x5347524E;  // 'NRGS', little endian
  traits.version = 1;
  traits.flag_point_size = 1 << 0;
  traits.flag_has_cube = 1 << 1;
  traits.flag_param_gen = 0;
  traits.flag_footprint = 0;
  traits.literal_block_bytes = 128;
  traits.literals_tail_aligned = true;
  traits.has_hash_section = true;
  traits.extra_entries = 0;
  return traits;
}

krypton::GuestShaderPack g_pack;

// Where each shader object keeps its copy of the container header, and the
// pointer to the microcode the guest actually runs (a physical address).
constexpr uint32_t kVertexHeader = 872;
constexpr uint32_t kPixelHeader = 40;
constexpr uint32_t kVertexMicrocode = 0x20;
constexpr uint32_t kPixelMicrocode = 0x18;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint32_t Virtual(uint32_t physical) {
  return physical >= 0x80000000u ? physical : 0xC0000000u | (physical & 0x1FFFFFFFu);
}

// The key gen-guest-shaders.py stores for a shader object: FNV-1a 64 over its
// microcode, with every vfetch instruction zeroed for a vertex shader, since
// the guest rewrites those in place from the bound declaration. The fetch table
// is read from the header exactly as vertex_fetch_dwords does: S at header
// dword 6, the literal prefix size in S[0], S[7] entries at S[9 + S[6]], each
// an instruction index after the prefix in its low 12 bits.
uint64_t ShaderKey(const uint8_t* base, uint32_t object, bool pixel) {
  const uint32_t header = object + (pixel ? kPixelHeader : kVertexHeader);
  const uint32_t header_size = Load32(base, header + 4);
  const uint32_t size = Load32(base, header + 8);
  const uint32_t ucode = Virtual(Load32(base, object + (pixel ? kPixelMicrocode : kVertexMicrocode)));
  if (size == 0 || size > 0x10000 || (size & 3))
    return 0;
  const uint8_t* code = rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), ucode);
  std::vector<uint8_t> words(code, code + size);
  if (!pixel) {
    const uint32_t sub = header + Load32(base, header + 24);
    if (Load32(base, header + 24) + 40 > header_size)
      return 0;
    const uint32_t prefix = Load32(base, sub) / 4;
    const uint32_t table = sub + 4 * (9 + Load32(base, sub + 24));
    const uint32_t count = Load32(base, sub + 28);
    for (uint32_t i = 0; i < count; ++i) {
      const size_t dword = prefix + 3 * (Load32(base, table + 4 * i) & 0xFFF);
      if ((dword + 3) * 4 > words.size())
        return 0;
      std::fill_n(words.begin() + dword * 4, 12, uint8_t(0));
    }
  }
  return krypton::HashGuestMicrocode(words.data(), words.size());
}

// Keyed by the object and its microcode pointer, so a freed object whose
// address is reused for a different shader is looked up again.
std::mutex g_cache_mutex;
std::unordered_map<uint64_t, int> g_cache;

int Find(const uint8_t* base, uint32_t object, bool pixel) {
  if (!object)
    return -1;
  const uint32_t ucode = Load32(base, object + (pixel ? kPixelMicrocode : kVertexMicrocode));
  const uint64_t cache_key = uint64_t(object) << 32 | ucode;
  std::lock_guard lock(g_cache_mutex);
  auto& slots = g_cache;
  if (const auto it = slots.find(cache_key); it != slots.end())
    return it->second;
  const uint64_t key = ShaderKey(base, object, pixel);
  const int slot = g_pack.FindSlot(pixel, key);
  if (slot < 0)
    REXLOG_WARN("guest_shaders: {} shader {:08X} (key {:016X}, magic {:08X}, {} byte microcode) "
                "is not in the pack",
                pixel ? "pixel" : "vertex", object, key,
                Load32(base, object + (pixel ? kPixelHeader : kVertexHeader)),
                Load32(base, object + (pixel ? kPixelHeader : kVertexHeader) + 8));
  slots.emplace(cache_key, slot);
  return slot;
}

}  // namespace

bool LoadGuestShaders() {
  const size_t size = size_t(kGuestShaderPackDataEnd - kGuestShaderPackData);
  return g_pack.Load(kGuestShaderPackData, size, MakeTraits());
}

int FindGuestVertexShader(const uint8_t* base, uint32_t object) { return Find(base, object, false); }
int FindGuestPixelShader(const uint8_t* base, uint32_t object) { return Find(base, object, true); }

int FindGuestVertexShaderByKey(uint64_t key) { return g_pack.FindSlot(false, key); }

const GuestShader& GuestVertexShader(uint32_t slot) { return g_pack.Vertex(slot); }
const GuestShader& GuestPixelShader(uint32_t slot) { return g_pack.Pixel(slot); }

}  // namespace nocturne
