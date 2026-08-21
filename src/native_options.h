// nocturnerecomp - appends extra rows to the game's own settings screen
// (main menu, or pause -> help & options -> settings), driven by the same
// left/right in-place cycling the stock rows use.
//
// To add a row, append an entry to kCustomRows in native_options.cpp -- see
// the "Adding a row" section at the top of that file. It also documents the
// reverse-engineering the whole thing rests on.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

// Arms this file's pre-game watchdog suppression for `page`, or disarms it when
// `page` is 0.
//
// With no game running, the UI manager's per-frame update runs a watchdog that
// yanks the front end back to the previous screen the frame after any screen
// whose Activate clears the manager's "settled" flag (mgr+332) opens. The
// stretch screen does that, and so does CScreenAchievement -- so both need the
// same one-shot suppression, which NativeOptions_PostEvent owns because it
// already overrides the single page-switch queue push every screen goes
// through. See the comment on NativeOptions_PostEvent for the full rationale.
//
// Call from a guest thread just before posting the switch to `page`, and only
// when no save is loaded; the suppression disarms itself on the way back out.
void EnterPregameScreen(uint32_t page);

class NativeOptions {
 public:
  // Installs the guest function overrides. Call once runtime() is live
  // (OnPostSetup), same as GraphicsSettings::Bind -- every address here is
  // hardcoded against the game's own pinned build.
  void Bind(rex::Runtime* runtime);

 private:
  rex::Runtime* runtime_ = nullptr;
};

// Process-wide instance shared between the app hooks.
NativeOptions& GetNativeOptions();

}  // namespace nocturne
