// Inventory inspector overlay (default I): a live view of the player-status
// API (player_status.h) for verifying the struct layout in-game -- relic
// flags, item counts, equipment slots, stats -- plus a log of the player.*
// events. Debug tool; it is also the base the randomizer's item tracker grows
// from.
#pragma once

#include <memory>

namespace rex {
class Runtime;
namespace ui {
class ImGuiDrawer;
}
}  // namespace rex

namespace nocturne {

class PlayerInspectorDialog;  // defined in the .cpp

class PlayerInspector {
 public:
  PlayerInspector();
  ~PlayerInspector();

  // Subscribes to the player.* events (call after PlayerStatus::Bind).
  void Bind(rex::Runtime* runtime);
  // Registers the overlay and its keybind (OnCreateDialogs).
  void Attach(rex::ui::ImGuiDrawer* drawer);

 private:
  std::unique_ptr<PlayerInspectorDialog> dialog_;
};

PlayerInspector& GetPlayerInspector();

}  // namespace nocturne
