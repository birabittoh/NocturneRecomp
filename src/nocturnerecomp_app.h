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

#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include "achievements_menu.h"
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
