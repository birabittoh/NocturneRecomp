// See enhancements.h.

#include "enhancements.h"

#include <cstring>
#include <iterator>

#include <rex/cvar.h>

#include "ui_text.h"

REXCVAR_DEFINE_BOOL(enh_fast_warps, false, "Enhancements", "Faster warp room teleports");
REXCVAR_DEFINE_BOOL(enh_anti_freeze, false, "Enhancements",
                    "No screen freeze on level-ups and acquisitions");
REXCVAR_DEFINE_BOOL(enh_remove_clock_statue, false, "Enhancements",
                    "Remove the Marble Gallery clock room statue");
REXCVAR_DEFINE_BOOL(enh_boost_drop_rates, false, "Enhancements", "Raise every enemy's drop rates");
REXCVAR_DEFINE_BOOL(enh_death_on_vacation, false, "Enhancements",
                    "Death doesn't take Alucard's equipment");
REXCVAR_DEFINE_BOOL(enh_skip_maria_alchemy, false, "Enhancements",
                    "Skip meeting Maria in the Alchemy Laboratory");
REXCVAR_DEFINE_BOOL(enh_outer_wall_elevator, false, "Enhancements",
                    "Randomizer fix for the Outer Wall elevator");
REXCVAR_DEFINE_BOOL(enh_scylla_softlock, false, "Enhancements", "Fix the Scylla fight softlock");
REXCVAR_DEFINE_BOOL(enh_bridge_input, false, "Enhancements", "Collapse the bridge with Down + Y");
REXCVAR_DEFINE_BOOL(enh_catacombs_escape, false, "Enhancements",
                    "Blocks to leave the Catacombs without movement relics");
REXCVAR_DEFINE_BOOL(enh_reverse_library_card, false, "Enhancements",
                    "Library Card goes to the Reverse Library in the inverted castle");
REXCVAR_DEFINE_BOOL(enh_clear_file, false, "Enhancements", "Treat every file as a cleared game");
REXCVAR_DEFINE_BOOL(enh_map_tracker, false, "Enhancements",
                    "Mark relic locations on the castle map");
REXCVAR_DEFINE_BOOL(enh_clock_room_door, false, "Enhancements",
                    "Clock Room door opens with both rings in the inventory");
REXCVAR_DEFINE_BOOL(enh_key_items_stay, false, "Enhancements", "Dropped key items don't despawn");
REXCVAR_DEFINE_BOOL(enh_free_jewel_of_open, false, "Enhancements",
                    "The Librarian sells the Jewel of Open for free");
REXCVAR_DEFINE_BOOL(enh_cheap_duplicator, false, "Enhancements",
                    "The Duplicator costs 15,000 gold");
REXCVAR_DEFINE_BOOL(enh_infinite_items, false, "Enhancements",
                    "Library Card, Heart Refresh, Antivenom, Fire Boomerang and Toadstool are "
                    "never used up");
REXCVAR_DEFINE_BOOL(enh_skip_maria_silver_ring, false, "Enhancements",
                    "Skip Maria's cutscene at the Silver Ring");
REXCVAR_DEFINE_BOOL(enh_demon_card_door, false, "Enhancements",
                    "Open the Demon card switch door");
REXCVAR_DEFINE_INT32(enh_wing_smash_color, 0, "Enhancements",
                     "Wing Smash trail color (0: the game's own, 1-8: randomizer colors)");
REXCVAR_DEFINE_UINT32(enh_alchemy_vats, 0, "Enhancements",
                      "Alchemy Laboratory vat shuffle (0: off, else the shuffle's seed)");
REXCVAR_DEFINE_INT32(enh_game_mode, 0, "Enhancements",
                     "Game mode (0: normal, 1: Bat Master, 2: Leg Day, 3: Richter)");

