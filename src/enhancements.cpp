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
REXCVAR_DEFINE_INT32(enh_wing_smash_color, 0, "Enhancements",
                     "Wing Smash trail color (0: the game's own, 1-8: randomizer colors)");

namespace nocturne {

namespace {

using rando::Enhancement;
using rando::Options;

bool PresetIs(const Options& o, const char* name) {
  const auto& presets = rando::Presets();
  return o.preset >= 0 && static_cast<size_t>(o.preset) < presets.size() &&
         std::strcmp(presets[o.preset].name, name) == 0;
}

// XB_Randomize applies these whatever the options.
bool Always(const Options&) { return true; }

// Names for upstream's WS_COLORS (PS1 15-bit colors), in its order.
constexpr const char* kWingSmashColorNames[rando::kWingSmashColorCount + 1] = {
    "Default", "Magenta", "Violet", "Olive", "Red", "Blue", "Green", "Orange", "Maroon"};

}  // namespace

const std::vector<EnhancementInfo>& EnhancementList() {
  static const std::vector<EnhancementInfo> list = {
      {Enhancement::kFastWarps, "enh_fast_warps", "Fast Warps",
       "Warp rooms teleport faster.", [](const Options& o) { return o.fast_warps; }},
      {Enhancement::kAntiFreeze, "enh_anti_freeze", "Anti-Freeze",
       "Removes the screen freeze when you level up or pick something up.",
       [](const Options& o) { return o.anti_freeze; }},
      {Enhancement::kRemoveClockStatue, "enh_remove_clock_statue", "Remove Clock Statue",
       "Removes the statue in the Marble Gallery clock room.",
       [](const Options& o) { return o.clock_statue; }},
      {Enhancement::kBoostDropRates, "enh_boost_drop_rates", "Boost Enemy Drops",
       "Raises every enemy's item drop rates.",
       [](const Options& o) { return o.drops && o.drop_rates; }},
      {Enhancement::kDeathOnVacation, "enh_death_on_vacation", "Death on Vacation",
       "Death doesn't take Alucard's equipment at the castle entrance.",
       [](const Options& o) { return PresetIs(o, "Bounty Hunter") || PresetIs(o, "Hitman"); }},
      {Enhancement::kSkipMariaAlchemy, "enh_skip_maria_alchemy", "Skip Maria in Alchemy Lab",
       "Removes the meeting with Maria in the Alchemy Laboratory.", &Always},
      {Enhancement::kOuterWallElevator, "enh_outer_wall_elevator", "Outer Wall Elevator Fix",
       "The randomizer's fix for the Outer Wall elevator.", &Always},
      {Enhancement::kScyllaSoftlock, "enh_scylla_softlock", "Scylla Softlock Fix",
       "Fixes a softlock in the Scylla fight in the Underground Caverns.", &Always},
      {Enhancement::kBridgeInput, "enh_bridge_input", "Bridge Collapse on Down + Y",
       "Player 1 can collapse the bridge by pressing Down + Y.", &Always},
      {Enhancement::kCatacombsEscape, "enh_catacombs_escape", "Catacombs Escape Blocks",
       "Adds blocks so you can climb out of the Catacombs without Leap Stone, Gravity Boots "
       "or flight.",
       &Always},
      {Enhancement::kReverseLibraryCard, "enh_reverse_library_card", "Reverse Library Card",
       "In the inverted castle, the Library Card takes you to the Reverse Library.",
       [](const Options& o) {
         return PresetIs(o, "Expedition") || PresetIs(o, "Bounty Hunter") ||
                PresetIs(o, "Hitman");
       }},
      {Enhancement::kClearFile, "enh_clear_file", "Cleared-Game Files",
       "Every file counts as a cleared game (the randomizer's PatchClearFile).", &Always},
  };
  return list;
}

const char* WingSmashColorName(int index) {
  return index >= 0 && index <= rando::kWingSmashColorCount ? kWingSmashColorNames[index]
                                                            : kWingSmashColorNames[0];
}

std::vector<std::string> EnhancementCvarNames() {
  std::vector<std::string> names;
  for (const auto& e : EnhancementList()) {
    names.emplace_back(e.cvar);
  }
  names.emplace_back(kWingSmashColorCvar);
  return names;
}

rando::Enhancements CurrentEnhancements() {
  const bool on[] = {
      REXCVAR_GET(enh_fast_warps),          REXCVAR_GET(enh_anti_freeze),
      REXCVAR_GET(enh_remove_clock_statue), REXCVAR_GET(enh_boost_drop_rates),
      REXCVAR_GET(enh_death_on_vacation),   REXCVAR_GET(enh_skip_maria_alchemy),
      REXCVAR_GET(enh_outer_wall_elevator), REXCVAR_GET(enh_scylla_softlock),
      REXCVAR_GET(enh_bridge_input),        REXCVAR_GET(enh_catacombs_escape),
      REXCVAR_GET(enh_reverse_library_card), REXCVAR_GET(enh_clear_file),
  };
  static_assert(std::size(on) == static_cast<size_t>(Enhancement::kCount));
  rando::Enhancements e;
  for (size_t i = 0; i < std::size(on); ++i) {
    e.flags |= on[i] ? 1u << i : 0;
  }
  const int color = REXCVAR_GET(enh_wing_smash_color);
  e.wing_smash_color = color >= 0 && color <= rando::kWingSmashColorCount ? color : 0;
  return e;
}

}  // namespace nocturne
