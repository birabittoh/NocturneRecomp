#pragma once

// The start screen's Mods page: what the SDK mod manager overlay offers
// (Installed with Available and Enabled panes, All for the catalog), drawn in
// the main page's style and driven by keyboard, gamepad, mouse or touch. It runs
// before a Runtime exists, so mods are read straight from the mods folder.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include <rex/net/http.h>
#include <rex/system/mod_catalog.h>
#include <rex/system/mod_state.h>

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
class WindowedAppContext;
}  // namespace rex::ui

namespace nocturne {

class ModsScreen {
 public:
  ModsScreen(rex::ui::ImmediateDrawer* drawer, rex::ui::WindowedAppContext* context);
  ~ModsScreen();

  // Rereads the mods folder and the catalog on the next frame.
  void Reopen(bool gamepad) {
    loaded_ = false;
    status_.clear();
    gamepad_ = gamepad;
    focus_ = {};
  }

  // Nothing is loaded yet, so leaving the page makes marked removals and
  // staged updates real right away.
  void Commit();

  // The first error among the enabled mods (unmet requirement, game too old),
  // or empty when the game can start.
  static std::string BlockingIssue();

  // The cursor is on the intro's music toggle, which the intro draws.
  bool MusicFocused() const { return focus_.zone == Zone::kMusic; }
  // Likewise the language picker.
  bool PickerFocused() const { return focus_.zone == Zone::kPicker; }
  void FocusPicker() { focus_ = {Zone::kPicker, 0}; }
  void FocusMusic() { focus_ = {Zone::kMusic, 0}; }
  void ReturnFocus() { LeaveBottom(); }

  // Draws into the current window and returns whether Back was chosen. `ready`
  // is false while the screen fades in or a native dialog is open.
  bool Draw(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha, bool ready);

 private:
  using Clock = std::chrono::steady_clock;
  enum class CatalogLoad { kIdle, kLoading, kReady, kFailed };
  enum class Zone { kBack, kTab, kTool, kAvailable, kEnabled, kCatalog, kMusic, kPicker };
  struct Focus {
    Zone zone = Zone::kEnabled;
    int index = 0;
    bool operator==(const Focus&) const = default;
  };
  enum class ToolId { kAutoSort, kRefreshDisk, kOpenFolder, kInstallFile, kUpdate, kRefreshCatalog };
  struct Tool {
    ToolId id;
    std::string label;
    bool enabled = true;
  };
  // Which part of a row a finger landed on.
  enum class Part { kBody, kTrash, kAction };
  // One finger from press to release: a tap acts on release, a swipe scrolls
  // the list it started in, a hold on an Enabled mod grabs it for dragging.
  struct Gesture {
    bool down = false;
    bool has_target = false;
    Focus target;
    Part part = Part::kBody;
    int list = -1;
    ImVec2 start{0, 0};
    float last_y = 0.0f;
    float scroll_start = 0.0f;
    float velocity = 0.0f;
    Clock::time_point since;
    bool dragging = false;
    bool held = false;
  };
  struct ListGeometry {
    float top = 0.0f;
    float bottom = 0.0f;
    float pitch = 1.0f;
  };

  void Reload();
  void Persist();
  void StartCatalogFetch();
  CatalogLoad CatalogStatus() const;

  // Model.
  void BuildLists();
  void BuildTools();
  int Count(Zone zone) const;
  bool Valid(const Focus& focus) const;
  void Settle();
  int TabCount() const;
  const std::string& IdAt(const Focus& focus) const;
  void FocusMod(const std::string& id);
  void InstallFromFile();
  const rex::system::ModInfo& Info(const std::string& id) const;
  const rex::system::CatalogMod* Listed(const std::string& id) const;
  bool UpdateAvailable(const std::string& id) const;
  std::string Incompatible(const rex::system::CatalogMod& mod) const;

