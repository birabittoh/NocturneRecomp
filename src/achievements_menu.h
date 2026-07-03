// nocturnerecomp - bridge the guest "Achievements" pause-menu entry to the SDK
// overlay, pausing the game while it is open.
//
// Symphony of the Night's pause menu has an "Achievements" entry that, on real
// hardware, calls XamShowAchievementsUI to raise the Xbox 360 achievements
// blade. There is no system blade under recompilation, so we intercept the
// guest wrapper that would call it (sub_825D8028) and instead drive the SDK's
// built-in ImGui achievements overlay:
//
//   * Selecting Achievements (guest A) -> open the overlay + pause the game.
//   * Controller B -> close it (like backing out of a menu) + resume the game.
//
// The game is paused by broadcasting XN_SYS_UI = true (the same notification a
// real console raises when system UI appears); well-behaved titles freeze
// gameplay/audio while it is set and resume when it clears. We additionally
// lock the guest's controller reads so the underlying pause menu can't navigate
// out from under the overlay.
//
// ReXApp owns the overlay privately and only exposes it through the
// `bind_achievements` keybind (default F7), so we toggle that bind via a
// synthetic key event and keep a shadow `open_` flag to make A = open-only and
// B = close-only.
//
// The bind is otherwise a live hotkey, letting a player pop the overlay
// directly (skipping the XN_SYS_UI pause) and showing a misleading "F7" entry
// in Settings even though the intended way in is the guest pause menu. Bind()
// clears the bind_achievements cvar so it reads as unbound; our own toggle
// (ToggleOverlayBindOnUIThread) briefly re-arms it with its real key just long
// enough to drive the synthetic key event, then clears it again.
#pragma once

#include <atomic>
#include <memory>

namespace rex {
namespace ui {
class ImGuiDrawer;
class Window;
class WindowedAppContext;
}  // namespace ui
namespace input {
class InputSystem;
}  // namespace input
}  // namespace rex

namespace nocturne {

class AchievementsInputWatcher;  // per-frame B poller, defined in the .cpp

class AchievementsMenu {
 public:
  AchievementsMenu();
  ~AchievementsMenu();

  // Provide live UI objects. Call once setup completes (window/context from
  // OnPostSetup); the input system is polled for the controller close button.
  // Also clears the bind_achievements cvar -- see the file comment above.
  void Bind(rex::ui::Window* window, rex::ui::WindowedAppContext* context,
            rex::input::InputSystem* input_system);

  // Register the per-frame input watcher with the ImGui drawer (OnCreateDialogs).
  void AttachWatcher(rex::ui::ImGuiDrawer* drawer);

  // From the guest XamShowAchievementsUI hook (guest thread): open the overlay
  // and pause the game.
  void OpenFromGuest();

  // From the watcher each UI frame: close the overlay (and resume the game)
  // when B is pressed.
  void PollForClose();

  // Left-stick Y in [-1, 1] (up positive) while the overlay is open, else 0.
  // Used to scroll the achievement list with the controller.
  float LeftStickY();

  // True while our achievements overlay is up (drives scrolling / scroll-read).
  bool IsOverlayOpen() const { return open_.load(); }

  // True while the guest's controller input must be locked: the overlay is open,
  // OR we are still swallowing the B press that closed it (until B is released)
  // so it doesn't leak through and back the game's menu out.
  bool ShouldSuppressGuestInput() const { return open_.load() || lock_until_b_release_.load(); }

 private:
  void ToggleOverlayBindOnUIThread();

  rex::ui::Window* window_ = nullptr;
  rex::ui::WindowedAppContext* context_ = nullptr;
  rex::input::InputSystem* input_ = nullptr;
  std::unique_ptr<AchievementsInputWatcher> watcher_;
  std::atomic<bool> open_{false};
  std::atomic<bool> lock_until_b_release_{false};
  bool prev_b_down_ = false;
};

// Process-wide instance shared between the guest hook and the app.
AchievementsMenu& Achievements();

}  // namespace nocturne
