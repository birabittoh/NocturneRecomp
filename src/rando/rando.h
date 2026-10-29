// Randomizer front end: generates a seed of MottZilla's SOTN_XB_RANDO core
// (ext/SOTN_XB_RANDO, https://github.com/MottZilla/SOTN_XB_RANDO) in memory.
//
// The upstream tool patches a decrypted copy of default.xex on disk. Here the
// image the game was loaded from is copied into a host buffer laid out like
// that file (MakeBaseXex), the core's XB_Randomize patches that buffer (its
// stdio is redirected, see rando_core.cpp), and the result is the list of
// guest byte runs it changed (PatchSet) plus the spoiler log. Applying a
// PatchSet to the running game is src/rando_runtime.h's job.
//
// The core is a pile of globals (one run at a time, not re-entrant), so
// Generator is a singleton driving it from one worker thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
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
  bool shop = true;
  bool candles = true;
  bool force_no_complexity = false;
  // The upstream options that are also enhancements (fast warps, anti-freeze,
  // clock statue, boosted drop rates, map tracker) are not here: they apply
  // over any world from the enhancements, and can change while a game runs.
  // Upstream's generation never draws from rand() for them, so leaving them
  // out doesn't change the world a seed generates.

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
  // Can't be finished without glitches (shift lines, boomerang jumps...).
  bool requires_glitches = false;
};

const std::vector<Preset>& Presets();

// Options as "key=value" lines, the format of randomizer.txt and of a save's
// seed record (src/rando_runtime.h): everything Generate reads, so the same
// lines generate the same run. The preset is saved by name. Loading leaves
// options missing from the file as they were.
bool LoadOptionsFile(const std::filesystem::path& path, Options* options);
bool SaveOptionsFile(const std::filesystem::path& path, const Options& options,
                     const std::string& extra_lines = {});

// A random two-item-name seed, as the upstream GUI generates.
std::string RandomSeedName();

// The game image prepared for the randomizer: a header region followed by the
// flat (decrypted, decompressed) image. That is the layout of upstream's
// decrypted xex wherever the core reads or writes, so its hard-coded file
// offsets address this buffer directly. The core never reads the header
// region, so it is left zeroed.
struct BaseXex {
  std::vector<uint8_t> file;
  uint32_t header_size = 0;  // file offset of the image
  uint32_t image_base = 0;   // guest address of the image
  // File offsets map to guest addresses (image_base + offset - header_size)
  // only up to here: past the first compressed data block of the retail
  // file, the decrypted file and the flat image part ways, and upstream
  // never writes there.
  uint32_t mapped_end = 0;

  uint32_t GuestOf(size_t file_offset) const {
    return image_base + static_cast<uint32_t>(file_offset - header_size);
  }
};

// Wraps the flat image mapped at `image_base` (the game module as loaded, before
// any guest code runs) and runs upstream's VerifyXEX checks on it.
std::shared_ptr<const BaseXex> MakeBaseXex(const uint8_t* image, size_t size, uint32_t image_base,
                                           std::string* error);

// One run of guest bytes a seed changes, with the base image's bytes kept for
// reverting.
struct PatchRun {
  uint32_t guest = 0;
  std::vector<uint8_t> base;
  std::vector<uint8_t> bytes;
};

struct PatchSet {
  std::vector<PatchRun> runs;  // sorted by guest address, disjoint
  // Every word the core wrote, by guest address, with a mask of the bytes it
  // wrote (0xFF000000: the first). A write can leave a byte as it was (with
  // the code blanked out of the image, every patched immediate of 0 does), so
  // the runs alone don't say which instructions a world patches.
  std::map<uint32_t, uint32_t> writes;

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
  // Where the preset can put items (the core's XB_LocationSet: "Guarded",
  // "Spread" or "Equipment"), which is what the map tracker marks.
  std::string location_set;
};

// Generates a seed on the calling thread, blocking while another run holds
// the core. Progress goes to Generator's log.
std::shared_ptr<const Result> Generate(const BaseXex& base, const Options& options,
                                       std::string* error);

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
  kMapTracker,         // XB_ApplyMapTrackerPatch with the "Guarded" location set
  // Patches XB_PresetWrites applies for some presets.
  kClockRoomDoor,      // XB_PATCHCLOCKROOMOPEN (every preset but Bingo)
  kKeyItemsStay,       // entity 0xA doesn't expire (every preset but Bingo)
  kFreeJewelOfOpen,    // the shop relic's price (every preset)
  kCheapDuplicator,    // XB_RandomizeShopItems' "500K slot" price
  kInfiniteItems,      // Glitch / NoLogic "Infinite Usage"
  kSkipMariaSilverRing,// Glitch / NoLogic "Remove Silver Ring Maria Cutscene"
  kDemonCardDoor,      // Bounty Hunter "Open Demon Card Switch Door"
  kCount,
};

// The gameplay patches of a preset (its XB_Apply*Patches), without the
// preset's seed. They share one code cave, so one at a time.
enum class GameMode : int {
  kNormal,
  kBatMaster,  // XB_ApplyBatMasterPatches
  kLegDay,     // XB_ApplyLegDayPatches
  kRichter,    // XB_ApplyRichterPatches
  kCount,
};

struct Enhancements {
  uint32_t flags = 0;        // bit (1 << Enhancement)
  int wing_smash_color = 0;  // 0: the game's own; 1-8: upstream's WS_COLORS
  // XB_RandomizeAlchemyVats with its rand() seeded from this; 0: off.
  uint32_t alchemy_vats = 0;
  GameMode game_mode = GameMode::kNormal;

  bool Has(Enhancement e) const { return flags & (1u << static_cast<int>(e)); }
  void Clear(Enhancement e) { flags &= ~(1u << static_cast<int>(e)); }
  bool Empty() const {
    return !flags && !wing_smash_color && !alchemy_vats && game_mode == GameMode::kNormal;
  }
  bool operator==(const Enhancements&) const = default;
};

inline constexpr int kWingSmashColorCount = 8;

// The world to put in guest memory: `seed` (null: vanilla) with
// `enhancements` written over it, as one PatchSet against the base image.
// `location_set` is the seed's Result::location_set (empty for vanilla), for
// the map tracker. Shares the core with Generate. The caller leaves out
// enhancements the seed already has or that clash with it (see
// EffectiveEnhancements in src/enhancements.h).
std::shared_ptr<const PatchSet> ComposeWorld(const BaseXex& base, const PatchSet* seed,
                                             const std::string& location_set,
                                             const Enhancements& enhancements,
                                             std::string* error);

enum class Status { kIdle, kRunning, kDone, kFailed, kAborted };

class Generator {
 public:
  static Generator& Get();

  // Generates a seed from `base` on a worker thread; false when a run is
  // already going. On success, the spoiler log is also written to `spoilers`.
  bool Start(std::shared_ptr<const BaseXex> base, const std::filesystem::path& spoilers,
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
  void Run(std::shared_ptr<const BaseXex> base, std::filesystem::path spoilers, Options options);
  void Fail(Status status, const char* message);

  std::atomic<Status> status_{Status::kIdle};
  mutable std::mutex result_mutex_;
  std::shared_ptr<const Result> result_;
};

}  // namespace nocturne::rando
