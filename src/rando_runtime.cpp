// See src/rando_runtime.h.

#include "rando_runtime.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>

#include "generated/nocturnerecomp_init.h"

#include "in_game_latch.h"
#include "ppc_interpreter.h"

namespace nocturne {
namespace {

// Fixed pool of interpreter entry thunks: the dispatcher wants a plain
// PPCFunc* per function, so each interpreted function gets one template
// instantiation bound to a slot in g_thunk_bounds. A slot is keyed by its
// bounds and never freed or rebound, so a guest thread still inside a thunk
// while seeds change never sees its bounds move. A function's bounds don't
// depend on the seed, so slots are reused across seeds; 512 is far more
// than SOTN_XB_RANDO patches in total.
constexpr size_t kMaxInterpThunks = 512;
constexpr size_t kNoSlot = ~size_t(0);

struct FuncBounds {
  uint32_t start;
  uint32_t end;
};
FuncBounds g_thunk_bounds[kMaxInterpThunks];
std::mutex g_slot_mutex;
std::map<std::pair<uint32_t, uint32_t>, size_t> g_slot_of;  // bounds -> slot

template <size_t I>
void InterpThunk(PPCContext& ctx, uint8_t* base) {
  PpcInterpret(ctx, base, g_thunk_bounds[I].start, g_thunk_bounds[I].end);
}

template <size_t... Is>
constexpr std::array<PPCFunc*, sizeof...(Is)> MakeThunks(std::index_sequence<Is...>) {
  return {&InterpThunk<Is>...};
}
constexpr auto g_thunks = MakeThunks(std::make_index_sequence<kMaxInterpThunks>{});

size_t SlotFor(uint32_t start, uint32_t end) {
  std::lock_guard lock(g_slot_mutex);
  auto [it, inserted] = g_slot_of.try_emplace({start, end}, g_slot_of.size());
  if (inserted) {
    if (it->second >= kMaxInterpThunks) {
      g_slot_of.erase(it);
      return kNoSlot;
    }
    g_thunk_bounds[it->second] = {start, end};
  }
  return it->second;
}

// The active seed's code caves: regions it writes into previously-zero
// space outside the recompiled range. Calls into them miss the dispatcher's
// table and reach CaveResolver.
struct CaveRegion {
  uint32_t start;
  uint32_t end;
};
std::mutex g_cave_mutex;
std::vector<CaveRegion> g_cave_regions;  // sorted, disjoint
uint8_t* g_membase = nullptr;

PPCFunc* CaveResolver(uint32_t target) {
  std::lock_guard lock(g_cave_mutex);
  auto it = std::upper_bound(g_cave_regions.begin(), g_cave_regions.end(), target,
                             [](uint32_t addr, const CaveRegion& r) { return addr < r.start; });
  if (it == g_cave_regions.begin()) {
    return nullptr;
  }
  const CaveRegion& region = *(it - 1);
  if (target >= region.end || (target & 3)) {
    return nullptr;
  }
  uint32_t bad_pc = 0, bad_insn = 0;
  if (!PpcScanFunction(g_membase, target, region.end, &bad_pc, &bad_insn)) {
    REXLOG_WARN("[rando] cave function {:08X} uses instruction {:08X} at {:08X} the "
                "interpreter doesn't support -- call will trap",
                target, bad_insn, bad_pc);
    return nullptr;
  }
  const size_t slot = SlotFor(target, region.end);
  if (slot == kNoSlot) {
    REXLOG_WARN("[rando] out of interpreter thunk slots for cave function {:08X}", target);
    return nullptr;
  }
  return g_thunks[slot];
}

// Functions currently overridden, with the pointer RestoreFunction needs.
std::vector<std::pair<uint32_t, PPCFunc*>> g_overrides;

// Recompiled function starts, sorted.
const std::vector<uint32_t>& FunctionStarts() {
  static const std::vector<uint32_t> starts = [] {
    std::vector<uint32_t> v;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m) {
      v.push_back(static_cast<uint32_t>(m->guest));
    }
    std::sort(v.begin(), v.end());
    return v;
  }();
  return starts;
}

uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Writes guest memory, lifting the guest page protection (with real host
// protection behind it) for code and read-only data.
void WriteGuest(rex::memory::Memory* memory, uint32_t guest, const uint8_t* data, size_t len) {
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(guest);
  auto* heap = memory->LookupHeap(guest);
  uint32_t first = 0, last = 0;
  const bool read_only =
      heap && heap->QueryProtect(guest, &first) &&
      heap->QueryProtect(guest + static_cast<uint32_t>(len) - 1, &last) &&
      (!(first & rex::memory::kMemoryProtectWrite) || !(last & rex::memory::kMemoryProtectWrite));
  uint32_t old = 0;
  const bool lifted =
      read_only && heap->Protect(guest, static_cast<uint32_t>(len),
                                 rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                                 &old);
  std::memcpy(host, data, len);
  if (lifted) {
    heap->Protect(guest, static_cast<uint32_t>(len), old);
  }
}

std::string HashText(uint64_t hash) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64, hash);
  return buf;
}

}  // namespace

