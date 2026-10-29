// Randomizer overlay (default F5): pick a preset and options, enter a seed,
// and randomize the running game with the vendored SOTN_XB_RANDO core (see
// src/rando/rando.h), applied in memory by src/rando/rando_runtime.h. Only from the
// title screen or main menu: once a game is running (file select on), the
// overlay is a read-only tracker of owned relics and items, with the active
// seed's spoiler log behind a toggle.
//
// The main menu's Randomizer entry (main_menu.h) edits the same options on
// the game's own settings screen (native_randomizer.cpp), so both go through
// options() and Randomize() here.
//
// Randomizer by MottZilla: https://github.com/MottZilla/SOTN_XB_RANDO
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

#include "rando/rando.h"

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
  // (OnCreateDialogs). `user_data_root` holds the saved options and the
  // spoiler log.
  void Attach(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& user_data_root);

  // The options the overlay and the main menu's Randomizer pages
  // (native_randomizer.cpp) edit, saved in randomizer.txt without the seed,
  // which lasts for the session. Any thread.
  rando::Options options() const;
  // Bumped by every change to options(), so an editor knows to reload.
  uint32_t options_revision() const { return revision_.load(); }
  // Saves `options` as the ones being edited.
  void SetOptions(const rando::Options& options);

  // Saves `options` and generates a seed from them, applied once it is done
  // (see Poll). False when there is nothing to do or it can't start: a run
  // or a request is already going, the randomizer isn't supported, or that
  // seed is already the active one. Any thread.
  bool Randomize(const rando::Options& options);
  // Hands a finished run to the runtime. Called every UI frame.
  void Poll();

 private:
  std::unique_ptr<RandoMenuDialog> dialog_;
  std::filesystem::path user_root_;
  mutable std::mutex mutex_;
  rando::Options options_;  // under mutex_
  std::atomic<uint32_t> revision_{0};
  // Randomize started a run that Poll hasn't applied yet.
  std::atomic<bool> apply_pending_{false};
};

RandoMenu& GetRandoMenu();

}  // namespace nocturne
