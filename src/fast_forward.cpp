// fast_forward - hold a key (default Ctrl) or a controller button (default
// RB) to speed up the game.
//
// Two complementary mechanisms:
//
// 1. Uses the SDK's generic guest clock time-scalar
//    (rex::chrono::Clock::set_guest_time_scalar), the same lever real
//    emulators use for turbo/fast-forward: it scales how fast guest ticks,
//    timers, and (via KernelState::StartHeadlessVblankThreadIfNeeded, which
//    paces off Clock::QueryGuestTickCount) vblank interrupts advance
//    relative to real time. Pure SDK mechanism, see rexglue-sdk
//    src/core/clock.cpp.
//
// 2. Directly advances the game's own fixed-timestep catch-up clock
//    (target_time, see docs/native-renderer-pacing-investigation.md) -- (1)
//    alone turned out NOT to speed up actual gameplay: target_time only ever
//    advances via a per-vblank path clamped to 1-2 quanta regardless of
//    vblank rate, so scaling vblank delivery sped up the vblank ISR without
//    speeding up simulation. Bumping target_time directly drives the game's
//    own designed catch-up do-while (sub_8258B3B8) to run extra Update()s,
//    which is the actual internal-framerate lever. Vanilla-build address
//    only (see game_symbols's "app.singleton_ptr"); guarded by a
//    plausibility check before ever writing.
//
// Both are restored/stopped whenever both the key and button are released,
// and on unbind, so a shutdown never leaves the game stuck sped up.
#include "fast_forward.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>
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

// Application-object field offsets (see docs/native-renderer-pacing-
// investigation.md, "full frame-pacing chain reverse-engineered").
constexpr uint32_t kTargetTimeOffset = 2232;
constexpr uint32_t kGameTimeOffset = 2236;
constexpr uint32_t kAppStructSpan = kGameTimeOffset + 4;

// 300ths-of-a-second units per real second at 1x speed (60Hz * 5 units/quantum).
constexpr double kUnitsPerSecondAt1x = 300.0;
// Caps a single OnDraw call's target_time bump so a lag spike or very high
// scale can't force one outer mode-loop call into an enormous synchronous
// catch-up burst -- mirrors the kMaxCatchUpTicks-style bound rexglue-sdk's
// own headless vblank thread already applies elsewhere
// (KernelState::StartHeadlessVblankThreadIfNeeded).
constexpr double kMaxUnitsPerCall = 150.0;
// Plausibility bounds for the dereferenced Application struct -- guards
// against running on a build (e.g. TU) where this address doesn't hold what
// vanilla does, without needing a second known-good address to compare
// against.
constexpr int64_t kMaxPlausibleTimeValue = 100'000'000;
constexpr int64_t kMaxPlausibleTimeDelta = 10'000;

int32_t ReadGuestI32BE(rex::memory::Memory* memory, uint32_t guest_address) {
  const uint8_t* host_address = memory->TranslateVirtual<const uint8_t*>(guest_address);
  return static_cast<int32_t>(rex::memory::load_and_swap<uint32_t>(host_address));
}