namespace nocturne {

namespace {

using rando::Enhancement;
using rando::GameMode;
using rando::Options;

bool PresetIs(const Options& o, const char* name) {
  const auto& presets = rando::Presets();
  return o.preset >= 0 && static_cast<size_t>(o.preset) < presets.size() &&
         std::strcmp(presets[o.preset].name, name) == 0;
}

SeedUse IncludedIf(bool included) { return included ? SeedUse::kIncluded : SeedUse::kNone; }

bool IsBountyHunter(const Options& o) {
  return PresetIs(o, "Bounty Hunter") || PresetIs(o, "Hitman");
}

bool IsGlitchOrNoLogic(const Options& o) {
  return PresetIs(o, "Glitch") || PresetIs(o, "NoLogic") || PresetIs(o, "NoLogicEq");
}

// XB_Randomize applies these whatever the options.
SeedUse Always(const Options&) { return SeedUse::kIncluded; }

// Every preset but Bingo (XB_PresetWrites).
SeedUse UnlessBingo(const Options& o) { return IncludedIf(!PresetIs(o, "Bingo")); }

// The preset each game mode takes its patches from.
constexpr const char* kGameModePresets[] = {nullptr, "Bat Master", "LegDay", "Richter Rando"};
static_assert(std::size(kGameModePresets) == static_cast<size_t>(GameMode::kCount));

}  // namespace

const std::vector<EnhancementInfo>& EnhancementList() {
  // Label and description are res/lang's [enhancements] <cvar> and <cvar>_desc.
  static const std::vector<EnhancementInfo> list = {
      {Enhancement::kFastWarps, "enh_fast_warps",
       [](const Options& o) { return IncludedIf(o.fast_warps); }, "section_quality_of_life"},
      {Enhancement::kAntiFreeze, "enh_anti_freeze",
       [](const Options& o) { return IncludedIf(o.anti_freeze); }},
      {Enhancement::kRemoveClockStatue, "enh_remove_clock_statue",
       [](const Options& o) { return IncludedIf(o.clock_statue); }},
      {Enhancement::kBoostDropRates, "enh_boost_drop_rates",
       [](const Options& o) { return IncludedIf(o.drops && o.drop_rates); }},
      {Enhancement::kDeathOnVacation, "enh_death_on_vacation",
       [](const Options& o) { return IncludedIf(IsBountyHunter(o)); }},
      {Enhancement::kSkipMariaAlchemy, "enh_skip_maria_alchemy", &Always},
      {Enhancement::kSkipMariaSilverRing, "enh_skip_maria_silver_ring",
       [](const Options& o) { return IncludedIf(IsGlitchOrNoLogic(o)); }},
      {Enhancement::kOuterWallElevator, "enh_outer_wall_elevator", &Always, "section_fixes"},
      {Enhancement::kScyllaSoftlock, "enh_scylla_softlock", &Always},
      {Enhancement::kBridgeInput, "enh_bridge_input", &Always},
      {Enhancement::kCatacombsEscape, "enh_catacombs_escape", &Always},
      {Enhancement::kClockRoomDoor, "enh_clock_room_door", &UnlessBingo},
      {Enhancement::kKeyItemsStay, "enh_key_items_stay", &UnlessBingo},
      {Enhancement::kDemonCardDoor, "enh_demon_card_door",
       [](const Options& o) { return IncludedIf(IsBountyHunter(o)); }},
      {Enhancement::kReverseLibraryCard, "enh_reverse_library_card",
       [](const Options& o) {
         return IncludedIf(PresetIs(o, "Expedition") || IsBountyHunter(o));
       },
       "section_items_and_shop"},
      {Enhancement::kInfiniteItems, "enh_infinite_items",
       [](const Options& o) { return IncludedIf(IsGlitchOrNoLogic(o)); }},
      {Enhancement::kFreeJewelOfOpen, "enh_free_jewel_of_open",
       [](const Options& o) {
         // Glitch sets it to 500,000 on purpose.
         return PresetIs(o, "Glitch") ? SeedUse::kConflicts : SeedUse::kIncluded;
       }},
      {Enhancement::kCheapDuplicator, "enh_cheap_duplicator",
       [](const Options& o) { return IncludedIf(o.shop); }},
      {Enhancement::kMapTracker, "enh_map_tracker",
       [](const Options& o) { return IncludedIf(o.map_tracker); }, "section_other"},
      {Enhancement::kClearFile, "enh_clear_file", &Always},
  };
  return list;
}

// Names for upstream's WS_COLORS (PS1 15-bit colors), in its order.
const char* WingSmashColorName(int index) {
  static constexpr const char* kKeys[rando::kWingSmashColorCount + 1] = {
      "color_0", "color_1", "color_2", "color_3", "color_4", "color_5", "color_6", "color_7",
      "color_8"};
  return Tr("enhancements", kKeys[index >= 0 && index <= rando::kWingSmashColorCount ? index : 0]);
}

SeedUse AlchemyVatsSeedUse(const Options&) { return SeedUse::kIncluded; }

const char* GameModeName(GameMode mode) {
  static constexpr const char* kKeys[] = {"mode_0", "mode_1", "mode_2", "mode_3"};
  return Tr("enhancements", kKeys[static_cast<size_t>(mode)]);
}

const char* GameModeDescription(GameMode mode) {
  static constexpr const char* kKeys[] = {"mode_0_desc", "mode_1_desc", "mode_2_desc",
                                          "mode_3_desc"};
  return Tr("enhancements", kKeys[static_cast<size_t>(mode)]);
}

SeedUse GameModeSeedUse(const Options& seed, GameMode mode) {
  if (mode == GameMode::kNormal) {
    return SeedUse::kNone;
  }
  if (PresetIs(seed, kGameModePresets[static_cast<size_t>(mode)])) {
    return SeedUse::kIncluded;
  }
  // These presets put their own code in the cave the modes use.
  for (const char* preset : {"Bat Master", "LegDay", "Richter Rando", "Bounty Hunter", "Hitman"}) {
    if (PresetIs(seed, preset)) {
      return SeedUse::kConflicts;
    }
  }
  return SeedUse::kNone;
}

std::vector<std::string> EnhancementCvarNames() {
  std::vector<std::string> names;
  for (const auto& e : EnhancementList()) {
    names.emplace_back(e.cvar);
  }
  names.emplace_back(kWingSmashColorCvar);
  names.emplace_back(kAlchemyVatsCvar);
  names.emplace_back(kGameModeCvar);
  return names;
}

rando::Enhancements CurrentEnhancements() {
  const bool on[] = {
      REXCVAR_GET(enh_fast_warps),           REXCVAR_GET(enh_anti_freeze),
      REXCVAR_GET(enh_remove_clock_statue),  REXCVAR_GET(enh_boost_drop_rates),
      REXCVAR_GET(enh_death_on_vacation),    REXCVAR_GET(enh_skip_maria_alchemy),
      REXCVAR_GET(enh_outer_wall_elevator),  REXCVAR_GET(enh_scylla_softlock),
      REXCVAR_GET(enh_bridge_input),         REXCVAR_GET(enh_catacombs_escape),
      REXCVAR_GET(enh_reverse_library_card), REXCVAR_GET(enh_clear_file),
      REXCVAR_GET(enh_map_tracker),          REXCVAR_GET(enh_clock_room_door),
      REXCVAR_GET(enh_key_items_stay),       REXCVAR_GET(enh_free_jewel_of_open),
      REXCVAR_GET(enh_cheap_duplicator),     REXCVAR_GET(enh_infinite_items),
      REXCVAR_GET(enh_skip_maria_silver_ring), REXCVAR_GET(enh_demon_card_door),
  };
  static_assert(std::size(on) == static_cast<size_t>(Enhancement::kCount));
  rando::Enhancements e;
  for (size_t i = 0; i < std::size(on); ++i) {
    e.flags |= on[i] ? 1u << i : 0;
  }
  const int color = REXCVAR_GET(enh_wing_smash_color);
  e.wing_smash_color = color >= 0 && color <= rando::kWingSmashColorCount ? color : 0;
  e.alchemy_vats = REXCVAR_GET(enh_alchemy_vats);
  const int mode = REXCVAR_GET(enh_game_mode);
  e.game_mode = mode > 0 && mode < static_cast<int>(GameMode::kCount) ? static_cast<GameMode>(mode)
                                                                      : GameMode::kNormal;
  return e;
}

rando::Enhancements EffectiveEnhancements(const rando::Enhancements& wanted,
                                          const Options* seed) {
  if (!seed) {
    return wanted;
  }
  rando::Enhancements e = wanted;
  for (const auto& info : EnhancementList()) {
    if (info.seed_use(*seed) != SeedUse::kNone) {
      e.Clear(info.id);
    }
  }
  if (AlchemyVatsSeedUse(*seed) != SeedUse::kNone) {
    e.alchemy_vats = 0;
  }
  if (GameModeSeedUse(*seed, e.game_mode) != SeedUse::kNone) {
    e.game_mode = GameMode::kNormal;
  }
  return e;
}

}  // namespace nocturne
