// nocturnerecomp - Host menu, opened by the Android back button (see
// window_sdl.cpp's SDL_HINT_ANDROID_TRAP_BACK_BUTTON). Backed by a real Android
// AlertDialog (see host_menu.cpp and NocturneRecompActivity.showHostMenu), not
// an ImGui overlay: it needs to draw as its own window above the touch controls
// and the game surface, both of which ImGui overlays share a render target with.
// Android only. The F-key overlays are reached through the SDK's overlay menu.
//
// It carries the actions that have no overlay behind them and that an Android
// user cannot perform for themselves, because Android 11+ hides Android/data
// from on-device file managers: picking a GPU driver. Saves are
// imported and exported, and mods installed, on the start screen.
#pragma once

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace nocturne {

class HostMenu {
 public:
  explicit HostMenu(rex::ui::Window* window);
  ~HostMenu();

  HostMenu(const HostMenu&) = delete;
  HostMenu& operator=(const HostMenu&) = delete;

  // Used by the JNI callbacks in host_menu.cpp to reach the window that owns
  // the SDL/UI thread the callback has to marshal onto.
  rex::ui::Window* window_for_callback() const { return window_; }

 private:
  rex::ui::Window* window_;
};

}  // namespace nocturne