void RandoRuntime::Bind(rex::Runtime* runtime, const std::filesystem::path& game_data_root,
                        const std::filesystem::path& user_data_root) {
  runtime_ = runtime;
  base_xex_ = game_data_root / "default.xex";
  active_path_ = user_data_root / "rando_active.txt";
  g_membase = runtime->memory()->virtual_membase();

  std::error_code ec;
  {
    std::lock_guard lock(mutex_);
    state_.supported = true;
    if (std::filesystem::exists(game_data_root / "default.xexp", ec)) {
      // UserModule::LoadFromFile applies a staged title update as a delta
      // patch; the randomizer's offsets are for the unpatched image.
      state_.supported = false;
      state_.unsupported_reason =
          "A title update is staged next to default.xex. The randomizer only supports the "
          "unpatched game.";
    } else if (!std::filesystem::exists(base_xex_, ec)) {
      state_.supported = false;
      state_.unsupported_reason = "default.xex wasn't found in the game folder.";
    }
  }

  runtime->function_dispatcher()->SetFallbackResolver(&CaveResolver);
  runtime->mod_registry()->RegisterTick([this] { Tick(); });

  // The seed that was active when the game last ran: the world a randomized
  // save expects. Regenerate it (well under a second) and apply it before
  // any guest code runs.
  if (!std::filesystem::exists(active_path_, ec)) {
    return;
  }
  auto fail = [&](const std::string& message) {
    REXLOG_WARN("[rando] {} -- booting the vanilla game", message);
    std::lock_guard lock(mutex_);
    state_.message = message + " Booted the vanilla game.";
  };
  if (!state().supported) {
    fail("Couldn't restore the active seed: " + state().unsupported_reason);
    return;
  }
  rando::Options options;
  std::string saved_hash;
  rando::LoadOptionsFile(active_path_, &options);
  {
    std::ifstream f(active_path_);
    for (std::string line; std::getline(f, line);) {
      if (line.rfind("hash=", 0) == 0) {
        saved_hash = line.substr(5);
      }
    }
  }
  std::string error;
  auto seed = rando::Generate(base_xex_, options, &error);
  if (!seed) {
    fail("Couldn't regenerate the active seed \"" + options.seed + "\": " + error);
    return;
  }
  const std::string hash = HashText(seed->patches.Hash());
  if (hash != saved_hash) {
    // The same options no longer give the same seed (the randomizer core
    // changed). A different world would break the save; better vanilla.
    fail("The active seed \"" + options.seed + "\" regenerated differently (hash " + hash +
         ", saved " + saved_hash + ")");
    return;
  }
  if (!Apply(seed, &error)) {
    fail("Couldn't apply the active seed: " + error);
    return;
  }
  REXLOG_INFO("[rando] restored active seed \"{}\" ({})", options.seed, hash);
}

void RandoRuntime::RequestApply(std::shared_ptr<const rando::Result> seed) {
  std::lock_guard lock(mutex_);
  request_ = Request::kApply;
  requested_ = std::move(seed);
  state_.request_pending = true;
}

void RandoRuntime::RequestRevert() {
  std::lock_guard lock(mutex_);
  request_ = Request::kRevert;
  requested_.reset();
  state_.request_pending = true;
}

