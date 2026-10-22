// See enhancements.h.

#include "enhancements.h"

#include <cstring>
#include <iterator>

#include <rex/cvar.h>

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

// Names for upstream's WS_COLORS (PS1 15-bit colors), in its order.
constexpr const char* kWingSmashColorNames[rando::kWingSmashColorCount + 1] = {
    "Default", "Magenta", "Violet", "Olive", "Red", "Blue", "Green", "Orange", "Maroon"};

struct GameModeText {
  const char* name;
  const char* preset;  // the preset whose patches these are
  const char* description;
};
constexpr GameModeText kGameModes[] = {
    {"Normal", nullptr, "The game as it is."},
    {"Bat Master", "Bat Master",
     "All bat abilities upgraded: cheap Fire of Bat and Wing Smash, free bat form, a huge Echo "
     "of Bat hitbox, and the bat opens doors. Y cancels Wing Smash; holding Y moves the bat at "
     "normal speed."},
    {"Leg Day", "LegDay",
     "Infinite Leap Stone jumping and a super backdash that opens red doors. Upstream marks it "
     "beta: other doors don't open this way yet."},
    {"Richter", "Richter Rando",
     "Play as Richter without his relics: find the Holy Glasses and all five Vlad relics to "
     "reach the end. He can use the shop, food, Library Cards and Life Apples. Library Card "
     "teleport with Faerie Scroll or Library Card: hold LB+RB and press B; use food by holding "
     "Up and pressing RT."},
};
static_assert(std::size(kGameModes) == static_cast<size_t>(GameMode::kCount));

}  // namespace

const std::vector<EnhancementInfo>& EnhancementList() {
  static const std::vector<EnhancementInfo> list = {
      {Enhancement::kFastWarps, "enh_fast_warps", "Fast Warps",
       "Warp rooms teleport faster.", [](const Options& o) { return IncludedIf(o.fast_warps); },
       "Quality of life"},
      {Enhancement::kAntiFreeze, "enh_anti_freeze", "Anti-Freeze",
       "Removes the screen freeze when you level up or pick something up.",
       [](const Options& o) { return IncludedIf(o.anti_freeze); }},
      {Enhancement::kRemoveClockStatue, "enh_remove_clock_statue", "Remove Clock Statue",
       "Removes the statue in the Marble Gallery clock room.",
       [](const Options& o) { return IncludedIf(o.clock_statue); }},
      {Enhancement::kBoostDropRates, "enh_boost_drop_rates", "Boost Enemy Drops",
       "Raises every enemy's item drop rates.",
       [](const Options& o) { return IncludedIf(o.drops && o.drop_rates); }},
      {Enhancement::kDeathOnVacation, "enh_death_on_vacation", "Death on Vacation",
       "Death doesn't take Alucard's equipment at the castle entrance.",
       [](const Options& o) { return IncludedIf(IsBountyHunter(o)); }},
      {Enhancement::kSkipMariaAlchemy, "enh_skip_maria_alchemy", "Skip Maria in Alchemy Lab",
       "Removes the meeting with Maria in the Alchemy Laboratory.", &Always},
      {Enhancement::kSkipMariaSilverRing, "enh_skip_maria_silver_ring",
       "Skip Maria at the Silver Ring", "Removes Maria's cutscene at the Silver Ring.",
       [](const Options& o) { return IncludedIf(IsGlitchOrNoLogic(o)); }},
      {Enhancement::kOuterWallElevator, "enh_outer_wall_elevator", "Outer Wall Elevator Fix",
       "The randomizer's fix for the Outer Wall elevator.", &Always, "Fixes"},
      {Enhancement::kScyllaSoftlock, "enh_scylla_softlock", "Scylla Softlock Fix",
       "Fixes a softlock in the Scylla fight in the Underground Caverns.", &Always},
      {Enhancement::kBridgeInput, "enh_bridge_input", "Bridge Collapse on Down + Y",
       "Player 1 can collapse the bridge by pressing Down + Y.", &Always},
      {Enhancement::kCatacombsEscape, "enh_catacombs_escape", "Catacombs Escape Blocks",
       "Adds blocks so you can climb out of the Catacombs without Leap Stone, Gravity Boots "
       "or flight.",
       &Always},
      {Enhancement::kClockRoomDoor, "enh_clock_room_door", "Clock Room Door Rings",
       "The Clock Room door opens with the Gold and Silver Ring in the inventory; they no "
       "longer need to be worn.",
       &UnlessBingo},
      {Enhancement::kKeyItemsStay, "enh_key_items_stay", "Key Items Stay",
       "Items lying in the castle (like the one by the Spirit Orb) don't despawn.",
       &UnlessBingo},
      {Enhancement::kDemonCardDoor, "enh_demon_card_door", "Open Demon Card Door",
       "Removes the switch door in the Abandoned Mine that needs the Demon familiar.",
       [](const Options& o) { return IncludedIf(IsBountyHunter(o)); }},
      {Enhancement::kReverseLibraryCard, "enh_reverse_library_card", "Reverse Library Card",
       "In the inverted castle, the Library Card takes you to the Reverse Library.",
       [](const Options& o) {
         return IncludedIf(PresetIs(o, "Expedition") || IsBountyHunter(o));
       },
       "Items and shop"},
      {Enhancement::kInfiniteItems, "enh_infinite_items", "Infinite-Use Items",
       "Library Card, Heart Refresh, Antivenom, Fire Boomerang and Toadstool are never used "
       "up.",
       [](const Options& o) { return IncludedIf(IsGlitchOrNoLogic(o)); }},
      {Enhancement::kFreeJewelOfOpen, "enh_free_jewel_of_open", "Free Jewel of Open",
       "The Librarian sells the Jewel of Open for nothing instead of 500 gold.",
       [](const Options& o) {
         // Glitch sets it to 500,000 on purpose.
         return PresetIs(o, "Glitch") ? SeedUse::kConflicts : SeedUse::kIncluded;
       }},
      {Enhancement::kCheapDuplicator, "enh_cheap_duplicator", "Cheaper Duplicator",
       "The Librarian's Duplicator costs 15,000 gold instead of 500,000.",
       [](const Options& o) { return IncludedIf(o.shop); }},
      {Enhancement::kMapTracker, "enh_map_tracker", "Map Tracker",
       "Marks the relic locations on the castle map, as the randomizer's in-game tracker.",
       [](const Options& o) { return IncludedIf(o.map_tracker); }, "Other"},
      {Enhancement::kClearFile, "enh_clear_file", "Cleared-Game Files",
       "Every file counts as a cleared game (the randomizer's PatchClearFile).", &Always},
  };
  return list;
}

const char* WingSmashColorName(int index) {
  return index >= 0 && index <= rando::kWingSmashColorCount ? kWingSmashColorNames[index]
                                                            : kWingSmashColorNames[0];
}

SeedUse AlchemyVatsSeedUse(const Options&) { return SeedUse::kIncluded; }

const char* GameModeName(GameMode mode) {
  return kGameModes[static_cast<size_t>(mode)].name;
}

const char* GameModeDescription(GameMode mode) {
  return kGameModes[static_cast<size_t>(mode)].description;
}

SeedUse GameModeSeedUse(const Options& seed, GameMode mode) {
  if (mode == GameMode::kNormal) {
    return SeedUse::kNone;
  }
  if (PresetIs(seed, kGameModes[static_cast<size_t>(mode)].preset)) {
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
