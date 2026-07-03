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

class AudioPlayerDialog;

class AudioPlayer {
 public:
  AudioPlayer();
  ~AudioPlayer();

  void Bind(rex::ui::Window* window, rex::ui::WindowedAppContext* context);
  void AttachDialog(rex::ui::ImGuiDrawer* drawer);

 private:
  rex::ui::Window* window_ = nullptr;
  rex::ui::WindowedAppContext* context_ = nullptr;
  std::unique_ptr<AudioPlayerDialog> dialog_;
};

AudioPlayer& GetAudioPlayer();

}  // namespace nocturne
