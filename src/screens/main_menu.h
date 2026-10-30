// nocturnerecomp: adds a "Randomizer" item to the main menu, right under
// Single Player, opening the settings screen on its randomizer pages (see
// native_options.h's SelectSettingsPage and native_randomizer.cpp). With that
// item selected, the description panel shows the randomizer's state: the seed
// New Game gets, or a run in progress.
//
// The main menu (front-end page 1) is a six-item list: Single Player,
// Leaderboards, Achievements, Help & Options, then "Unlock Full Game" and
// "Return to Arcade" (relabelled Quit to launcher, see font_overlay.cpp), of which
// its Activate keeps one: the full game moves Quit up onto the Unlock item
// and hides the last. Ours goes in at index 1 and every stock item moves down
// one, so each hook here hands the stock code the index it was written for.
#pragma once

#include <cstdint>

struct PPCContext;

namespace nocturne {

// From the shared menu-list setup hook (help_options_menu.cpp), before the
// original runs: one more item for the main menu's list.
void AdjustMainMenuListSetup(PPCContext& ctx);

// Repaints the Randomizer item after a language switch. Guest thread only.
void RefreshMainMenuText(PPCContext& ctx, uint8_t* base);

}  // namespace nocturne
