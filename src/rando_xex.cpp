// See src/rando_xex.h. Implements the runtime side of randomizer .text
// support: diff the patched xex against the base image, map the differing
// instructions to recompiled functions, and swap each affected function's
// dispatcher entry for a thunk that interprets it from guest memory (which
// holds the patched image). Works for any same-layout patched xex, not one
// specific randomizer seed.

#include "rando_xex.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <utility>
#include <vector>

#include "generated/nocturnerecomp_init.h"

#include "ppc_interpreter.h"

REXCVAR_DEFINE_STRING(rando_xex_path, "", "Game",
                      "Boot this randomizer-patched xex (a same-layout in-place "
                      "variant of the base xex, e.g. SOTN_XB_RANDO's Rando.xex, "
                      "placed in the game data root) instead of the base image. "
                      "Empty = disabled.");

namespace nocturne {
namespace {

// Fixed pool of interpreter entry thunks: the dispatcher wants a plain
// PPCFunc* per function, so each changed function gets one template
// instantiation bound to a slot in g_thunk_bounds. 512 slots is ~30x what
// SOTN_XB_RANDO actually patches.
constexpr size_t kMaxInterpThunks = 512;

struct FuncBounds {
  uint32_t start;
  uint32_t end;
};
FuncBounds g_thunk_bounds[kMaxInterpThunks];

template <size_t I>
void InterpThunk(PPCContext& ctx, uint8_t* base) {
  PpcInterpret(ctx, base, g_thunk_bounds[I].start, g_thunk_bounds[I].end);
}

template <size_t... Is>
constexpr std::array<PPCFunc*, sizeof...(Is)> MakeThunks(std::index_sequence<Is...>) {
  return {&InterpThunk<Is>...};
}
constexpr auto g_thunks = MakeThunks(std::make_index_sequence<kMaxInterpThunks>{});

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return f.good() || f.eof();
}

inline uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

void InstallRandoOverrides(rex::Runtime* runtime, const std::filesystem::path& base_xex,
                           const std::filesystem::path& patched_xex) {
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!dispatcher || !memory) {
    REXLOG_WARN("[rando] runtime not ready -- randomizer code changes are inactive");
    return;
  }

  std::vector<uint8_t> base_img, patched_img;
  if (!ReadFileBytes(base_xex, base_img) || !ReadFileBytes(patched_xex, patched_img)) {
    REXLOG_WARN("[rando] failed to read {} / {} for the .text diff -- randomizer code "
                "changes are inactive",
                base_xex.string(), patched_xex.string());
    return;
  }
  if (base_img.size() != patched_img.size() || base_img.size() < 0x10) {
    REXLOG_WARN("[rando] {} and {} differ in size -- randomizer code changes are inactive",
                base_xex.string(), patched_xex.string());
    return;
  }

  // XEX2 header: u32 exe_offset at 0x8 = file offset of the embedded PE.
  // Both images are decrypted/uncompressed same-layout basefiles, so the PE
  // maps flat at image_base: guest = image_base + (file_offset - pe_offset).
  // (Getting this wrong -- treating raw pointers as file-relative -- is
  // exactly the 0x4000-off-target bug documented in rando_xex.h.)
  uint32_t pe_off = ReadBE32(base_img.data() + 8);
  if (pe_off != ReadBE32(patched_img.data() + 8)) {
    REXLOG_WARN("[rando] PE offsets differ between base and patched xex -- randomizer "
                "code changes are inactive");
    return;
  }
  uint64_t code_file_start = uint64_t(pe_off) + (REX_CODE_BASE - REX_IMAGE_BASE);
  uint64_t code_file_end = code_file_start + REX_CODE_SIZE;
  if (code_file_end > base_img.size()) {
    code_file_end = base_img.size();
  }

  // Sorted recompiled-function starts, for mapping diff addresses to the
  // containing function. The table is emitted sorted, but don't rely on it.
  std::vector<uint32_t> starts;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m) {
    starts.push_back(static_cast<uint32_t>(m->guest));
  }
  std::sort(starts.begin(), starts.end());
  if (starts.empty()) {
    REXLOG_WARN("[rando] empty function table -- randomizer code changes are inactive");
    return;
  }

  // Collect the set of functions whose instructions the patch touches.
  std::set<uint32_t> changed;  // indices into `starts`
  size_t stray_diffs = 0;
  for (uint64_t off = code_file_start & ~3ull; off + 4 <= code_file_end; off += 4) {
    if (std::memcmp(base_img.data() + off, patched_img.data() + off, 4) == 0) {
      continue;
    }
    uint32_t guest = REX_IMAGE_BASE + static_cast<uint32_t>(off - pe_off);
    auto it = std::upper_bound(starts.begin(), starts.end(), guest);
    if (it == starts.begin()) {
      ++stray_diffs;
      continue;
    }
    changed.insert(static_cast<uint32_t>((it - 1) - starts.begin()));
  }
  if (stray_diffs) {
    REXLOG_WARN("[rando] {} patched dword(s) in the code range precede the first known "
                "function -- ignored", stray_diffs);
  }
  if (changed.empty()) {
    REXLOG_INFO("[rando] no .text differences between {} and {} -- data-only patch, "
                "no function overrides needed",
                patched_xex.filename().string(), base_xex.filename().string());
    return;
  }

  uint8_t* membase = memory->virtual_membase();
  uint32_t code_end = REX_CODE_BASE + REX_CODE_SIZE;
  size_t installed = 0, skipped = 0, slot = 0;
  for (uint32_t idx : changed) {
    uint32_t start = starts[idx];
    uint32_t end = idx + 1 < starts.size() ? starts[idx + 1] : code_end;

    uint32_t bad_pc = 0, bad_insn = 0;
    if (!PpcScanFunction(membase, start, end, &bad_pc, &bad_insn)) {
      REXLOG_WARN("[rando] patched function {:08X} uses instruction {:08X} at {:08X} the "
                  "interpreter doesn't support -- leaving vanilla native code (this "
                  "randomizer change is inert)",
                  start, bad_insn, bad_pc);
      ++skipped;
      continue;
    }
    if (slot >= kMaxInterpThunks) {
      REXLOG_WARN("[rando] more than {} patched functions -- {:08X} and the rest keep "
                  "vanilla native code",
                  kMaxInterpThunks, start);
      ++skipped;
      continue;
    }
    g_thunk_bounds[slot] = {start, end};
    if (dispatcher->OverrideFunction(start, g_thunks[slot])) {
      REXLOG_INFO("[rando] interpreting patched function {:08X}-{:08X}", start, end);
      ++installed;
      ++slot;
    } else {
      REXLOG_WARN("[rando] failed to override patched function {:08X}", start);
      ++skipped;
    }
  }
  REXLOG_INFO("[rando] {} patched function(s): {} interpreted from the patched image, "
              "{} left native",
              changed.size(), installed, skipped);
}

}  // namespace nocturne
