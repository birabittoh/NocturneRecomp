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
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <thread>

#include <rex/system/xex_module.h>

#include "rando_core.h"
#include "xb_rando.h"

extern int XB_GeneratedSeedComplexity;  // defined in xb_rando.cpp, not in its header

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

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
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

std::string RandomSeedName() {
  std::mt19937 rng{std::random_device{}()};
  auto word = [&] { return std::string(XB_ItemNames[rng() % 256]); };
  return word() + " " + word() + std::to_string(rng() % 1000);
}

bool WriteDecryptedXex(const std::filesystem::path& src, const std::filesystem::path& dst,
                       std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) {
      *error = msg;
    }
    return false;
  };

  std::vector<uint8_t> xex;
  if (!ReadFile(src, xex)) {
    return fail("Couldn't read the game's default.xex.");
  }
  std::vector<uint8_t> image;
  if (!rex::runtime::XexModule::ExtractBaseImage(xex.data(), xex.size(), image)) {
    return fail("Couldn't decrypt default.xex (unsupported encryption or compression).");
  }

  const uint32_t header_size = ReadBE32(xex.data() + 8);
  const uint32_t header_count = ReadBE32(xex.data() + 20);
  constexpr uint32_t kFileFormatInfoKey = 0x3FF;
  uint32_t info = 0;
  for (uint32_t i = 0; i < header_count && 24 + 8 * i + 8 <= header_size; ++i) {
    if (ReadBE32(xex.data() + 24 + 8 * i) == kFileFormatInfoKey) {
      info = ReadBE32(xex.data() + 24 + 8 * i + 4);
    }
  }
  if (!info || info + 8 > header_size) {
    return fail("default.xex has no file format info header.");
  }

  // info: u32 size, u16 encryption (0 none, 1 normal), u16 compression
  // (0 none, 1 basic); basic is followed by (data_size, zero_size) blocks.
  const uint32_t info_size = ReadBE32(xex.data() + info);
  const uint32_t compression = (uint32_t(xex[info + 6]) << 8) | xex[info + 7];

  std::vector<uint8_t> out(xex.begin(), xex.begin() + header_size);
  out[info + 4] = out[info + 5] = 0;  // no encryption; compression is left as it was

  if (compression == 0) {
    out.insert(out.end(), image.begin(), image.end());
  } else if (compression == 1) {
    // Basic compression drops the zero runs between data blocks; the stored
    // form is just the data parts back to back.
    size_t pos = 0;
    for (uint32_t b = 8; b + 8 <= info_size; b += 8) {
      const uint32_t data = ReadBE32(xex.data() + info + b);
      const uint32_t zeros = ReadBE32(xex.data() + info + b + 4);
      if (pos + data > image.size()) {
        return fail("default.xex has an inconsistent compression table.");
      }
      out.insert(out.end(), image.begin() + pos, image.begin() + pos + data);
      pos += size_t(data) + zeros;
    }
  } else {
    return fail("default.xex uses an unsupported compression type.");
  }

  if (out.size() != xex.size()) {
    return fail("The decrypted xex isn't the same size as default.xex.");
  }

  std::ofstream f(dst, std::ios::binary | std::ios::trunc);
  if (!f) {
    return fail("Couldn't open Rando.xex for writing.");
  }
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  f.close();
  if (!f) {
    return fail("Couldn't write Rando.xex.");
  }
  return true;
}

bool VerifyDecryptedXex(const std::filesystem::path& path, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) {
      *error = msg;
    }
    return false;
  };
  std::vector<uint8_t> xex;
  if (!ReadFile(path, xex) || xex.size() < 0x4114) {
    return fail("Couldn't read the xex.");
  }
  if (ReadBE32(xex.data()) != 0x58455832) {
    return fail("Invalid XEX file.");
  }
  if (ReadBE32(xex.data() + 0x4000) != 0x4D5A9000) {
    return fail("XEX file still needs to be decrypted.");
  }
  // Upstream also compares the word at 0x112, but that is a security-header
  // field xextool rewrites when it decrypts; ours leaves it as shipped. The PE
  // timestamp identifies the build on its own.
  if (ReadBE32(xex.data() + 0x4110) != 0x5DAEED45) {
    return fail("This isn't the unpatched USA XBLA default.xex the randomizer supports.");
  }
  return true;
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

bool Generator::Start(const std::filesystem::path& base_xex, const std::filesystem::path& output,
                      const std::filesystem::path& spoilers, const Options& options) {
  Status expected = status_;
  if (expected == Status::kRunning) {
    return false;
  }
  status_ = Status::kRunning;
  ClearLog();
  detail::SetSpoilersPath(spoilers.string());
  std::thread([this, base_xex, output, options] { Run(base_xex, output, options); }).detach();
  return true;
}

void Generator::Run(std::filesystem::path base_xex, std::filesystem::path output, Options o) {
  std::string err;
  if (!WriteDecryptedXex(base_xex, output, &err)) {
    Fail(Status::kFailed, err.c_str());
    return;
  }
  if (!VerifyDecryptedXex(output, &err)) {
    Fail(Status::kFailed, err.c_str());
    return;
  }
  Append("Vanilla XEX copied to Rando.XEX successfully.");

  // What SyncGUI_OptionsToXB does: every XB_ option the core reads.
  const auto& presets = Presets();
  const Preset& preset = presets[std::min<size_t>(o.preset, presets.size() - 1)];
  XB_RandomizeRelicLocations = o.relics;
  XB_UseRandomStartingGear = o.starting_gear;
  XB_RandomizeEquipment = o.equipment;
  XB_RandomizeDrops = o.drops;
  XB_BoostDropRates = o.drop_rates;
  XB_RandomizeShop = o.shop;
  XB_ClockStatue = o.clock_statue;
  XB_FastWarps = o.fast_warps;
  XB_AntiFreeze = o.anti_freeze;
  XB_CandleRando = o.candles;
  XB_EnableMapTracker = o.map_tracker;
  XB_ForceNoComplexity = o.force_no_complexity;
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

  XB_AbortSignal = 0;
  XB_GeneratedSeedComplexity = 0;
  const bool ok = XB_Randomize(const_cast<char*>(output.string().c_str()), seed) != 0;
  if (!ok) {
    Fail(Status::kFailed, "XB_Randomize() Error!");
  } else if (XB_GeneratedSeedComplexity > 15) {
    // XB_Randomize reports a failed or aborted relic placement as success
    // with an impossible complexity.
    Fail(XB_AbortSignal ? Status::kAborted : Status::kFailed,
         "Seed generation error or operation aborted!");
  } else {
    Append("XB_Randomize() completed successfully.");
    status_ = Status::kDone;
    return;
  }
  std::error_code ec;
  std::filesystem::remove(output, ec);
}

}  // namespace nocturne::rando
