// fast_forward - hold a key (default Tab) or a controller button (default
// LThumb) to speed up the game.
//
// Sets the SDK's generic guest clock time-scalar
// (rex::chrono::Clock::set_guest_time_scalar), the same lever real emulators
// use for turbo: it scales how fast guest ticks, timers, and vblank interrupts
// advance relative to real time. The actual internal-framerate speed-up comes
// from src/frame_pacer.cpp, which drives the game's target_time clock at
// (real time * this scalar) -- so this file only has to publish the scalar and
// restore it to 1.0 when both inputs release (and on unbind), so a shutdown
// never leaves the game stuck sped up.
#include "fast_forward.h"

#include <string>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_DOUBLE(fast_forward_scale, 2.5, "Fast Forward",
                      "Guest time multiplier applied while the fast-forward key/button is held");

namespace nocturne {

namespace {

constexpr const char* kDefaultBindKey = "Tab";
constexpr const char* kDefaultBindButton = "LThumb";

}  // namespace

// Listens for both the keyboard hold (real key-down/up events) and the
// controller hold (polled once per UI frame via OnDraw -- XInput has no
// button-event callback, see achievements_menu.cpp's B-watcher for the same
// polling pattern in the base game). Speed-up is active whenever either
// input is held; released only once both are up.
class FastForwardWatcher : public rex::ui::WindowInputListener, public rex::ui::ImGuiDialog {
 public:
  // `window`/`input_system`/`runtime` may all still be null here -- the
  // app's own OnCreateDialogs (where this watcher is attached) runs before
  // its OnPostSetup (where FastForward::Bind supplies them), the reverse of
  // mod-loading order this code was ported from. SetContext() is called once
  // they're actually known.
  FastForwardWatcher(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer,
                     rex::input::InputSystem* input_system, rex::Runtime* runtime)
      : ImGuiDialog(drawer) {
    // Registered the same way rex::ui::RegisterBind registers its own binds
    // (same CVAR category, so these show up alongside other keybinds in the
    // settings overlay) -- RegisterBind itself only dispatches on key-down,
    // with no matching key-up, so fast-forward's hold/release needs direct
    // event/poll handling instead.
    rex::cvar::RegisterFlag({
        .name = "bind_fast_forward",
        .type = rex::cvar::FlagType::String,
        .category = "Input/Keybinds/System",
        .description = "Fast-forward",
        .setter = [this](std::string_view v) -> bool {
          bind_key_ = std::string(v);
          return true;
        },
        .getter = [this]() -> std::string { return bind_key_; },
        .lifecycle = rex::cvar::Lifecycle::kHotReload,
        .default_value = std::string(kDefaultBindKey),
    });
    rex::cvar::RegisterFlag({
        .name = "bind_fast_forward_gamepad",
        .type = rex::cvar::FlagType::String,
        .category = "Input/Keybinds/System",
        .description = "Fast-forward (controller)",
        .setter = [this](std::string_view v) -> bool {
          bind_button_ = std::string(v);
          return true;
        },
        .getter = [this]() -> std::string { return bind_button_; },
        .lifecycle = rex::cvar::Lifecycle::kHotReload,
        .default_value = std::string(kDefaultBindButton),
    });
    SetContext(window, input_system, runtime);
  }

  ~FastForwardWatcher() override {
    if (window_) {
      window_->RemoveInputListener(this);
    }
    if (key_held_ || pad_held_) {
      rex::chrono::Clock::set_guest_time_scalar(1.0);
    }
  }

  // Called once window/input_system/runtime are known -- either immediately
  // (constructor, if FastForward::Bind already ran) or later from
  // FastForward::Bind itself. The input listener is only registered once,
  // the first time a non-null window shows up.
  void SetContext(rex::ui::Window* window, rex::input::InputSystem* input_system,
                  rex::Runtime* runtime) {
    input_system_ = input_system;
    runtime_ = runtime;
    if (window_ || !window) {
      return;
    }
    window_ = window;
    window_->AddInputListener(this, 0);
  }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (key_held_ || e.virtual_key() == rex::ui::VirtualKey::kNone ||
        e.virtual_key() != rex::ui::ParseVirtualKey(bind_key_)) {
      return;
    }
    key_held_ = true;
    ApplyState();
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (!key_held_ || e.virtual_key() != rex::ui::ParseVirtualKey(bind_key_)) {
      return;
    }
    key_held_ = false;
    ApplyState();
  }

 protected:
  // Runs once per UI frame regardless of visibility (no ImGui::Begin call
  // here, so nothing is actually drawn) -- used as a per-frame poll point for
  // the controller's held state (XInput has no button-event callback).
  void OnDraw(ImGuiIO&) override {
    if (input_system_) {
      uint16_t mask = rex::ui::ParseGamepadButton(bind_button_);
      if (mask != 0) {
        rex::input::X_INPUT_STATE state{};
        input_system_->GetState(0, &state);
        const bool down = (state.gamepad.buttons & mask) != 0;
        if (down != pad_held_) {
          pad_held_ = down;
          ApplyState();
        }
      }
    }
  }

 private:
  void ApplyState() {
    rex::chrono::Clock::set_guest_time_scalar((key_held_ || pad_held_)
                                                  ? REXCVAR_GET(fast_forward_scale)
                                                  : 1.0);
  }

  rex::ui::Window* window_ = nullptr;
  rex::input::InputSystem* input_system_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::string bind_key_ = kDefaultBindKey;
  std::string bind_button_ = kDefaultBindButton;
  bool key_held_ = false;
  bool pad_held_ = false;
};

FastForward::FastForward() = default;
FastForward::~FastForward() = default;

void FastForward::Bind(rex::ui::Window* window, rex::input::InputSystem* input_system,
                       rex::Runtime* runtime) {
  window_ = window;
  input_system_ = input_system;
  runtime_ = runtime;
  // OnCreateDialogs (AttachWatcher) runs before OnPostSetup (Bind) for the
  // app itself, unlike for a mod -- if the watcher already exists, hand it
  // the now-known context so it can register its input listener.
  if (watcher_) {
    watcher_->SetContext(window_, input_system_, runtime_);
  }
}

void FastForward::AttachWatcher(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !watcher_) {
    watcher_ = std::make_unique<FastForwardWatcher>(window_, drawer, input_system_, runtime_);
  }
}

FastForward& GetFastForward() {
  static FastForward instance;
  return instance;
}

}  // namespace nocturne
