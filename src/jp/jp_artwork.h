// nocturnerecomp - show JP's artwork while World's folder is mounted, and
// switch it while the game runs.
//
// The artwork setting picks whose artwork is shown: "world", "jp", or "auto"
// (JP's while the language is Japanese). The game always boots on World's
// files; the choice is applied to what they left in guest memory, so it can
// change at any time:
//
// - MEDIA\uiresource.xpr. Both releases' files have the same layout; only a
//   few textures differ (the main menu logo, Dracula X instead of
//   Castlevania; the copyright screen; the trial upsell screen; the LT/RT
//   glyphs). The loader keeps each texture in physical memory, found through
//   the UI image bank by name, and those bytes are overwritten with the other
//   release's. The renderer hashes texture memory every frame, so the change
//   shows on the next one.
// - DATA\fonts\fonts.zip, the PS1 side's graphics (title, prologue, file
//   select, in-game text art...). The game inflates every entry it uses at
//   boot into a resident table (sub_82234E80) and builds its screens from
//   there, so the entries the releases do not share are rewritten in place
//   with JP's, or put back from a copy of World's. A screen already built
//   keeps what it took until it is next loaded.
//
// Nothing happens when JP is not installed, or when JP is the mounted folder
// already (its artwork is then the only one there is): JpArtworkAvailable()
// is false, and the settings hide the artwork row.
#pragma once

#include <cstdint>

namespace nocturne {

// Both releases are installed and World is mounted, so there is a choice.
bool JpArtworkAvailable();

// Starts following the artwork and language settings. Call once at setup.
void BindJpArtwork();

// Applies the setting to guest memory when it, the language or the loaded
// data changed. Runs from the per-frame hook (GraphicsSettings_PerFrame).
void ApplyPendingArtwork(uint8_t* base);

}  // namespace nocturne
