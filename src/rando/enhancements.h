// The randomizer core's standalone patches as game settings: one bool cvar
// per rando::Enhancement, plus the Wing Smash trail color, the Alchemy
// Laboratory vat shuffle and the game mode, saved to settings.toml with the
// rest of the curated settings and drawn on its Enhancements tab
// (settings.cpp).
//
// They are not part of any seed: RandoRuntime writes them over whatever world
// is in guest memory (vanilla or a seed), and change on the next guest tick
// even mid-game (see src/rando/rando_runtime.h). Over a seed, the
// ones the seed already applies or would clash with are left out
// (EffectiveEnhancements).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rando/rando.h"

namespace nocturne {

// How a seed's options relate to an enhancement.
enum class SeedUse {
  kNone,      // the seed doesn't touch it: the enhancement applies over it
  kIncluded,  // the seed applies it already
  kConflicts, // the seed's preset patches the same code differently
};

struct EnhancementInfo {
  rando::Enhancement id;
  const char* cvar;
  SeedUse (*seed_use)(const rando::Options&);
  const char* section = nullptr;  // an [enhancements] key, set on the first entry of each section
};

const std::vector<EnhancementInfo>& EnhancementList();

// "Wing Smash Trail Color"; index 0 is the game's own color. Names are in the
// current language. Seeds roll
// their own color; this one is written over it.
inline constexpr const char* kWingSmashColorCvar = "enh_wing_smash_color";
const char* WingSmashColorName(int index);

// "Shuffle Alchemy Lab Vats": the shuffle's rand() seed, 0 for off. Every
// seed shuffles the vats itself.
inline constexpr const char* kAlchemyVatsCvar = "enh_alchemy_vats";
SeedUse AlchemyVatsSeedUse(const rando::Options&);

// "Game Mode": a rando::GameMode.
inline constexpr const char* kGameModeCvar = "enh_game_mode";
const char* GameModeName(rando::GameMode mode);
const char* GameModeDescription(rando::GameMode mode);
SeedUse GameModeSeedUse(const rando::Options& seed, rando::GameMode mode);

// Every cvar above, for saving and resetting.
std::vector<std::string> EnhancementCvarNames();

// What the cvars ask for now.
rando::Enhancements CurrentEnhancements();

// `wanted` minus what `seed` (null: vanilla) already applies or clashes with.
rando::Enhancements EffectiveEnhancements(const rando::Enhancements& wanted,
                                          const rando::Options* seed);

}  // namespace nocturne
