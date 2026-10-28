// Host side of the vendored SOTN_XB_RANDO core: the callbacks and globals its
// Win32 GUI (main.cpp upstream) would otherwise provide, the presets table
// that GUI builds with its combo box, and the Generator that sets the core's
// options the way SyncGUI_OptionsToXB does and runs XB_Randomize.
//
// Randomizer by MottZilla: https://github.com/MottZilla/SOTN_XB_RANDO

#include "rando.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <thread>


#include "rando_core.h"
#include "xb_rando.h"

extern int XB_GeneratedSeedComplexity;  // defined in xb_rando.cpp, not in its header
extern char XB_LocationSet[64];         // likewise

// What xb_rando.cpp expects main.cpp to define.
char sSeedName[256] = "";

namespace {
std::mutex g_log_mutex;
std::vector<std::string> g_log;
}  // namespace

void LogMsg(char* sMsg) {
  nocturne::rando::Generator::Get().Append(sMsg);
}

void LogScratch() {
  nocturne::rando::Generator::Get().Scratch();
}

// The core's relic placement runs on its own thread while XB_Randomize polls
// XB_ThreadStatus here; upstream pumps Win32 messages in this slot.
void DoMessages() {
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void KickProgress(int) {}

namespace nocturne::rando {

namespace {

constexpr const char* kNormal1 = "Basic Randomizer.";

uint32_t ReadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace

const std::vector<Preset>& Presets() {
  // Mirrors Show_Preset_Description in upstream main.cpp: the order, which
  // controls each preset fixes, and the text it shows.
  static const std::vector<Preset> presets = {
      {"Standard", false, true, 6, false,
       {"Minimum Complexity = User Choice", kNormal1,
        "Note: Complexity is not difficulty. It's progression chain length."}},
      {"O.G.", true, false, 0, true,
       {"Minimum Complexity = 1",
        "Basic Randomizer. Gold & Silver Rings, Holy Glasses, and Spikebreaker are in original "
        "locations. Relic locations only.",
        "Starting Gear: Random."}},
      {"Guarded O.G.", true, false, 0, true,
       {"Minimum Complexity = 1",
        "Basic Randomizer. Gold & Silver Rings, Holy Glasses, and Spikebreaker are in original "
        "locations.",
        "Starting Gear: Random."}},
      {"Lycanthrope", true, false, 0, true,
       {"Minimum Complexity = 8", "Start with all Wolf Relics. Reduced MP cost for Wolf.",
        "Starting Gear: Shield Rod, Shaman Shield, Silver Crown, Brilliant Mail, Blood Cloak, and "
        "Mystic Pendant."}},
      {"Nimble", true, false, 0, true,
       {"Minimum Complexity = 6", "Start with Soul of Bat, Gravity Boots, and Leap Stone.",
        "Starting Gear: Mana Prism, Buffalo Star, Dragon Helm, Alucard Mail, Twilight Cloak, and "
        "Duplicator."}},
      {"Warlock", true, false, 0, true,
       {"Minimum Complexity = 6", "Start with Form of Mist and 99 INT. Spell MP cost greatly reduced.",
        "Starting Gear: Wizard Hat, the rest random."}},
      {"Bat Master", true, false, 0, true,
       {"Minimum Complexity = 1",
        "All Bat Abilities upgraded. Bat form can open doors. Pressing Y Cancels Wing Smash. "
        "Holding Y moves bat at normal speed.",
        "Starting Gear: Random.", "Designed by: MT_Fun & MottZilla"}},
      {"Adventure", true, true, 8, false,
       {"Minimum Complexity = User Choice. 8 Default.",
        "Normal Randomizer with Equipment Location checks. Takes longer to complete.",
        "Starting Gear: Random."}},
      {"Expedition", true, false, 0, true,
       {"Minimum Complexity = 7", "Start with Soul of Bat, Gravity Boots, and Leap Stone.",
        "Starting Gear: Mana Prism, Buffalo Star, Dragon Helm, Alucard Mail, Twilight Cloak, and "
        "Duplicator.",
        "Equipment Location checks!"}},
      {"Bounty Hunter", true, false, 0, true,
       {"Minimum Complexity = 5",
        "The 5 Vlad relics are dropped from 5 randomly picked enemies. You must find the 5 Familiar "
        "Cards to find out which 5 enemies you need to defeat to get each Vlad relic.",
        "Starting Gear: Alucart Sword, Shield, Mail, Dragon Helm, Twilight Cloak, Ring of "
        "Arcana"}},
      {"Hitman", true, false, 0, true,
       {"Minimum Complexity = 1",
        "This is mostly the same as Bounty Hunter except you start with all 5 cards and the drop "
        "rate on the Vlad relics is increased. When generating these seeds you will see the names "
        "of the 5 targets as well.",
        "Starting Gear: Alucart Sword, Shield, Mail, Dragon Helm, Twilight Cloak, Ring of "
        "Arcana"}},
      {"Richter Rando", true, true, 6, false,
       {"Minimum Complexity = User Choice. 6 Default & Max.",
        "Play randomizer as Richter. You must find Holy Glasses and all 5 Vlads to reach the end. "
        "Richter can make use of certain relics to progress or power up. He can also use Food, "
        "Library Cards, and Life Apples.",
        "Quick Tips:", "Library Card Teleport with Faerie Scroll or Library Card.",
        "Hold LB+RB and Press B.", "Use Food by holding Up and Pressing RT.",
        "See READTHIS.TXT in SOTN_XB_RANDO for more info."}},
      {"Glitch", true, false, 0, true,
       {"Minimum Complexity = 5",
        "This preset requires the use of different Glitches to complete. This includes Shift "
        "Lines, Reverse Shift Lines, Fire Boomerang Jumps, and more. The logic is similar to the "
        "current PSX randomizer's Glitch preset.",
        "Starting Gear: Random + Fire Boomerang & Library Card.", "Equipment Location checks!"}},
      {"NoLogic", true, false, 0, true,
       {"Minimum Complexity = 1",
        "Another glitch preset. In this preset you start with Soul of Wolf, Leap Stone, Fire "
        "Boomerangs, Axe Lord Armor, Duplicator, Manna Prism, Shield Rod, and Shaman Shield. "
        "Relics can be anywhere as there is no location logic.",
        "Guarded Location checks!"}},
      {"NoLogicEq", true, false, 0, true,
       {"Minimum Complexity = 1",
        "Another glitch preset. In this preset you start with Soul of Wolf, Leap Stone, Fire "
        "Boomerangs, Axe Lord Armor, Duplicator, Manna Prism, Shield Rod, and Shaman Shield. "
        "Relics can be anywhere as there is no location logic.",
        "Equipment Location checks!"}},
      {"Bingo", false, false, 0, false,
       {"Vanilla Relics, start with Holy Glasses, no random Library Card or Duplicator drops from "
        "enemies. Be aware all check box options other than Randomize Relics still work for "
        "Bingo.",
        "For Bingo Board Play Only."}},
      {"LegDay", true, false, 0, true,
       {"Minimum Complexity = 7", "Infinite Leap Stone jumping. Super Backdash.",
        "BETA VERSION! Door behavior incomplete!", "Starting Gear: Random.",
        "Designed by: MT_Fun & MottZilla"}},
  };
  return presets;
}

namespace {

using KeyValues = std::map<std::string, std::string>;

bool ReadKeyValues(const std::filesystem::path& path, KeyValues& kv) {
  std::ifstream f(path);
  if (!f) {
    return false;
  }
  for (std::string line; std::getline(f, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (auto eq = line.find('='); eq != std::string::npos) {
      kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
  }
  return true;
}

// Every boolean option and its key, in file order.
template <typename O, typename F>
void ForEachFlag(O& o, F&& f) {
  f("relics", o.relics);
  f("starting_gear", o.starting_gear);
  f("equipment", o.equipment);
  f("drops", o.drops);
  f("shop", o.shop);
  f("candles", o.candles);
  f("force_no_complexity", o.force_no_complexity);
}

}  // namespace

bool LoadOptionsFile(const std::filesystem::path& path, Options* o) {
  KeyValues kv;
  if (!ReadKeyValues(path, kv)) {
    return false;
  }
  if (kv.count("preset")) {
    // By name, which is what the core goes by; an index from older files.
    const auto& presets = Presets();
    const std::string& value = kv["preset"];
    const auto it = std::find_if(presets.begin(), presets.end(),
                                 [&](const Preset& p) { return value == p.name; });
    const int p = it != presets.end() ? static_cast<int>(it - presets.begin())
                                      : std::atoi(value.c_str());
    if (p >= 0 && p < static_cast<int>(presets.size())) {
      o->preset = p;
    }
  }
  if (kv.count("complexity")) {
    o->complexity = std::clamp(std::atoi(kv["complexity"].c_str()), 1, 8);
  }
  if (kv.count("seed")) {
    o->seed = kv["seed"];
  }
  ForEachFlag(*o, [&](const char* key, bool& v) {
    if (kv.count(key)) {
      v = kv[key] == "1";
    }
  });
  return true;
}

bool SaveOptionsFile(const std::filesystem::path& path, const Options& o,
                     const std::string& extra_lines) {
  std::ofstream f(path, std::ios::trunc);
  const auto& presets = Presets();
  f << "preset=" << presets[std::min<size_t>(o.preset, presets.size() - 1)].name
    << "\ncomplexity=" << o.complexity << "\nseed=" << o.seed << "\n";
  ForEachFlag(o, [&](const char* key, bool v) { f << key << '=' << (v ? 1 : 0) << '\n'; });
  f << extra_lines;
  return static_cast<bool>(f);
}

std::string RandomSeedName() {
  std::mt19937 rng{std::random_device{}()};
  auto word = [&] { return std::string(XB_ItemNames[rng() % 256]); };
  return word() + " " + word() + std::to_string(rng() % 1000);
}

namespace {

// Upstream's buffer is the decrypted retail default.xex: its 0x4000-byte
// header, then the image. Its offsets map onto the image up to the end of the
// retail file's first compressed data block, as kMappedSize says.
constexpr uint32_t kHeaderSize = 0x4000;
constexpr uint32_t kMappedSize = 15007744;

std::shared_ptr<const BaseXex> BuildBaseXex(const uint8_t* image, size_t size,
                                            uint32_t image_base, std::string* error) {
  auto fail = [&](const char* msg) -> std::shared_ptr<const BaseXex> {
    if (error) {
      *error = msg;
    }
    return nullptr;
  };

  auto base = std::make_shared<BaseXex>();
  base->header_size = kHeaderSize;
  base->image_base = image_base;
  base->mapped_end = kHeaderSize + std::min<uint32_t>(kMappedSize, static_cast<uint32_t>(size));
  base->file.reserve(kHeaderSize + size);
  base->file.resize(kHeaderSize);
  base->file.insert(base->file.end(), image, image + size);

  // Upstream's VerifyXEX, minus the word at 0x112: that is a security-header
  // field xextool rewrites when it decrypts. The PE timestamp identifies the
  // build on its own.
  const auto& f = base->file;
  if (f.size() < 0x4114 || ReadBE32(f.data() + 0x4000) != 0x4D5A9000) {
    return fail("The game image isn't a PE image.");
  }
  if (ReadBE32(f.data() + 0x4110) != 0x5DAEED45) {
    return fail("This isn't the USA XBLA game the randomizer supports.");
  }
  return base;
}

// `writes`: the file offsets the core wrote, from SetMemoryXex.
PatchSet DiffBase(const BaseXex& base, const std::vector<uint8_t>& patched,
                  const std::vector<size_t>& writes) {
  // Changed bytes closer than this are kept in one run; the unchanged bytes
  // between them are written back as they were.
  constexpr size_t kMergeGap = 8;
  PatchSet set;
  for (size_t off : writes) {
    if (off >= base.header_size) {
      const uint32_t guest = base.GuestOf(off);
      set.writes[guest & ~3u] |= 0xFF000000u >> ((guest & 3) * 8);
    }
  }
  const size_t end = std::min(base.file.size(), patched.size());
  for (size_t off = base.header_size; off < end;) {
    if (base.file[off] == patched[off]) {
      ++off;
      continue;
    }
    size_t run_end = off + 1;
    for (size_t gap = 0; run_end + gap < end && gap <= kMergeGap;) {
      if (base.file[run_end + gap] != patched[run_end + gap]) {
        run_end += gap + 1;
        gap = 0;
      } else {
        ++gap;
      }
    }
    PatchRun run;
    run.guest = base.GuestOf(off);
    run.base.assign(base.file.begin() + off, base.file.begin() + run_end);
    run.bytes.assign(patched.begin() + off, patched.begin() + run_end);
    set.runs.push_back(std::move(run));
    off = run_end;
  }
  return set;
}

// The core keeps its state in globals; one run at a time.
std::mutex g_core_mutex;

}  // namespace

std::shared_ptr<const BaseXex> MakeBaseXex(const uint8_t* image, size_t size, uint32_t image_base,
                                           std::string* error) {
  return BuildBaseXex(image, size, image_base, error);
}

size_t PatchSet::ByteCount() const {
  size_t n = 0;
  for (const auto& run : runs) {
    n += run.bytes.size();
  }
  return n;
}

uint64_t PatchSet::Hash() const {
  // FNV-1a over (guest, length, bytes) of every run, integers big-endian.
  uint64_t h = 0xcbf29ce484222325ull;
  auto byte = [&](uint8_t b) {
    h ^= b;
    h *= 0x100000001b3ull;
  };
  auto u32 = [&](uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) {
      byte(static_cast<uint8_t>(v >> s));
    }
  };
  for (const auto& run : runs) {
    u32(run.guest);
    u32(static_cast<uint32_t>(run.bytes.size()));
    for (uint8_t b : run.bytes) {
      byte(b);
    }
  }
  return h;
}

std::shared_ptr<const Result> Generate(const BaseXex& base, const Options& o,
                                       std::string* error) {
  auto fail = [&](const char* msg) -> std::shared_ptr<const Result> {
    if (error) {
      *error = msg;
    }
    return nullptr;
  };

  std::lock_guard core_lock(g_core_mutex);
  auto& gen = Generator::Get();
  gen.Append("Game image loaded.");

  // What SyncGUI_OptionsToXB does: every XB_ option the core reads.
  const auto& presets = Presets();
  const Preset& preset = presets[std::min<size_t>(o.preset, presets.size() - 1)];
  XB_RandomizeRelicLocations = o.relics;
  XB_UseRandomStartingGear = o.starting_gear;
  XB_RandomizeEquipment = o.equipment;
  XB_RandomizeDrops = o.drops;
  XB_RandomizeShop = o.shop;
  XB_CandleRando = o.candles;
  XB_ForceNoComplexity = o.force_no_complexity;
  // Enhancements, written over the world by ComposeWorld instead.
  XB_BoostDropRates = XB_ClockStatue = XB_FastWarps = XB_AntiFreeze = XB_EnableMapTracker = 0;
  XB_ChosenComplexity = std::clamp(o.complexity, 1, 8);
  std::strncpy(XB_PresetName, preset.name, sizeof(XB_PresetName) - 1);
  XB_PresetName[sizeof(XB_PresetName) - 1] = 0;

  // SeedStringToNumber.
  std::strncpy(sSeedName, o.seed.c_str(), sizeof(sSeedName) - 1);
  sSeedName[sizeof(sSeedName) - 1] = 0;
  uint32_t seed = 0;
  for (size_t i = 0; sSeedName[i]; ++i) {
    seed += uint32_t(static_cast<int8_t>(sSeedName[i])) << ((i & 3) * 8);
  }

  std::vector<uint8_t> patched = base.file;
  std::vector<size_t> writes;
  detail::SetMemoryXex(&patched, base.mapped_end, &writes);
  detail::SeedGeneratorRng(seed);
  XB_AbortSignal = 0;
  XB_GeneratedSeedComplexity = 0;
  const bool ok = XB_Randomize(const_cast<char*>(detail::kMemoryXexPath), seed) != 0;
  detail::SetMemoryXex(nullptr, 0);
  std::string spoilers = detail::TakeSpoilers();

  if (!ok) {
    return fail("XB_Randomize() Error!");
  }
  if (XB_GeneratedSeedComplexity > 15) {
    // XB_Randomize reports a failed or aborted relic placement as success
    // with an impossible complexity.
    return fail(XB_AbortSignal ? "Operation aborted." : "Seed generation error!");
  }
  if (detail::MemoryXexOutOfRange()) {
    // A write the guest-address mapping doesn't cover; applying the rest
    // would leave a half-patched seed.
    return fail("The randomizer wrote outside the mapped part of the game image.");
  }

  auto result = std::make_shared<Result>();
  result->options = o;
  result->patches = DiffBase(base, patched, writes);
  result->spoilers = std::move(spoilers);
  result->complexity = XB_GeneratedSeedComplexity;
  result->location_set = XB_LocationSet;
  return result;
}

std::shared_ptr<const PatchSet> ComposeWorld(const BaseXex& base, const PatchSet* seed,
                                             const std::string& location_set,
                                             const Enhancements& enhancements,
                                             std::string* error) {
  std::vector<uint8_t> patched = base.file;
  std::vector<size_t> writes;
  if (seed) {
    for (const auto& run : seed->runs) {
      std::copy(run.bytes.begin(), run.bytes.end(),
                patched.begin() + (run.guest - base.image_base + base.header_size));
    }
  }
  if (!enhancements.Empty()) {
    std::lock_guard core_lock(g_core_mutex);
    detail::SetMemoryXex(&patched, base.mapped_end, &writes);
    detail::WriteEnhancements(enhancements, location_set);
    const bool out_of_range = detail::MemoryXexOutOfRange();
    detail::SetMemoryXex(nullptr, 0);
    if (out_of_range) {
      if (error) {
        *error = "An enhancement wrote outside the mapped part of the game image.";
      }
      return nullptr;
    }
  }
  auto world = std::make_shared<PatchSet>(DiffBase(base, patched, writes));
  if (seed) {
    for (const auto& [addr, mask] : seed->writes) {
      world->writes[addr] |= mask;
    }
  }
  return world;
}

Generator& Generator::Get() {
  static Generator instance;
  return instance;
}

void Generator::Append(const char* line) {
  std::lock_guard lock(g_log_mutex);
  g_log.emplace_back(line);
}

void Generator::Scratch() {
  std::lock_guard lock(g_log_mutex);
  if (!g_log.empty()) {
    g_log.pop_back();
  }
}

std::vector<std::string> Generator::Log() const {
  std::lock_guard lock(g_log_mutex);
  return g_log;
}

void Generator::ClearLog() {
  std::lock_guard lock(g_log_mutex);
  g_log.clear();
}

void Generator::Abort() {
  XB_AbortSignal = 1;
}

void Generator::Fail(Status status, const char* message) {
  Append(message);
  status_ = status;
}

std::shared_ptr<const Result> Generator::result() const {
  std::lock_guard lock(result_mutex_);
  return result_;
}

bool Generator::Start(std::shared_ptr<const BaseXex> base, const std::filesystem::path& spoilers,
                      const Options& options) {
  if (!base) {
    return false;
  }
  Status expected = status_;
  if (expected == Status::kRunning) {
    return false;
  }
  status_ = Status::kRunning;
  XB_AbortSignal = 0;
  ClearLog();
  std::thread([this, base = std::move(base), spoilers, options] {
    Run(base, spoilers, options);
  }).detach();
  return true;
}

void Generator::Run(std::shared_ptr<const BaseXex> base, std::filesystem::path spoilers, Options o) {
  std::string err;
  auto result = Generate(*base, o, &err);
  if (!result) {
    Fail(XB_AbortSignal ? Status::kAborted : Status::kFailed, err.c_str());
    return;
  }
  if (!spoilers.empty()) {
    std::ofstream f(spoilers, std::ios::binary | std::ios::trunc);
    f << result->spoilers;
  }
  char line[128];
  std::snprintf(line, sizeof(line), "Seed generated: %zu runs, %zu bytes, hash %016llx.",
                result->patches.runs.size(), result->patches.ByteCount(),
                static_cast<unsigned long long>(result->patches.Hash()));
  Append(line);
  {
    std::lock_guard lock(result_mutex_);
    result_ = std::move(result);
  }
  status_ = Status::kDone;
}

}  // namespace nocturne::rando
