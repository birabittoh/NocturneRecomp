// Randomizer front end: generates a randomizer-patched xex ("Rando.xex") from
// the game's own default.xex, using MottZilla's SOTN_XB_RANDO core
// (ext/SOTN_XB_RANDO, https://github.com/MottZilla/SOTN_XB_RANDO).
//
// The upstream tool needs the user to decrypt default.xex with xextool first;
// here that step is done in-process (WriteDecryptedXex), then the core's
// XB_Randomize patches the copy in place. Booting the result is handled by
// src/rando_xex.h (the rando_xex_name cvar); this file only produces it.
//
// The core is a pile of globals (one run at a time, not re-entrant), so
// Generator is a singleton driving it from one worker thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nocturne::rando {

// What the options screen edits; mirrors the checkboxes of the upstream GUI
// (see SyncGUI_OptionsToXB in its main.cpp).
struct Options {
  int preset = 0;      // index into Presets()
  int complexity = 6;  // minimum complexity, 1-8; only used by some presets
  std::string seed;

  bool relics = true;
  bool starting_gear = true;
  bool equipment = true;
  bool drops = true;
  bool drop_rates = false;
  bool shop = true;
  bool clock_statue = false;
  bool fast_warps = false;
  bool anti_freeze = false;
  bool candles = true;
  bool map_tracker = false;
  bool force_no_complexity = false;
};

struct Preset {
  const char* name;
  // The six "what to randomize" checkboxes are fixed by the preset rather
  // than chosen by the player.
  bool fixes_randomization;
  // The player picks the minimum complexity (default_complexity); otherwise
  // the preset sets it.
  bool picks_complexity;
  int default_complexity;
  // Switch the six checkboxes on when the preset is selected.
  bool sets_normal_options;
  std::vector<const char*> description;
};

const std::vector<Preset>& Presets();

// A random two-item-name seed, as the upstream GUI generates.
std::string RandomSeedName();

// Writes `dst` as a decrypted copy of the retail `src` default.xex: same
// size and layout, the encryption flag cleared, the way xextool -e u leaves
// it -- which is the form the randomizer's hard-coded file offsets address.
bool WriteDecryptedXex(const std::filesystem::path& src, const std::filesystem::path& dst,
                       std::string* error);

// The sanity checks upstream's VerifyXEX runs on its input.
bool VerifyDecryptedXex(const std::filesystem::path& path, std::string* error);

enum class Status { kIdle, kRunning, kDone, kFailed, kAborted };

class Generator {
 public:
  static Generator& Get();

  // Generates `output` from `base_xex`; false when a run is already going.
  // `spoilers` is where the spoiler log is written.
  bool Start(const std::filesystem::path& base_xex, const std::filesystem::path& output,
             const std::filesystem::path& spoilers, const Options& options);
  void Abort();

  Status status() const { return status_; }
  std::vector<std::string> Log() const;
  void ClearLog();

  // Called by the core.
  void Append(const char* line);
  void Scratch();

 private:
  void Run(std::filesystem::path base_xex, std::filesystem::path output, Options options);
  void Fail(Status status, const char* message);

  std::atomic<Status> status_{Status::kIdle};
};

}  // namespace nocturne::rando
