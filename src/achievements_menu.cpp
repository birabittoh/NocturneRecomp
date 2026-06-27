// nocturnerecomp - open/close the SDK achievements overlay from the game and
// pause the title while it is up. See achievements_menu.h for the overview.
//
// Symphony of the Night's "Achievements" pause-menu entry calls the XAM export
// XamShowAchievementsUI through the guest thunk sub_825D8028. The SDK stubs
// that export (there is no system blade), so we intercept the thunk and instead
// drive the SDK's built-in achievements overlay, pausing the game via the
// XN_SYS_UI notification:
//
//   * Selecting Achievements (guest A) -> open the overlay + XN_SYS_UI = true.
//   * Controller B -> close it + XN_SYS_UI = false.
//
// ReXApp owns the overlay privately and only exposes it through the
// `bind_achievements` keybind (default F7), so we toggle that bind via a
// synthetic key event and keep a shadow `open_` flag to make A open-only and
// B close-only. While an overlay is open the SDK forces guest controller input
// to neutral only while the mouse is captured, so we read the controller B
// directly via InputSystem::GetState each frame from a tiny watcher dialog and
// additionally zero the guest's XAM input reads so the pause menu underneath
// can't navigate.

#include "achievements_menu.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "generated/nocturnerecomp_init.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

// Original guest thunks (real recompiled bodies; the weak `sub_*` aliases are
// overridden by the strong hook definitions below).
extern "C" REX_FUNC(__imp__sub_825D8028);  // -> XamShowAchievementsUI
extern "C" REX_FUNC(__imp__sub_825DAF30);  // -> XamInputGetState (r3=user, r4=X_INPUT_STATE*)
extern "C" REX_FUNC(__imp__sub_825DAF70);  // -> XamInputGetKeystrokeEx (thin)
extern "C" REX_FUNC(__imp__sub_825DAF90);  // -> XamInputGetKeystrokeEx (stack wrapper)

namespace nocturne {
namespace {

// XN_SYS_UI notification id. Broadcasting this true/false is how a real console
// tells titles that system UI has appeared/closed; games freeze themselves
// while it is set (matching xam_ui.cpp's dialog dispatch).
constexpr uint32_t kXNotificationSystemUI = 0x9;

// Pause/resume the guest by telling it system UI is (in)active. Safe from any
// thread: BroadcastNotification only enqueues to listeners under a lock. Uses
// the global kernel_state() accessor (not the thread-bound REX_KERNEL_STATE)
// because the close path runs on the UI thread.
void SetGuestPaused(bool paused) {
  auto* ks = rex::system::kernel_state();
  if (ks) {
    ks->BroadcastNotification(kXNotificationSystemUI, paused ? 1u : 0u);
  }
}

// Scroll the overlay's achievement list with the left stick. The list lives in
// the SDK overlay's child window "##achlist"; nudge that window's scroll offset
// directly (imgui_internal) so it works regardless of mouse position.
void ScrollAchievementsList(float stick_y, float dt) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g || stick_y == 0.0f) {
    return;
  }
  constexpr float kPixelsPerSecond = 1400.0f;
  for (ImGuiWindow* window : g->Windows) {
    if (window->Name && std::strstr(window->Name, "##achlist")) {
      // Stick up (positive thumb_ly) scrolls toward the top of the list.
      float target = window->Scroll.y - stick_y * kPixelsPerSecond * dt;
      target = ImClamp(target, 0.0f, window->ScrollMax.y);
      window->Scroll.y = target;
      break;
    }
  }
}

}  // namespace

// No-window dialog whose only job is to drive the overlay from the controller
// every UI frame. ImGuiDialog auto-registers with the drawer in its constructor.
class AchievementsInputWatcher : public rex::ui::ImGuiDialog {
 public:
  explicit AchievementsInputWatcher(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    AchievementsMenu& menu = Achievements();
    menu.PollForClose();
    ScrollAchievementsList(menu.LeftStickY(), io.DeltaTime);
  }
};

AchievementsMenu::AchievementsMenu() = default;
AchievementsMenu::~AchievementsMenu() = default;

AchievementsMenu& Achievements() {
  static AchievementsMenu instance;
  return instance;
}

void AchievementsMenu::Bind(rex::ui::Window* window, rex::ui::WindowedAppContext* context,
                            rex::input::InputSystem* input_system) {
  window_ = window;
  context_ = context;
  input_ = input_system;
}

void AchievementsMenu::AttachWatcher(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !watcher_) {
    watcher_ = std::make_unique<AchievementsInputWatcher>(drawer);
  }
}

