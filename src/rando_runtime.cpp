// See src/rando_runtime.h.

#include "rando_runtime.h"

#include <cctype>
#include <cinttypes>
#include <cstring>
#include <fstream>
#include <map>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_registry.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>

#include "generated/nocturnerecomp_init.h"

#include "enhancements.h"
#include "game_state.h"
#include "rando_hooks.h"

namespace nocturne {
namespace {

uint8_t* g_membase = nullptr;

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

void RandoRuntime::Bind(rex::Runtime* runtime, const std::filesystem::path& user_data_root) {
  runtime_ = runtime;
  active_path_ = user_data_root / "rando_active.txt";
  slots_dir_ = user_data_root / "rando_slots";
  g_membase = runtime->memory()->virtual_membase();
  rando_hooks::Init(g_membase);

  // The randomizer works on the image as it was loaded, wherever it came
  // from. Nothing has run yet to change it, so this is the base every world
  // is diffed against.
  std::string base_error = "The game module isn't loaded.";
  const auto module = REX_KERNEL_STATE()->GetExecutableModule();
  const auto* xex = module ? module->xex_module() : nullptr;
  if (xex) {
    base_ = rando::MakeBaseXex(runtime->memory()->TranslateVirtual<const uint8_t*>(
                                   xex->base_address()),
                               xex->image_size(), xex->base_address(), &base_error);
  }
  {
    std::lock_guard lock(mutex_);
    state_.supported = base_ != nullptr;
    if (!base_) {
      state_.unsupported_reason = base_error;
    }
  }

  std::error_code ec;
  runtime->mod_registry()->RegisterTick([this] { Tick(); });

  // The New Game seed from the last run: also the world a save made before
  // per-slot binding expects. Regenerate it (well under a second) and apply
  // it before any guest code runs. The enhancements go in with it.
  const bool has_seed = std::filesystem::exists(active_path_, ec);
  if (!state().supported) {
    if (has_seed) {
      const std::string message = "Couldn't restore the active seed: " +
                                  state().unsupported_reason;
      REXLOG_WARN("[rando] {}", message);
      std::lock_guard lock(mutex_);
      state_.message = message;
    }
    return;
  }
  std::lock_guard apply_lock(apply_mutex_);
  auto fail = [&](const std::string& message) {
    REXLOG_WARN("[rando] {} -- booting the vanilla game", message);
    ApplyVanilla();
    std::lock_guard lock(mutex_);
    state_.message = message + " Booted the vanilla game.";
  };
  std::string error;
  if (!has_seed) {
    if (!Apply(nullptr, &error)) {
      REXLOG_WARN("[rando] couldn't apply the enhancements: {}", error);
      failed_enhancements_ = CurrentEnhancements();
      std::lock_guard lock(mutex_);
      state_.message = "Couldn't apply the enhancements: " + error;
    }
    return;
  }
  auto seed = LoadSeedFile(active_path_, &error);
  if (!seed) {
    fail(error);
    return;
  }
  if (!Apply(seed, &error)) {
    fail("Couldn't apply the active seed: " + error);
    return;
  }
  {
    std::lock_guard lock(mutex_);
    state_.new_game = seed;
  }
  REXLOG_INFO("[rando] restored active seed \"{}\" ({})", seed->options.seed,
              HashText(seed->patches.Hash()));
}

std::shared_ptr<const rando::Result> RandoRuntime::LoadSeedFile(const std::filesystem::path& path,
                                                                std::string* error) {
  rando::Options options;
  std::string saved_hash;
  rando::LoadOptionsFile(path, &options);
  {
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
      if (line.rfind("hash=", 0) == 0) {
        saved_hash = line.substr(5);
      }
    }
  }
  for (const auto& cached : seed_cache_) {
    if (cached->options == options && HashText(cached->patches.Hash()) == saved_hash) {
      return cached;
    }
  }
  std::string gen_error;
  auto seed = rando::Generate(*base_, options, &gen_error);
  if (!seed) {
    *error = "Couldn't regenerate seed \"" + options.seed + "\": " + gen_error;
    return nullptr;
  }
  const std::string hash = HashText(seed->patches.Hash());
  if (hash != saved_hash) {
    // The same options no longer give the same seed (the randomizer core
    // changed). A different world would break the save; better vanilla.
    *error = "Seed \"" + options.seed + "\" regenerated differently (hash " + hash + ", saved " +
             saved_hash + ").";
    return nullptr;
  }
  seed_cache_.push_back(seed);
  return seed;
}

void RandoRuntime::WriteSeedFile(const std::filesystem::path& path,
                                 const std::shared_ptr<const rando::Result>& seed) const {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (seed) {
    rando::SaveOptionsFile(path, seed->options, "hash=" + HashText(seed->patches.Hash()) + "\n");
  } else {
    std::ofstream(path) << "vanilla=1\n";
  }
}

bool RandoRuntime::IsVanillaFile(const std::filesystem::path& path) {
  std::ifstream f(path);
  for (std::string line; std::getline(f, line);) {
    if (line == "vanilla=1") {
      return true;
    }
  }
  return false;
}

std::filesystem::path RandoRuntime::SlotPath(const std::string& slot) const {
  std::string safe;
  for (char c : slot) {
    safe += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
  }
  return slots_dir_ / (safe + ".txt");
}

bool RandoRuntime::ApplyWorld(std::shared_ptr<const rando::Result> seed, std::string* error) {
  const State current = state();
  if (seed == current.active && CurrentEnhancements() == current.enhancements) {
    return true;
  }
  return Apply(std::move(seed), error);
}

void RandoRuntime::ApplyVanilla() {
  std::string error;
  if (!Apply(nullptr, &error)) {
    REXLOG_WARN("[rando] couldn't apply the enhancements: {}", error);
    failed_enhancements_ = CurrentEnhancements();
    Revert();
  }
}

void RandoRuntime::OnSaveRead(const std::string& slot) { last_read_slot_ = slot; }

void RandoRuntime::OnSaveLoading() {
  // File select reads every save to list it, then reads the chosen one once
  // more right before unpacking it, so the last read is the save being loaded.
  const std::string slot = last_read_slot_;
  const auto path = SlotPath(slot);
  std::shared_ptr<const rando::Result> seed;
  std::string message;
  std::error_code ec;
  std::lock_guard apply_lock(apply_mutex_);
  if (slot.empty() || !std::filesystem::exists(path, ec)) {
    message = "This save has no seed recorded (it predates per-save seeds); playing the vanilla "
              "game.";
  } else if (!IsVanillaFile(path)) {
    std::string error;
    seed = LoadSeedFile(path, &error);
    if (!seed) {
      message = error + " Playing the vanilla game.";
    }
  }
  std::string error;
  if (!ApplyWorld(seed, &error)) {
    message = "Couldn't apply this save's seed: " + error + " Playing the vanilla game.";
    ApplyVanilla();
  }
  REXLOG_INFO("[rando] loading save {}: {}", slot,
              state().active ? "seed \"" + state().active->options.seed + "\"" : "vanilla");
  std::lock_guard lock(mutex_);
  state_.message = std::move(message);
}

void RandoRuntime::OnSaveWritten(const std::string& slot) {
  if (slot.empty() || slots_dir_.empty()) {
    return;
  }
  const auto seed = state().active;
  WriteSeedFile(SlotPath(slot), seed);
  REXLOG_INFO("[rando] save {} bound to {}", slot,
              seed ? "seed \"" + seed->options.seed + "\"" : "the vanilla game");
}

void RandoRuntime::OnSaveDeleted(const std::string& slot) {
  std::error_code ec;
  if (!slot.empty() && !slots_dir_.empty() && std::filesystem::remove(SlotPath(slot), ec)) {
    REXLOG_INFO("[rando] save {} deleted; dropped its seed", slot);
  }
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

bool RandoRuntime::InFrontEnd() const {
  return g_membase && nocturne::InFrontEnd(g_membase);
}

void RandoRuntime::Tick() {
  // Back at the title: a loaded save may have brought its own seed in; New
  // Game gets the chosen one again.
  const bool front_end = InFrontEnd();
  if (front_end && !was_front_end_) {
    const auto new_game = state().new_game;
    std::string error;
    std::lock_guard apply_lock(apply_mutex_);
    if (!ApplyWorld(new_game, &error)) {
      REXLOG_WARN("[rando] couldn't restore the New Game seed: {}", error);
      ApplyVanilla();
      std::lock_guard lock(mutex_);
      state_.new_game.reset();
      state_.message = "Couldn't restore the New Game seed: " + error;
    }
  }
  if (!front_end && was_front_end_) {
    // Menu messages ("start a new game") are stale once a game starts; a
    // save load sets its own afterwards.
    std::lock_guard lock(mutex_);
    state_.message.clear();
  }
  was_front_end_ = front_end;

  // An enhancement was switched in the settings: rewrite the world in memory,
  // wherever the game is. Unlike a seed, enhancements are not part of what a
  // save was written in, so nothing breaks if a game is running; this is the
  // guest thread between frames, so no guest code sees a half-written world.
  // Data the game already copied out of the image (a stage's layout, say)
  // keeps its old bytes until the game loads it again.
  const State current = state();
  const rando::Enhancements wanted = CurrentEnhancements();
  if (current.supported && !current.request_pending && wanted != current.enhancements) {
    std::string error;
    std::lock_guard apply_lock(apply_mutex_);
    // A set that failed was reported once; wait for another change.
    if (failed_enhancements_ != wanted && !Apply(current.active, &error)) {
      REXLOG_WARN("[rando] couldn't apply the enhancements: {}", error);
      failed_enhancements_ = wanted;
      std::lock_guard lock(mutex_);
      state_.message = "Couldn't apply the enhancements: " + error;
    }
  }

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
  if (!InFrontEnd()) {
    // Also covers file select: the game may already be reading stage data.
    message = "A game is running. Quit to the title screen to change the seed.";
  } else if (request == Request::kApply) {
    std::string error;
    std::lock_guard apply_lock(apply_mutex_);
    if (Apply(seed, &error)) {
      WriteSeedFile(active_path_, seed);
      std::lock_guard lock(mutex_);
      state_.new_game = seed;
      message = "Seed \"" + seed->options.seed + "\" is active. Start a new game.";
    } else {
      message = "Couldn't apply the seed: " + error;
    }
  } else {
    {
      std::lock_guard apply_lock(apply_mutex_);
      ApplyVanilla();
    }
    std::error_code ec;
    std::filesystem::remove(active_path_, ec);
    std::lock_guard lock(mutex_);
    state_.new_game.reset();
    message = "Back to the vanilla game.";
  }
  std::lock_guard lock(mutex_);
  state_.request_pending = false;
  state_.message = std::move(message);
}

bool RandoRuntime::Apply(std::shared_ptr<const rando::Result> seed, std::string* error) {
  // The settings' enhancements, recorded as applied; the ones written are
  // those the seed doesn't already have or clash with.
  const rando::Enhancements enhancements = CurrentEnhancements();
  const rando::Enhancements written =
      EffectiveEnhancements(enhancements, seed ? &seed->options : nullptr);
  if (!seed && (written.Empty() || !base_)) {
    // Enhancements need the image the randomizer's offsets are for.
    Revert();
    return true;
  }
  if (!base_) {
    *error = state().unsupported_reason;
    return false;
  }
  std::shared_ptr<const rando::PatchSet> patches;
  if (written.Empty()) {
    patches = std::shared_ptr<const rando::PatchSet>(seed, &seed->patches);
  } else {
    patches = rando::ComposeWorld(*base_, seed ? &seed->patches : nullptr, written, error);
    if (!patches) {
      return false;
    }
  }
  auto* memory = runtime_->memory();
  const uint32_t image_end = base_->image_base +
                             static_cast<uint32_t>(base_->file.size() - base_->header_size);
  for (const auto& run : patches->runs) {
    if (run.guest < base_->image_base || run.guest + run.bytes.size() > image_end) {
      *error = "the patches reach outside the game image";
      return false;
    }
  }

  Revert();

  // Data: write every run. Live memory should still hold the base image;
  // anything else means the game changed it, and is worth knowing about.
  size_t live_mismatches = 0;
  for (const auto& run : patches->runs) {
    const uint8_t* host = memory->TranslateVirtual<const uint8_t*>(run.guest);
    if (std::memcmp(host, run.base.data(), run.base.size()) != 0 && live_mismatches++ < 8) {
      REXLOG_WARN("[rando] live memory at {:08X} ({} bytes) isn't the base image", run.guest,
                  run.base.size());
    }
    WriteGuest(memory, run.guest, run.bytes.data(), run.bytes.size());
  }

  // Code: point the hooks at the patches each site now holds. Patched
  // instructions no hook covers stay inert.
  const uint32_t code_start = REX_CODE_BASE;
  const uint32_t code_end = REX_CODE_BASE + REX_CODE_SIZE;
  std::map<uint32_t, uint32_t> code_writes;
  for (const auto& [addr, mask] : patches->writes) {
    if (addr >= code_start && addr < code_end) {
      code_writes.emplace(addr, mask);
    }
  }
  rando_hooks::Report report;
  rando_hooks::Activate(
      g_membase,
      [&](uint32_t addr) {
        const auto it = code_writes.find(addr);
        return it == code_writes.end() ? 0u : it->second;
      },
      &report);
  size_t inactive = report.unknown.size();
  for (uint32_t addr : report.unknown) {
    REXLOG_WARN("[rando] the instruction at {:08X} holds {:08X}, a patch its hook doesn't know "
                "-- this change stays inactive",
                addr, ReadBE32(memory->TranslateVirtual<const uint8_t*>(addr)));
  }
  for (const auto& [addr, mask] : code_writes) {
    if (!rando_hooks::IsSite(addr)) {
      REXLOG_WARN("[rando] the instruction at {:08X} changed to {:08X}, but no hook covers it "
                  "-- this change stays inactive",
                  addr, ReadBE32(memory->TranslateVirtual<const uint8_t*>(addr)));
      ++inactive;
    }
  }
  if (!report.active_sites.empty()) {
    std::string sites;
    for (const auto& [addr, state] : report.active_sites) {
      sites += fmt::format(" {:08X}:{}", addr, state >> 16);
      if (state & 0xFFFF) {
        sites += fmt::format("/{:X}", state & 0xFFFF);
      }
    }
    REXLOG_INFO("[rando] patched instructions (site:variant/immediate):{}", sites);
  }

  REXLOG_INFO("[rando] applied {} with enhancements {:05X}/{}/{:08X}/{}: {} runs, {} bytes, {} "
              "patched instruction(s) active, {} inactive, {} run(s) where live memory wasn't "
              "the base image",
              seed ? "seed \"" + seed->options.seed + "\"" : "the vanilla game",
              written.flags, written.wing_smash_color, written.alchemy_vats,
              static_cast<int>(written.game_mode), patches->runs.size(),
              patches->ByteCount(), report.active_sites.size(), inactive, live_mismatches);
  applied_ = std::move(patches);
  std::lock_guard lock(mutex_);
  state_.active = std::move(seed);
  state_.enhancements = enhancements;
  state_.inactive_code_patches = inactive;
  return true;
}

void RandoRuntime::Revert() {
  rando_hooks::Deactivate();

  {
    std::lock_guard lock(mutex_);
    state_.active.reset();
    state_.enhancements = {};
    state_.inactive_code_patches = 0;
  }
  const auto applied = std::exchange(applied_, nullptr);
  if (!applied) {
    return;
  }
  auto* memory = runtime_->memory();
  size_t live_mismatches = 0;
  for (const auto& run : applied->runs) {
    const uint8_t* host = memory->TranslateVirtual<const uint8_t*>(run.guest);
    if (std::memcmp(host, run.bytes.data(), run.bytes.size()) != 0 && live_mismatches++ < 8) {
      REXLOG_WARN("[rando] live memory at {:08X} ({} bytes) changed since it was patched",
                  run.guest, run.bytes.size());
    }
    WriteGuest(memory, run.guest, run.base.data(), run.base.size());
  }
  REXLOG_INFO("[rando] back to the base image ({} run(s) had changed in play)",
              live_mismatches);
}

RandoRuntime& GetRandoRuntime() {
  static RandoRuntime instance;
  return instance;
}

}  // namespace nocturne


// The memory card object's save methods (vtable 0x82216678; r4 = the save's
// content name, e.g. "BASLUS-00067DRAX0", one package per slot) and the
// game's LoadSaveData, which unpacks the save just read into g_Status, the
// castle flags and the room to start in. Its only caller is file select's
// "load game" step, which starts the game right after, so the seed applied
// here is in place before any of the save's world loads. Vanilla image only,
// like the randomizer itself.
#include <rex/hook.h>

extern "C" REX_FUNC(__imp__sub_824F8190);  // create + write a save
extern "C" REX_FUNC(__imp__sub_824F84D8);  // overwrite a save
extern "C" REX_FUNC(__imp__sub_824F86C8);  // read a save
extern "C" REX_FUNC(__imp__sub_822542F8);  // LoadSaveData
extern "C" REX_FUNC(__imp__sub_825D8218);  // -> XamContentDelete(user, XCONTENT_DATA*, ...)

namespace {

constexpr uint32_t kContentDataFileNameOffset = 0x108;  // XCONTENT_DATA::file_name_raw
constexpr size_t kContentNameMax = 42;

std::string GuestName(const uint8_t* base, uint32_t addr) {
  if (!addr) {
    return {};
  }
  const char* p = reinterpret_cast<const char*>(base + addr);
  return std::string(p, strnlen(p, kContentNameMax));
}

}  // namespace

REX_HOOK_RAW(sub_824F8190) {
  const std::string slot = GuestName(base, ctx.r4.u32);
  __imp__sub_824F8190(ctx, base);
  nocturne::GetRandoRuntime().OnSaveWritten(slot);
}

REX_HOOK_RAW(sub_824F84D8) {
  const std::string slot = GuestName(base, ctx.r4.u32);
  __imp__sub_824F84D8(ctx, base);
  nocturne::GetRandoRuntime().OnSaveWritten(slot);
}

REX_HOOK_RAW(sub_824F86C8) {
  nocturne::GetRandoRuntime().OnSaveRead(GuestName(base, ctx.r4.u32));
  __imp__sub_824F86C8(ctx, base);
}

REX_HOOK_RAW(sub_822542F8) {
  nocturne::GetRandoRuntime().OnSaveLoading();
  __imp__sub_822542F8(ctx, base);
}

REX_HOOK_RAW(sub_825D8218) {
  const uint32_t data = ctx.r4.u32;
  const std::string slot = data ? GuestName(base, data + kContentDataFileNameOffset) : "";
  __imp__sub_825D8218(ctx, base);
  nocturne::GetRandoRuntime().OnSaveDeleted(slot);
}
