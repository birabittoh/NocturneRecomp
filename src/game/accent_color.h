// nocturnerecomp - mirror the game's own accent color setting (Settings menu,
// R/G/B each 0-15) onto the ImGui overlay theme.
//
// Reads the setting straight out of live guest memory every frame, so overlay
// theming follows the sliders in real time -- including before the player
// saves. See accent_color.cpp for how the guest address was found and why
// it's safe to hardcode.
//
// The live struct sits in static XEX data and reads as all-zero until the
// game actually populates it (no save loaded yet this session -- (0,0,0)
// isn't a real accent value since the shipped default is (0,0,8)). Until
// then we fall back to reading the most recently written save file, same as
// the very first implementation of this feature, so the overlay doesn't
// wash out to near-white (zero-saturation black shades toward white -- see
// rex::ui::ShadeAccent) while sitting at the main menu.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <imgui.h>

namespace rex {
namespace ui {
class ImGuiDrawer;
}  // namespace ui
namespace system {
class KernelState;
}  // namespace system
}  // namespace rex

namespace nocturne {

class AccentColorWatcher;

class AccentColor {
 public:
  AccentColor();
  ~AccentColor();

  // Call once KernelState exists (OnPostSetup) so guest memory and save
  // paths can be resolved.
  void Bind(rex::system::KernelState* kernel_state, std::filesystem::path user_data_root);

  // Registers the per-frame poller with the ImGui drawer (OnCreateDialogs).
  void AttachWatcher(rex::ui::ImGuiDrawer* drawer);

  // Re-reads the accent color setting, if it changed, and applies the
  // resulting theme. Prefers live guest memory; falls back to the newest
  // save file while the live struct isn't populated yet. Safe to call
  // before Bind() (no-op until bound).
  void RefreshIfChanged();

 private:
  void RefreshFromSaveFile();

  rex::system::KernelState* kernel_state_ = nullptr;
  std::filesystem::path user_data_root_;

  uint32_t last_r_ = 0;
  uint32_t last_g_ = 0;
  uint32_t last_b_ = 0;
  bool has_read_once_ = false;

  std::filesystem::file_time_type last_save_write_time_{};
  bool has_read_save_once_ = false;
  bool has_applied_no_save_fallback_ = false;

  std::unique_ptr<AccentColorWatcher> watcher_;
};

// Process-wide instance shared between the app hooks.
AccentColor& GetAccentColor();

}  // namespace nocturne