void WriteGuestI32BE(rex::memory::Memory* memory, uint32_t guest_address, int32_t value) {
  uint8_t* host_address = memory->TranslateVirtual<uint8_t*>(guest_address);
  rex::memory::store_and_swap<uint32_t>(host_address, static_cast<uint32_t>(value));
}

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
  // here, so nothing is actually drawn) -- used as a per-frame poll point
  // for the controller's held state, as the lazy resolve point for
  // game_symbols's "app.singleton_ptr" (published in its own
  // OnCreateDialogs, which may run before or after this one), and as the
  // per-frame tick for the target_time bump below (real elapsed time is
  // measured here rather than assuming a fixed rate, since this can run at
  // monitor refresh -- see src/repaint_pump.h).
  void OnDraw(ImGuiIO&) override {
    if (!addr_resolved_ && runtime_ && runtime_->mod_registry()) {
      if (auto addr = runtime_->mod_registry()->FindAddress("app.singleton_ptr")) {
        app_singleton_ptr_addr_ = *addr;
        addr_resolved_ = true;
      }
    }

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

    auto now = std::chrono::steady_clock::now();
    bool active = key_held_ || pad_held_;
    if (!active) {
      last_tick_time_ = now;
      target_time_remainder_ = 0.0;
      return;
    }

    double scale = REXCVAR_GET(fast_forward_scale);
    double dt = std::chrono::duration<double>(now - last_tick_time_).count();
    last_tick_time_ = now;
    if (!(scale > 1.0) || !(dt > 0.0)) {
      return;
    }

    BumpTargetTime(std::min((scale - 1.0) * kUnitsPerSecondAt1x * dt, kMaxUnitsPerCall));
  }

 private:
  void ApplyState() {
    rex::chrono::Clock::set_guest_time_scalar((key_held_ || pad_held_)
                                                  ? REXCVAR_GET(fast_forward_scale)
                                                  : 1.0);
  }

  // Dereferences app.singleton_ptr and, if the pointed-to struct passes a
  // plausibility check (bounded, sane-looking game_time/target_time), adds
  // `units` to target_time -- see this file's header comment for why this,
  // not guest_time_scalar, is the lever that actually speeds up gameplay.
  // Silently no-ops (once-logged) if the address hasn't resolved or the
  // guard fails, e.g. running under a build (TU) this address wasn't found
  // for -- never trusts a wrong-build pointer.
  void BumpTargetTime(double units) {
    if (units <= 0.0 || !addr_resolved_ || !runtime_ || !runtime_->memory()) {
      return;
    }
    auto* memory = runtime_->memory();

    auto* ptr_heap = memory->LookupHeap(app_singleton_ptr_addr_);
    if (!ptr_heap || ptr_heap->QueryRangeAccess(app_singleton_ptr_addr_,
                                                app_singleton_ptr_addr_ + 3) ==
                        rex::memory::PageAccess::kNoAccess) {
      LogGuardFailureOnce("singleton pointer slot unreadable");
      return;
    }
    uint32_t app_base =
        static_cast<uint32_t>(ReadGuestI32BE(memory, app_singleton_ptr_addr_));
    if (app_base == 0) {
      LogGuardFailureOnce("singleton pointer is null (game not launched yet?)");
      return;
    }

    auto* app_heap = memory->LookupHeap(app_base);
    if (!app_heap || app_heap->QueryRangeAccess(app_base, app_base + kAppStructSpan - 1) ==
                        rex::memory::PageAccess::kNoAccess) {
      LogGuardFailureOnce("dereferenced app struct unreadable");
      return;
    }

    int32_t game_time = ReadGuestI32BE(memory, app_base + kGameTimeOffset);
    int32_t target_time = ReadGuestI32BE(memory, app_base + kTargetTimeOffset);
    int64_t delta = static_cast<int64_t>(target_time) - static_cast<int64_t>(game_time);
    if (std::abs(static_cast<int64_t>(game_time)) > kMaxPlausibleTimeValue ||
        std::abs(static_cast<int64_t>(target_time)) > kMaxPlausibleTimeValue ||
        std::abs(delta) > kMaxPlausibleTimeDelta) {
      LogGuardFailureOnce("game_time/target_time values look implausible");
      return;
    }

    // Carry the fractional part of `units` forward so small per-call
    // truncation doesn't lose the bump over time.
    double whole = std::floor(units);
    int64_t add = static_cast<int64_t>(whole);
    target_time_remainder_ += units - whole;
    if (target_time_remainder_ >= 1.0) {
      add += 1;
      target_time_remainder_ -= 1.0;
    }
    int64_t new_target = static_cast<int64_t>(target_time) + add;
    WriteGuestI32BE(memory, app_base + kTargetTimeOffset, static_cast<int32_t>(new_target));
  }

  void LogGuardFailureOnce(const char* reason) {
    if (guard_failure_logged_) {
      return;
    }
    guard_failure_logged_ = true;
    REXLOG_INFO("fast_forward: target_time bump disabled ({}) -- falling back to "
               "guest_time_scalar-only speed-up",
               reason);
  }

  rex::ui::Window* window_ = nullptr;
  rex::input::InputSystem* input_system_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::string bind_key_ = kDefaultBindKey;
  std::string bind_button_ = kDefaultBindButton;
  bool key_held_ = false;
  bool pad_held_ = false;

  bool addr_resolved_ = false;
  uint32_t app_singleton_ptr_addr_ = 0;
  bool guard_failure_logged_ = false;
  std::chrono::steady_clock::time_point last_tick_time_ = std::chrono::steady_clock::now();
  double target_time_remainder_ = 0.0;
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