RandoRuntime::State RandoRuntime::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

bool RandoRuntime::SaveLoaded() const {
  return g_membase && nocturne::SaveLoaded(g_membase);
}

void RandoRuntime::Tick() {
  Request request;
  std::shared_ptr<const rando::Result> seed;
  {
    std::lock_guard lock(mutex_);
    request = std::exchange(request_, Request::kNone);
    seed = std::move(requested_);
    requested_.reset();
  }
  if (request == Request::kNone) {
    return;
  }

  std::string message;
  if (SaveLoaded()) {
    message = "A save is loaded. Return to the title screen to change the seed.";
  } else if (request == Request::kApply) {
    std::string error;
    if (Apply(seed, &error)) {
      SaveActive();
      message = "Seed \"" + seed->options.seed + "\" is active. Start a new game.";
    } else {
      message = "Couldn't apply the seed: " + error;
    }
  } else {
    Revert();
    std::error_code ec;
    std::filesystem::remove(active_path_, ec);
    message = "Back to the vanilla game.";
  }
  std::lock_guard lock(mutex_);
  state_.request_pending = false;
  state_.message = std::move(message);
}

bool RandoRuntime::Apply(std::shared_ptr<const rando::Result> seed, std::string* error) {
  if (!base_) {
    base_ = rando::LoadBaseXex(base_xex_, error);
    if (!base_) {
      return false;
    }
  }
  auto* memory = runtime_->memory();
  auto* dispatcher = runtime_->function_dispatcher();
  const uint32_t code_start = REX_CODE_BASE;
  const uint32_t code_end = REX_CODE_BASE + REX_CODE_SIZE;
  const uint32_t image_end = base_->image_base +
                             static_cast<uint32_t>(base_->file.size() - base_->header_size);
  for (const auto& run : seed->patches.runs) {
    if (run.guest < base_->image_base || run.guest + run.bytes.size() > image_end) {
      *error = "the seed patches outside the game image";
      return false;
    }
  }

  Revert();

  // Data: write every run. Live memory should still hold the base image;
  // anything else means the game changed it, and is worth knowing about.
  size_t live_mismatches = 0;
  for (const auto& run : seed->patches.runs) {
    const uint8_t* host = memory->TranslateVirtual<const uint8_t*>(run.guest);
    if (std::memcmp(host, run.base.data(), run.base.size()) != 0 && live_mismatches++ < 8) {
      REXLOG_WARN("[rando] live memory at {:08X} ({} bytes) isn't the base image", run.guest,
                  run.base.size());
    }
    WriteGuest(memory, run.guest, run.bytes.data(), run.bytes.size());
  }

  // Code: the recompiled functions whose instructions changed, and the caves
  // (changed dwords that were zero in the base image, outside the code
  // range). Cave runs separated by short gaps (alignment padding between
  // cave functions) are one region; single dwords are pointers patched into
  // zero data, not code.
  const auto& starts = FunctionStarts();
  std::set<size_t> changed;  // indices into `starts`
  std::vector<CaveRegion> caves;
  constexpr uint32_t kCaveMergeGap = 32;
  for (const auto& run : seed->patches.runs) {
    const uint32_t run_end = run.guest + static_cast<uint32_t>(run.bytes.size());
    for (uint32_t a = run.guest; a < run_end; ++a) {
      if (a < code_start || a >= code_end || run.bytes[a - run.guest] == run.base[a - run.guest]) {
        continue;
      }
      auto it = std::upper_bound(starts.begin(), starts.end(), a);
      if (it != starts.begin()) {
        changed.insert(static_cast<size_t>((it - 1) - starts.begin()));
      }
    }
    for (uint32_t d = run.guest & ~3u; d < run_end; d += 4) {
      if (d >= code_start && d < code_end) {
        continue;
      }
      const uint8_t* base = base_->file.data() + base_->header_size + (d - base_->image_base);
      if (ReadBE32(base) != 0 || ReadBE32(memory->TranslateVirtual<const uint8_t*>(d)) == 0) {
        continue;
      }
      if (!caves.empty() && d >= caves.back().end && d - caves.back().end <= kCaveMergeGap) {
        caves.back().end = d + 4;
      } else if (caves.empty() || d >= caves.back().end) {
        caves.push_back({d, d + 4});
      }
    }
  }
  caves.erase(std::remove_if(caves.begin(), caves.end(),
                             [](const CaveRegion& r) { return r.end - r.start <= 4; }),
              caves.end());
  if (!caves.empty()) {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const CaveRegion& r : caves) {
      REXLOG_INFO("[rando] code cave {:08X}-{:08X} ({} bytes)", r.start, r.end, r.end - r.start);
      ranges.emplace_back(r.start, r.end);
    }
    if (!PpcAddInterpretableRanges(ranges)) {
      REXLOG_WARN("[rando] interpreter range table is full -- some cave code may trap");
    }
  }
  {
    std::lock_guard lock(g_cave_mutex);
    g_cave_regions = caves;
  }

  size_t inactive = 0;
  for (size_t idx : changed) {
    const uint32_t start = starts[idx];
    const uint32_t end = idx + 1 < starts.size() ? starts[idx + 1] : code_end;
    uint32_t bad_pc = 0, bad_insn = 0;
    if (!PpcScanFunction(g_membase, start, end, &bad_pc, &bad_insn)) {
      REXLOG_WARN("[rando] patched function {:08X} uses instruction {:08X} at {:08X} the "
                  "interpreter doesn't support -- this change stays inactive",
                  start, bad_insn, bad_pc);
      ++inactive;
      continue;
    }
    const size_t slot = SlotFor(start, end);
    PPCFunc* original = nullptr;
    if (slot == kNoSlot || !dispatcher->OverrideFunction(start, g_thunks[slot], &original)) {
      REXLOG_WARN("[rando] couldn't override patched function {:08X} -- this change stays "
                  "inactive",
                  start);
      ++inactive;
      continue;
    }
    g_overrides.emplace_back(start, original);
  }

  REXLOG_INFO("[rando] applied seed \"{}\": {} runs, {} bytes, {} function(s) interpreted, {} "
              "inactive, {} cave(s), {} run(s) where live memory wasn't the base image",
              seed->options.seed, seed->patches.runs.size(), seed->patches.ByteCount(),
              g_overrides.size(), inactive, caves.size(), live_mismatches);
  std::lock_guard lock(mutex_);
  state_.active = std::move(seed);
  state_.inactive_code_patches = inactive;
  return true;
}

