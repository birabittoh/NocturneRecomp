// Randomizer overlay (default F6): pick a preset and options, enter a seed,
// and randomize the running game with the vendored SOTN_XB_RANDO core (see
// src/rando/rando.h), applied in memory by src/rando_runtime.h. Only from the
// title screen or main menu; while a save is loaded the controls are locked.
//
// Randomizer by MottZilla: https://github.com/MottZilla/SOTN_XB_RANDO
#pragma once

#include <filesystem>
#include <memory>

namespace rex {
namespace ui {
class ImGuiDrawer;
}
}  // namespace rex

namespace nocturne {

class RandoMenuDialog;  // defined in the .cpp

class RandoMenu {
 public:
  RandoMenu();
  ~RandoMenu();

  // Registers the overlay and its keybind with the ImGui drawer
  // (OnCreateDialogs). `game_data_root` holds default.xex;
  // `user_data_root` holds the saved options and the spoiler log.
  void Attach(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& game_data_root,
              const std::filesystem::path& user_data_root);

 private:
  std::unique_ptr<RandoMenuDialog> dialog_;
};

RandoMenu& GetRandoMenu();

}  // namespace nocturne