void AchievementsMenu::ToggleOverlayBindOnUIThread() {
  // Synthesize the bind_achievements key -- the public way to toggle ReXApp's
  // privately-owned achievements overlay. Read the bind's current key so a
  // rebind is respected; fall back to its F7 default.
  std::string key = rex::cvar::GetFlagByName("bind_achievements");
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(key.empty() ? "F7" : key);
  rex::ui::KeyEvent key_event(window_, vk, /*repeat_count=*/1, /*prev_state=*/false,
                              /*modifier_shift_pressed=*/false, /*modifier_ctrl_pressed=*/false,
                              /*modifier_alt_pressed=*/false, /*modifier_super_pressed=*/false);
  rex::ui::ProcessKeyEvent(key_event);
}

void AchievementsMenu::OpenFromGuest() {
  if (!context_) {
    return;
  }
  // The hook runs on a guest thread; toggle on the UI thread. Open-only: ignore
  // repeat selects while the overlay is already up.
  context_->CallInUIThreadDeferred([this] {
    if (!open_.load()) {
      ToggleOverlayBindOnUIThread();
      open_.store(true);
      SetGuestPaused(true);  // freeze gameplay/audio while the overlay is up
    }
  });
}

void AchievementsMenu::PollForClose() {
  // Poll while the overlay is open, and also while we're still swallowing the
  // closing B press (waiting for release).
  if (!input_ || (!open_.load() && !lock_until_b_release_.load())) {
    prev_b_down_ = false;  // re-arm the edge for the next open
    return;
  }
  // Zero-initialized, so a failed/disconnected GetState leaves buttons neutral.
  rex::input::X_INPUT_STATE state{};
  input_->GetState(0, &state);
  const uint16_t buttons = state.gamepad.buttons;
  const bool b_down = (buttons & rex::input::X_INPUT_GAMEPAD_B) != 0;
  if (open_.load()) {
    if (b_down && !prev_b_down_) {
      ToggleOverlayBindOnUIThread();  // already on the UI thread (OnDraw)
      open_.store(false);
      SetGuestPaused(false);  // resume the game
      // Keep the guest locked until B is released so the press that closed the
      // overlay doesn't reach the game's menu (and back it out).
      lock_until_b_release_.store(true);
    }
  } else if (lock_until_b_release_.load() && !b_down) {
    lock_until_b_release_.store(false);
  }
  prev_b_down_ = b_down;
}

float AchievementsMenu::LeftStickY() {
  if (!input_ || !open_.load()) {
    return 0.0f;
  }
  rex::input::X_INPUT_STATE state{};
  input_->GetState(0, &state);
  const int16_t ly = state.gamepad.thumb_ly;  // up is positive
  constexpr int16_t kDeadzone = 8000;          // ~X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE
  if (ly > -kDeadzone && ly < kDeadzone) {
    return 0.0f;
  }
  return static_cast<float>(ly) / 32767.0f;
}

}  // namespace nocturne

// sub_825D8028(...) -> XamShowAchievementsUI (SDK stub). Open our overlay and
// pause the game, then run the original so the guest sees behavior identical to
// the stub.
REX_HOOK_RAW(sub_825D8028) {
  REXLOG_INFO("[achievements] guest opened Achievements UI -> opening overlay");
  nocturne::Achievements().OpenFromGuest();
  __imp__sub_825D8028(ctx, base);
}

// While the overlay is open, lock the game's controller input so its pause menu
// can't act (and back out) underneath it. The overlay is closed by our own
// host-side B watcher, which reads the controller independently.

// sub_825DAF30(user=r3, X_INPUT_STATE* state=r4) -> XamInputGetState. After the
// original fills the state, neutralize the gamepad (12 bytes at state+4) so the
// guest sees a controller with nothing pressed.
REX_HOOK_RAW(sub_825DAF30) {
  const uint32_t state_ptr = ctx.r4.u32;
  __imp__sub_825DAF30(ctx, base);
  if (state_ptr && nocturne::Achievements().ShouldSuppressGuestInput()) {
    std::memset(base + state_ptr + 4, 0, sizeof(rex::input::X_INPUT_GAMEPAD));
  }
}

// sub_825DAF70 / sub_825DAF90(...) -> XamInputGetKeystrokeEx. Report "no
// keystroke" (X_ERROR_EMPTY) so menu navigation/back presses are swallowed.
REX_HOOK_RAW(sub_825DAF70) {
  __imp__sub_825DAF70(ctx, base);
  if (nocturne::Achievements().ShouldSuppressGuestInput()) {
    ctx.r3.u32 = 0x000010D2u;  // X_ERROR_EMPTY (ERROR_EMPTY)
  }
}

REX_HOOK_RAW(sub_825DAF90) {
  __imp__sub_825DAF90(ctx, base);
  if (nocturne::Achievements().ShouldSuppressGuestInput()) {
    ctx.r3.u32 = 0x000010D2u;  // X_ERROR_EMPTY (ERROR_EMPTY)
  }
}
