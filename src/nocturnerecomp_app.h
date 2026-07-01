// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <imgui.h>

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
#include "audio_player.h"
#include "fonts.generated.h"

#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include "nocturnerecomp_fp_guard.h"

class NocturnerecompApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NocturnerecompApp>(new NocturnerecompApp(ctx, "nocturnerecomp",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& /*config*/) override {
#ifdef _WIN32
    timeBeginPeriod(1);
    veh_handle_ = InstallGuestFpExceptionHandlerWin();
#endif
  }

  void OnPostSetup() override {
#ifndef _WIN32
    // Install after SDK setup so we override any SDK-installed SIGFPE handler.
    veh_handle_ = InstallGuestFpExceptionHandlerPosix();
#endif

    // Bridge the guest "Achievements" pause-menu entry (XamShowAchievementsUI,
    // intercepted in achievements_menu.cpp) to the SDK's built-in achievements
    // overlay: guest A opens it (and pauses the game), controller B closes it.
    // Wired here because window(), app_context() and the input system are all
    // live after setup.
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    nocturne::Achievements().Bind(window(), &app_context(), input_sys);
    nocturne::GetAudioPlayer().Bind(window(), &app_context());

    auto* ks = rex::system::kernel_state();
    nocturne::GetAccentColor().Bind(ks, user_data_root());
    if (ks && ks->app_manager()) {
      auto* xmp = static_cast<rex::kernel::xam::apps::XmpApp*>(
          ks->app_manager()->FindApp(0xFA));
      if (xmp) {
        xmp->ScanFilesystem();
      }
    }

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
    nocturne::GetAudioPlayer().AttachDialog(drawer);
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
    RemoveGuestFpExceptionHandler(veh_handle_);
    veh_handle_ = nullptr;
#ifdef _WIN32
    timeEndPeriod(1);
#endif
  }

 private:
  void* veh_handle_ = nullptr;
};