  // Actions.
  void Move(int dx, int dy);
  void LeaveBottom();
  void Press();
  void ToggleMod(const std::string& id);
  void ToggleRemoval(const std::string& id);
  void MoveEnabled(const std::string& id, int delta);
  void ActivateTool(int index);
  void Input();
  void TouchBegin();
  void TouchUpdate();
  void TouchTarget(Focus self, Part part);

  // Drawing.
  bool Hit(ImVec2 min, ImVec2 max, Focus self, bool usable);
  bool SideButton(ImVec2 min, ImVec2 max, Focus row, Part part, bool selected, const ImVec4& glow);
  void DrawArrow(ImVec2 min, ImVec2 max, Focus row, bool right);
  void Plate(ImVec2 min, ImVec2 max, bool focused, bool picked, float a);
  void Text(ImVec2 min, ImVec2 max, const char* text, float size, float a, bool focused);
  void DrawTrash(ImVec2 min, ImVec2 max, Focus row, bool restore, bool selected);
  void DrawModRow(Zone zone, int index, ImVec2 min, ImVec2 max);
  void DrawCatalogRow(int index, ImVec2 min, ImVec2 max);
  template <class RowFn>
  void List(Zone zone, int count, ImVec2 min, ImVec2 max, float row_height, RowFn&& row);
  void DrawDetail(ImVec2 min, ImVec2 max);
  void DrawHints();

  rex::ui::ImmediateTexture* LocalIcon(const rex::system::ModInfo& mod);
  rex::ui::ImmediateTexture* RemoteIcon(const std::string& url);

  rex::ui::ImmediateDrawer* drawer_;
  rex::ui::WindowedAppContext* context_;
  bool loaded_ = false;
  std::filesystem::path mods_root_;
  std::vector<rex::system::ModStateEntry> entries_;
  std::unordered_map<std::string, rex::system::ModInfo> manifests_;
  // The `target` list of each mod made for another game version.
  std::unordered_map<std::string, std::string> other_target_;
  std::unordered_set<std::string> pending_removals_;
  std::vector<rex::system::ModIssue> issues_;

  // Per frame.
  ImDrawList* draw_ = nullptr;
  ImFont* font_ = nullptr;
  ImVec2 size_{0, 0};
  float unit_ = 1.0f;
  float alpha_ = 1.0f;
  bool ready_ = false;
  bool moved_ = false;
  std::vector<size_t> available_;
  std::vector<size_t> enabled_;
  std::vector<rex::system::CatalogMod> view_;
  std::vector<Tool> tools_;

  // Input state.
  int tab_ = 0;
  Focus focus_;
  // The cursor is on the trash can beside the focused mod.
  bool trash_ = false;
  // Where Up from the music toggle returns to.
  Focus before_music_;
  float hint_y_ = 0.0f;
  Focus shown_focus_{Zone::kBack, -1};
  Clock::time_point focus_since_ = Clock::now();
  bool focus_changed_ = false;
  // The installed mod last focused; the toolbar acts on it.
  std::string current_;
  // The mod being reordered, if any.
  std::string grabbed_;
  bool press_ = false;
  bool remove_ = false;
  bool back_ = false;
  bool gamepad_ = false;
  // The last pointer input was a finger, which has no hover.
  bool touch_ = false;
  Gesture gesture_;
  float scroll_[3] = {0, 0, 0};
  float fling_[3] = {0, 0, 0};
  ListGeometry geometry_[3];
  bool was_installing_ = false;

  // The file window is open, and the outcome of the last sideload.
  bool picking_ = false;
  std::string status_;
  bool status_error_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  rex::system::ModCatalog installer_;
  rex::net::CancelToken cancel_;

  std::unordered_map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> icons_;
  std::unordered_map<std::string, std::vector<uint8_t>> icon_bytes_;
  std::unordered_map<std::string, std::thread> icon_threads_;
  std::mutex icon_mutex_;
};

}  // namespace nocturne
