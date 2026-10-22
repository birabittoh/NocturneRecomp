// Randomizer overlay (default F6): pick a preset and options, enter a seed,
// and generate Rando.xex from the game's own default.xex using the vendored
// SOTN_XB_RANDO core (see src/rando/rando.h). The result is selected through
// the rando_xex_name cvar, which src/rando_xex.h boots on the next launch.
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
  // (OnCreateDialogs). `game_data_root` holds default.xex and receives
  // Rando.xex; `user_data_root` holds the saved options and the spoiler log.
  void Attach(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& game_data_root,
              const std::filesystem::path& user_data_root);

 private:
  std::unique_ptr<RandoMenuDialog> dialog_;
};

RandoMenu& GetRandoMenu();

}  // namespace nocturne
