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
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/system/xex_module.h>

#include "generated/nocturnerecomp_init.h"

#include "ppc_interpreter.h"

REXCVAR_DEFINE_STRING(rando_xex_name, "", "Game",
                      "Boot this randomizer-patched xex (a same-layout in-place "
                      "variant of the base xex, e.g. SOTN_XB_RANDO's Rando.xex, "
                      "placed in the game data root) instead of the base image. ")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

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

// Randomizer "code caves": regions the patch writes into previously-zero
// space outside the recompiled code range (new functions the base image
// never had, reached via patched branches/pointers). No recompiled function
// exists for them, so calls land in the dispatcher's fallback resolver
// (RandoCaveResolver below), which lazily binds an interpreter thunk per
// entry point.
struct CaveRegion {
  uint32_t start;
  uint32_t end;
};
std::vector<CaveRegion> g_cave_regions;         // sorted by start
std::mutex g_cave_mutex;                        // guards the two members below
std::unordered_map<uint32_t, PPCFunc*> g_cave_thunks;  // entry point -> bound thunk
size_t g_next_slot = 0;  // next free g_thunk_bounds slot (boot-time install + caves)
uint8_t* g_membase = nullptr;

template <size_t I>
void InterpThunk(PPCContext& ctx, uint8_t* base) {
  PpcInterpret(ctx, base, g_thunk_bounds[I].start, g_thunk_bounds[I].end);
}

template <size_t... Is>
constexpr std::array<PPCFunc*, sizeof...(Is)> MakeThunks(std::index_sequence<Is...>) {
  return {&InterpThunk<Is>...};
}
constexpr auto g_thunks = MakeThunks(std::make_index_sequence<kMaxInterpThunks>{});

inline uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return f.good() || f.eof();
}

