// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/imgui_theme.h>

#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include "accent_color.h"
#include "achievements_menu.h"
#include "fonts.generated.h"
#include "icon.generated.h"
#include "version.generated.h"

#include <rex/system/kernel_state.h>

class NocturnerecompApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NocturnerecompApp>(new NocturnerecompApp(ctx, "nocturnerecomp",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Lets a mod.toml pin a minimum build via `game_version = ">= x.y.z"`,
    // validated at Setup() alongside `requires`/`conflicts` -- see
    // docs/making-mods.md. Derived from the nearest git tag at configure
    // time (src/version.generated.h, see CMakeLists.txt).
    config.game_version = nocturne::kVersionString;

#ifdef _WIN32
    timeBeginPeriod(1);
#endif
  }

  void OnPostSetup() override {
    // Bridge the guest "Achievements" pause-menu entry (XamShowAchievementsUI,
    // intercepted in achievements_menu.cpp) to the SDK's built-in achievements
    // overlay: guest A opens it (and pauses the game), controller B closes it.
    // Wired here because window(), app_context() and the input system are all
    // live after setup.
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    nocturne::Achievements().Bind(window(), &app_context(), input_sys);

    window()->SetIcon(nocturne::kIconPNG, nocturne::kIconPNGSize);

    // Movement/aim in mnk_mode is driven by the D-pad and mouse keybinds, not
    // the left/right analog stick.
    for (const char* stick_cvar : {"keybind_lstick_up", "keybind_lstick_down",
                                    "keybind_lstick_left", "keybind_lstick_right",
                                    "keybind_lstick_press", "keybind_rstick_press"}) {
      rex::cvar::SetFlagByName(stick_cvar, "");
    }

    auto* ks = rex::system::kernel_state();
    nocturne::GetAccentColor().Bind(ks, user_data_root());

    // Feed the F3 debug overlay's "Guest FPS" readout. RegisterTick fires once
    // per guest frame on GPU swap (command-processor thread); the counter is
    // polled from the UI thread each time the overlay redraws to derive an
    // instantaneous FPS, smoothed by only recomputing every ~0.2s.
    runtime()->mod_registry()->RegisterTick(
        [this] { guest_frame_count_.fetch_add(1, std::memory_order_relaxed); });
    SetGuestFrameStats([this]() -> rex::ui::FrameStats {
      rex::ui::FrameStats stats;
      uint64_t count = guest_frame_count_.load(std::memory_order_relaxed);
      stats.frame_count = count;

      auto now = std::chrono::steady_clock::now();
      double elapsed = std::chrono::duration<double>(now - fps_poll_time_).count();
      if (elapsed >= 0.2) {
        fps_smoothed_ = static_cast<double>(count - fps_poll_frame_count_) / elapsed;
        fps_poll_time_ = now;
        fps_poll_frame_count_ = count;
      }
      stats.fps = fps_smoothed_;
      stats.frame_time_ms = fps_smoothed_ > 0.0 ? 1000.0 / fps_smoothed_ : 0.0;
      return stats;
    });

    // Keep guest input "active" while our achievements overlay is open so the
    // B-watcher / left-stick reads see the real controller regardless of mouse
    // position (the SDK otherwise zeroes input reads when the mouse captures an
    // overlay). The guest itself stays locked via the input hooks in
    // achievements_menu.cpp; outside our overlay, fall back to the SDK's
    // mouse-capture gating so its own overlays behave as before.
    if (input_sys) {
      input_sys->SetActiveCallback([this]() {
        if (nocturne::Achievements().ShouldSuppressGuestInput()) {
          return true;
        }
        auto* drawer = imgui_drawer();
        return !drawer || !drawer->GetIO().WantCaptureMouse;
      });
    }
  }

  // Register the per-frame achievements input watcher (closes the overlay on
  // controller B). The ImGui drawer is live here; the input system is supplied
  // later in OnPostSetup. See achievements_menu.cpp.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    nocturne::Achievements().AttachWatcher(drawer);
    nocturne::GetAccentColor().AttachWatcher(drawer);
  }

  // Replace the SDK's default pixel font with a serif face that fits the
  // game's gothic aesthetic.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(nocturne::kPTSerifRegularTTF),
                                static_cast<int>(nocturne::kPTSerifRegularTTFSize), 16.0f, &cfg);
  }

  // The overlay's whole color palette is mathematically derived (see
  // rex::ui::ApplyAccentTheme) from this single accent. Fallback used until
  // nocturne::AccentColor can read the player's own in-game accent color
  // setting (R/G/B, 0-15 each) from save data -- KernelState doesn't exist yet
  // at this point, so this matches the game's own shipped default (0, 0, 8).
  static constexpr ImVec4 kDefaultAccentColor = ImVec4(0.00f, 0.00f, 8.0f / 15.0f, 1.00f);

  void OnConfigureStyle(ImGuiStyle& style) override {
    rex::ui::ApplyAccentTheme(style, kDefaultAccentColor);
  }

  void OnShutdown() override {
#ifdef _WIN32
    timeEndPeriod(1);
#endif
  }

 private:
  std::atomic<uint64_t> guest_frame_count_{0};
  std::chrono::steady_clock::time_point fps_poll_time_ = std::chrono::steady_clock::now();
  uint64_t fps_poll_frame_count_ = 0;
  double fps_smoothed_ = 0.0;
};
