// Randomizer front end: generates a seed of MottZilla's SOTN_XB_RANDO core
// (ext/SOTN_XB_RANDO, https://github.com/MottZilla/SOTN_XB_RANDO) in memory.
//
// The upstream tool patches a decrypted copy of default.xex on disk. Here the
// game's own default.xex is decrypted in-process into a host buffer in the
// same file layout (LoadBaseXex), the core's XB_Randomize patches that
// buffer (its stdio is redirected, see rando_core.cpp), and the result is the
// list of guest byte runs it changed (PatchSet) plus the spoiler log. Applying
// a PatchSet to the running game is src/rando_runtime.h's job.
//
// The core is a pile of globals (one run at a time, not re-entrant), so
// Generator is a singleton driving it from one worker thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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

  bool operator==(const Options&) const = default;
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

// Options as "key=value" lines, the format of randomizer.txt. Loading leaves
// options missing from the file as they were.
bool LoadOptionsFile(const std::filesystem::path& path, Options* options);
bool SaveOptionsFile(const std::filesystem::path& path, const Options& options,
                     const std::string& extra_lines = {});

// A random two-item-name seed, as the upstream GUI generates.
std::string RandomSeedName();

// default.xex prepared for the randomizer: the xex header followed by the
// flat (decrypted, decompressed) image. That is the layout of upstream's
// decrypted xex wherever the core writes, so its hard-coded file offsets
// address this buffer directly.
struct BaseXex {
  std::vector<uint8_t> file;
  uint32_t header_size = 0;  // file offset of the image
  uint32_t image_base = 0;   // guest address of the image
  // File offsets map to guest addresses (image_base + offset - header_size)
  // only up to here: past the first compressed data block, the decrypted
  // file and the flat image part ways.
  uint32_t mapped_end = 0;

  uint32_t GuestOf(size_t file_offset) const {
    return image_base + static_cast<uint32_t>(file_offset - header_size);
  }
};

// Loads `xex` and runs upstream's VerifyXEX checks on the result. Loaded once
// per path and cached.
std::shared_ptr<const BaseXex> LoadBaseXex(const std::filesystem::path& xex, std::string* error);

// One run of guest bytes a seed changes, with the base image's bytes kept for
// reverting.
struct PatchRun {
  uint32_t guest = 0;
  std::vector<uint8_t> base;
  std::vector<uint8_t> bytes;
};

struct PatchSet {
  std::vector<PatchRun> runs;  // sorted by guest address, disjoint

  size_t ByteCount() const;
  // Identifies the seed's output; stable across runs and platforms.
  uint64_t Hash() const;
};

// A generated seed.
struct Result {
  Options options;
  PatchSet patches;
  std::string spoilers;
  int complexity = 0;
};

// Generates a seed on the calling thread, blocking while another run holds
// the core. Progress goes to Generator's log.
std::shared_ptr<const Result> Generate(const std::filesystem::path& base_xex,
                                       const Options& options, std::string* error);

// The core's standalone patches (fixes and quality-of-life changes a seed
// applies on top of its randomization), offered on their own: they apply to
// the vanilla game and on top of any seed. Each is one core function, so the
// bytes are exactly upstream's.
enum class Enhancement : int {
  kFastWarps,          // XB_ApplyFastWarps
  kAntiFreeze,         // XB_ApplyAntiFreeze
  kRemoveClockStatue,  // XB_RemoveClockStatue
  kBoostDropRates,     // the XB_BoostDropRates half of XB_DoDropsRando
  kDeathOnVacation,    // XB_ApplyDeathOnVacation (Bounty Hunter / Hitman)
  kSkipMariaAlchemy,   // XB_RemoveMariaAlchemyMeeting
  kOuterWallElevator,  // XB_FixOuterWallElevator
  kScyllaSoftlock,     // XB_FixScyllaSoftlock
  kBridgeInput,        // XB_PatchBridgeInput
  kCatacombsEscape,    // XB_PatchMines
  kReverseLibraryCard, // XB_ApplyReverseLibraryCardPatch (Expedition)
  kClearFile,          // XB_PatchClearFile
  kCount,
};

struct Enhancements {
  uint32_t flags = 0;        // bit (1 << Enhancement)
  int wing_smash_color = 0;  // 0: the game's own; 1-8: upstream's WS_COLORS

  bool Has(Enhancement e) const { return flags & (1u << static_cast<int>(e)); }
  bool Empty() const { return !flags && !wing_smash_color; }
  bool operator==(const Enhancements&) const = default;
};

inline constexpr int kWingSmashColorCount = 8;

// The world to put in guest memory: `seed` (null: vanilla) with
// `enhancements` written over it, as one PatchSet against the base image.
// Shares the core with Generate.
std::shared_ptr<const PatchSet> ComposeWorld(const BaseXex& base, const PatchSet* seed,
                                             const Enhancements& enhancements,
                                             std::string* error);

enum class Status { kIdle, kRunning, kDone, kFailed, kAborted };

class Generator {
 public:
  static Generator& Get();

  // Generates a seed from `base_xex` on a worker thread; false when a run is
  // already going. On success, the spoiler log is also written to `spoilers`.
  bool Start(const std::filesystem::path& base_xex, const std::filesystem::path& spoilers,
             const Options& options);
  void Abort();

  Status status() const { return status_; }
  // The last successful run's output, or null.
  std::shared_ptr<const Result> result() const;
  std::vector<std::string> Log() const;
  void ClearLog();

  // Called by the core.
  void Append(const char* line);
  void Scratch();

 private:
  void Run(std::filesystem::path base_xex, std::filesystem::path spoilers, Options options);
  void Fail(Status status, const char* message);

  std::atomic<Status> status_{Status::kIdle};
  mutable std::mutex result_mutex_;
  std::shared_ptr<const Result> result_;
};

}  // namespace nocturne::rando