PPCFunc* RandoCaveResolver(uint32_t target) {
  // Containing cave region, if any (regions are sorted and disjoint).
  auto it = std::upper_bound(
      g_cave_regions.begin(), g_cave_regions.end(), target,
      [](uint32_t addr, const CaveRegion& r) { return addr < r.start; });
  if (it == g_cave_regions.begin()) {
    return nullptr;
  }
  const CaveRegion& region = *(it - 1);
  if (target >= region.end || (target & 3)) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(g_cave_mutex);
  auto cached = g_cave_thunks.find(target);
  if (cached != g_cave_thunks.end()) {
    return cached->second;
  }

  uint32_t bad_pc = 0, bad_insn = 0;
  if (!PpcScanFunction(g_membase, target, region.end, &bad_pc, &bad_insn)) {
    REXLOG_WARN("[rando] cave function {:08X} uses instruction {:08X} at {:08X} the "
                "interpreter doesn't support -- call will trap",
                target, bad_insn, bad_pc);
    return nullptr;
  }
  if (g_next_slot >= kMaxInterpThunks) {
    REXLOG_WARN("[rando] out of interpreter thunk slots for cave function {:08X}", target);
    return nullptr;
  }
  size_t slot = g_next_slot++;
  g_thunk_bounds[slot] = {target, region.end};
  PPCFunc* fn = g_thunks[slot];
  g_cave_thunks.emplace(target, fn);
  REXLOG_INFO("[rando] interpreting cave function {:08X} (cave {:08X}-{:08X})", target,
              region.start, region.end);
  return fn;
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
  g_membase = memory->virtual_membase();

  std::vector<uint8_t> base_raw, patched_raw;
  if (!ReadFileBytes(base_xex, base_raw) || !ReadFileBytes(patched_xex, patched_raw)) {
    REXLOG_WARN("[rando] failed to read {} / {} for the .text diff -- randomizer code "
                "changes are inactive",
                base_xex.string(), patched_xex.string());
    return;
  }

  // Diff the *flat basefile images* (decrypted + decompressed, laid out as
  // mapped at image_base), never the raw files: the base xex is typically
  // AES-encrypted while randomizer output is plaintext, so a raw byte diff
  // "differs" everywhere (23k+ false patched functions), and with basic
  // compression file offsets don't even line up with guest addresses.
  std::vector<uint8_t> base_img, patched_img;
  if (!rex::runtime::XexModule::ExtractBaseImage(base_raw.data(), base_raw.size(), base_img) ||
      !rex::runtime::XexModule::ExtractBaseImage(patched_raw.data(), patched_raw.size(),
                                                 patched_img)) {
    REXLOG_WARN("[rando] failed to extract the basefile image from {} or {} (unsupported "
                "compression/encryption?) -- randomizer code changes are inactive",
                base_xex.string(), patched_xex.string());
    return;
  }
  if (base_img.size() != patched_img.size()) {
    REXLOG_WARN("[rando] extracted images differ in size ({} vs {} bytes) -- not a "
                "same-layout variant; randomizer code changes are inactive",
                base_img.size(), patched_img.size());
    return;
  }

  // The flat image maps at image_base: guest = image_base + image_offset.
  uint64_t code_file_start = REX_CODE_BASE - REX_IMAGE_BASE;
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
    uint32_t guest = REX_IMAGE_BASE + static_cast<uint32_t>(off);
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

  // Find code caves: contiguous patched dwords outside the code range whose
  // base-image bytes are all zero (new code written into padding/zero space
  // -- nonzero base means randomized *data*, which the booted image already
  // covers). Runs separated by short zero gaps (alignment padding between
  // cave functions) merge into one region.
  {
    constexpr uint64_t kCaveMergeGap = 32;
    for (uint64_t off = 0; off + 4 <= base_img.size(); off += 4) {
      if (off >= code_file_start && off < code_file_end) {
        continue;
      }
      if (std::memcmp(base_img.data() + off, patched_img.data() + off, 4) == 0 ||
          ReadBE32(base_img.data() + off) != 0) {
        continue;
      }
      uint32_t guest = REX_IMAGE_BASE + static_cast<uint32_t>(off);
      if (!g_cave_regions.empty() && guest - g_cave_regions.back().end <= kCaveMergeGap) {
        g_cave_regions.back().end = guest + 4;
      } else {
        g_cave_regions.push_back({guest, guest + 4});
      }
    }
    // Drop single-dword "regions": those are pointers patched into zero
    // data, not code.
    g_cave_regions.erase(std::remove_if(g_cave_regions.begin(), g_cave_regions.end(),
                                        [](const CaveRegion& r) { return r.end - r.start <= 4; }),
                         g_cave_regions.end());
    for (const CaveRegion& r : g_cave_regions) {
      REXLOG_INFO("[rando] code cave {:08X}-{:08X} ({} bytes) -- entry points bind to the "
                  "interpreter on first call",
                  r.start, r.end, r.end - r.start);
    }
    if (!g_cave_regions.empty()) {
      // Let both the scan (below) and interpretation flow through caves as
      // gotos (hook trampolines branch out and back mid-function), and catch
      // direct calls to cave entry points via the dispatcher fallback.
      std::vector<std::pair<uint32_t, uint32_t>> ranges;
      for (const CaveRegion& r : g_cave_regions) {
        ranges.emplace_back(r.start, r.end);
      }
      PpcSetInterpretableRanges(std::move(ranges));
      dispatcher->SetFallbackResolver(&RandoCaveResolver);
    }
  }

  if (changed.empty()) {
    REXLOG_INFO("[rando] no .text differences between {} and {} -- data-only patch, "
                "no function overrides needed",
                patched_xex.filename().string(), base_xex.filename().string());
    return;
  }

  uint8_t* membase = memory->virtual_membase();
  uint32_t code_end = REX_CODE_BASE + REX_CODE_SIZE;
  size_t installed = 0, skipped = 0;
  size_t& slot = g_next_slot;  // shared with RandoCaveResolver's lazy bindings
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