void RandoRuntime::Revert() {
  auto* dispatcher = runtime_->function_dispatcher();
  for (const auto& [start, original] : g_overrides) {
    dispatcher->RestoreFunction(start, original);
  }
  g_overrides.clear();
  {
    std::lock_guard lock(g_cave_mutex);
    g_cave_regions.clear();
  }

  std::shared_ptr<const rando::Result> active;
  {
    std::lock_guard lock(mutex_);
    active = std::exchange(state_.active, nullptr);
    state_.inactive_code_patches = 0;
  }
  if (!active) {
    return;
  }
  auto* memory = runtime_->memory();
  size_t live_mismatches = 0;
  for (const auto& run : active->patches.runs) {
    const uint8_t* host = memory->TranslateVirtual<const uint8_t*>(run.guest);
    if (std::memcmp(host, run.bytes.data(), run.bytes.size()) != 0 && live_mismatches++ < 8) {
      REXLOG_WARN("[rando] live memory at {:08X} ({} bytes) changed since the seed was applied",
                  run.guest, run.bytes.size());
    }
    WriteGuest(memory, run.guest, run.base.data(), run.base.size());
  }
  REXLOG_INFO("[rando] reverted seed \"{}\" ({} run(s) had changed in play)",
              active->options.seed, live_mismatches);
}

void RandoRuntime::SaveActive() const {
  auto seed = state().active;
  if (seed) {
    rando::SaveOptionsFile(active_path_, seed->options,
                           "hash=" + HashText(seed->patches.Hash()) + "\n");
  }
}

RandoRuntime& GetRandoRuntime() {
  static RandoRuntime instance;
  return instance;
}

}  // namespace nocturne
