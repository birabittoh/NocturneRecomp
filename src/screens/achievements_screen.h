// nocturnerecomp - route the guest "Achievements" menu entry to the game's own
// CScreenAchievement front-end screen instead of a system blade.
//
// Symphony of the Night ships a real achievements screen it never uses. The UI
// manager's init (sub_825ABED0) builds a 22-entry screen array, and slot 15 is
// a `CScreenAchievement` -- the class name comes straight out of the image: the
// string "CScreenAchievement::NotifyLeave " sits immediately after its vtable
// (off_820140A8), and its Activate logs "CScreenAchievement::Activate\n". Both
// "Achievements" menu entries (main menu, and the pause-side menu) instead call
// XamShowAchievementsUI to raise the Xbox 360 blade, so on real hardware that
// screen is dead code. Under recompilation there is no blade, so we send them
// to the screen the game already has.
//
// The screen is a full-screen image panel: its build (sub_825B6F68) makes one
// image widget at screen+548, and its Activate (sub_825B7000) points that
// widget at image-bank entry "ACHIEVEMENT_<n>" chosen by the page-switch
// event's arg1, for n in 1..12. Any other arg1 falls out of that switch and
// leaves the widget empty -- so posting arg1 = 0 opens the screen blank, which
// is where this starts.
//
// See achievements_menu.h for the SDK ImGui overlay this displaces as the
// destination of the guest button; that overlay is still built and still
// toggleable, it is just no longer what the menu entry opens.
#pragma once

#include <cstdint>

struct PPCContext;

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

class AchievementsScreen {
 public:
  // Installs the guest function overrides. Call once runtime() is live
  // (OnPostSetup), same as NativeOptions::Bind -- every address here is
  // hardcoded against the game's own pinned build.
  void Bind(rex::Runtime* runtime);

  // True once Bind found everything it needs; when false the caller should fall
  // back to whatever it did before (the SDK overlay).
  bool available() const { return available_; }

  // From the guest XamShowAchievementsUI hook, on a guest thread with that
  // thunk's live context: remember where we came from and post the page switch
  // to the achievements screen. Returns false if it could not, in which case
  // nothing was posted and the caller still owns the button.
  bool OpenFromGuest(PPCContext& ctx, uint8_t* base, uint32_t user_index);

 private:
  rex::Runtime* runtime_ = nullptr;
  bool available_ = false;
};

// Process-wide instance shared between the guest hook and the app.
AchievementsScreen& GetAchievementsScreen();

}  // namespace nocturne
