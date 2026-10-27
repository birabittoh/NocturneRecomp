#pragma once

// The start screen's Saves page: the game's save files with their memory card
// icons. A save is exported to a zip or deleted; an empty slot takes an
// import (a slot export, or a bare save block with its .header). Drawn in the main page's style and driven
// by keyboard, gamepad, mouse or touch, like the Mods page.

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "save_slots.h"

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
class WindowedAppContext;
}  // namespace rex::ui

namespace nocturne {

class SavesScreen {
 public:
  SavesScreen(rex::ui::ImmediateDrawer* drawer, rex::ui::WindowedAppContext* context,
              std::filesystem::path user_data_root);
  ~SavesScreen();

  // Rereads the slots on the next frame.
  void Reopen(bool gamepad);

  // The cursor is on the intro's music toggle or language picker, which the intro draws.
  bool MusicFocused() const { return focus_.zone == Zone::kMusic; }
  bool PickerFocused() const { return focus_.zone == Zone::kPicker; }
  void FocusPicker() { focus_ = {Zone::kPicker, 0}; }
  void FocusMusic() { focus_ = {Zone::kMusic, 0}; }
  void ReturnFocus() { focus_ = before_bottom_; }

  // Draws into the current window and returns whether Back was chosen. `ready`
  // is false while the screen fades in or a native dialog is open.
  bool Draw(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha, bool ready);

 private:
  // A slot's row, then the buttons beside the focused one.
  enum class Zone { kBack, kSlot, kExport, kDelete, kImport, kMusic, kPicker };
  struct Focus {
    Zone zone = Zone::kSlot;
    int index = 0;
    bool operator==(const Focus&) const = default;
  };

  void Reload();
  bool Used(int slot) const { return slots_[size_t(slot)].used; }
  void Move(int dx, int dy);
  void Press(const Focus& focus);
  void Export(int slot);
  void Import(int slot);
  void Delete();
  void CancelDelete();
  void Input();
  void ConfirmInput();

  bool Hit(ImVec2 min, ImVec2 max, Focus self);
  void Plate(ImVec2 min, ImVec2 max, bool focused, float a);
  void DrawSlot(int slot, ImVec2 min, ImVec2 max);
  void DrawSideButton(ImVec2 min, ImVec2 max, Focus self);
  void DrawConfirm();
  void DrawHints(float y);

  rex::ui::ImmediateDrawer* drawer_;
  rex::ui::WindowedAppContext* context_;
  std::filesystem::path root_;
  // Callbacks from a file dialog check it, since the page can close first.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  bool loaded_ = false;
  std::vector<SaveSlotInfo> slots_;
  // Each slot's icon frames.
  std::array<std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>, kSaveSlotCount> thumbnails_;

  // Per frame.
  ImDrawList* draw_ = nullptr;
  ImFont* font_ = nullptr;
  ImVec2 size_{0, 0};
  float unit_ = 1.0f;
  float alpha_ = 1.0f;
  bool ready_ = false;
  bool moved_ = false;

  Focus focus_;
  // Where Up from the music toggle returns to.
  Focus before_bottom_;
  bool back_ = false;
  bool gamepad_ = false;
  // The last pointer input was a finger, which has no hover.
  bool touch_ = false;
  // A file dialog is open.
  bool waiting_ = false;

  // A delete waiting for the player to agree, asked twice; -1 for none.
  int deleting_ = -1;
  int confirm_step_ = 0;
  bool confirm_on_delete_ = false;

  std::string status_;
  bool status_error_ = false;
};

}  // namespace nocturne
