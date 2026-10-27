// nocturnerecomp: adds an "Enhancements" item to both Help & Options menus,
// right under Settings, opening the settings screen on its enhancements page
// (see native_options.h's SelectSettingsPage).
//
// The game has two Help & Options screens: front-end page 4, reached from the
// main menu (How to Play / Controls / Settings / Credits, with a description
// panel for the selected item), and page 18, reached from the pause menu (the
// same four, in caps). Both open the settings screen (page 11) from their
// Settings item, and that screen goes back to whichever of the two is right
// for where the game is, so Enhancements only has to make the screen open on
// the other page. Everything else about opening it is the Settings item's own
// path, reused by handing the stock handlers the Settings item's index.
#pragma once

#include <cstdint>

struct PPCContext;

namespace nocturne {

// Repaints the Enhancements items after a language switch. Guest thread only.
void RefreshHelpOptionsText(PPCContext& ctx, uint8_t* base);

}  // namespace nocturne
