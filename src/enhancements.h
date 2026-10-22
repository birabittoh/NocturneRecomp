// The randomizer core's standalone patches as game settings: one bool cvar
// per rando::Enhancement plus the Wing Smash trail color, saved to
// settings.toml with the rest of the curated settings and drawn on its
// Enhancements tab (settings.cpp).
//
// They are not part of any seed: RandoRuntime writes them over whatever world
// is in guest memory (vanilla or a seed), and like seeds they only change at
// the title screen / main menu (see src/rando_runtime.h).
#pragma once

#include <string>
#include <vector>

#include "rando/rando.h"

namespace nocturne {

struct EnhancementInfo {
  rando::Enhancement id;
  const char* cvar;
  const char* label;
  const char* description;
  // The seed options under which every seed already applies it; null: never.
  bool (*in_seed)(const rando::Options&);
};

const std::vector<EnhancementInfo>& EnhancementList();

// "Wing Smash Trail Color"; index 0 is the game's own color.
inline constexpr const char* kWingSmashColorCvar = "enh_wing_smash_color";
const char* WingSmashColorName(int index);

// Every cvar above, for saving and resetting.
std::vector<std::string> EnhancementCvarNames();

// What the cvars ask for now.
rando::Enhancements CurrentEnhancements();

}  // namespace nocturne
