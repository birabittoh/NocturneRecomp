// nocturnerecomp - hold a key (default Ctrl) or a controller button (default
// RB) to speed up the game. Moved in-app from the former fast_forward mod;
// see fast_forward.cpp for the two-mechanism approach (guest_time_scalar +
// direct target_time bump) and why both are needed.
#pragma once

#include <memory>

namespace rex {
class Runtime;
namespace input {
class InputSystem;
}  // namespace input
namespace ui {
class ImGuiDrawer;
class Window;
}  // namespace ui
}  // namespace rex

namespace nocturne {

class FastForwardWatcher;  // key/gamepad listener + per-frame poller, defined in the .cpp

class FastForward {
 public:
  FastForward();
  ~FastForward();

  // Provide live UI/runtime objects. Call once window()/input system/runtime
  // are all live (OnPostSetup).
  void Bind(rex::ui::Window* window, rex::input::InputSystem* input_system,
            rex::Runtime* runtime);

  // Registers the key listener and per-frame gamepad/target_time watcher with
  // the ImGui drawer (OnCreateDialogs).
  void AttachWatcher(rex::ui::ImGuiDrawer* drawer);

 private:
  rex::ui::Window* window_ = nullptr;
  rex::input::InputSystem* input_system_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<FastForwardWatcher> watcher_;
};

// Process-wide instance shared between the app hooks.
FastForward& GetFastForward();

}  // namespace nocturne
