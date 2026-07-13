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
#include <rex/version.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/imgui_theme.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/window.h>

#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include "accent_color.h"
#include "achievements_menu.h"
#include "fonts.generated.h"
#include "icon.generated.h"
#include "native_command_processor.h"
#include "native_immediate_drawer.h"
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

    // Explicitly go headless (no xenos/graphics backend) rather than relying
    // on gpu_plugin being unset -- this is the documented entry point for
    // bringing your own renderer via OnCreateImmediateDrawer/OnPreLaunchModule.
    config.graphics = nullptr;

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

    // The SDK titles the window after the app identifier ("nocturnerecomp",
    // lowercase, since that name also drives the config file and user data
    // dir and can't be freely changed). Override the display-only title here.
    window()->SetTitle("NocturneRecomp " + std::string(REXGLUE_BUILD_TITLE));

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
  // game's gothic aesthetic. Fonts must be registered here (before the atlas
  // texture is baked and uploaded) -- adding a font afterward, e.g. from a
  // mod at runtime, leaves it without backing GPU texture data and crashes
  // the first time it's used to render.
  //
  // Also registers ImGui's embedded fixed-width font (ProggyClean) right
  // after, purely so mods needing column-aligned text (hex dumps, tables)
  // have a monospace option: look it up via ImGui::GetIO().Fonts->Fonts[1].
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(nocturne::kPTSerifRegularTTF),
                                static_cast<int>(nocturne::kPTSerifRegularTTFSize), 16.0f, &cfg);
    atlas->AddFontDefault();
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

  // Native renderer phase 2: called from SetupPresentation right after the
  // (still device-less) window opens, per the detached-mode contract in
  // rex/rex_app.h. Must return a drawer that tolerates CreateTexture() before
  // any GPU device exists -- the real Vulkan device/swapchain aren't built
  // until OnPreLaunchModule, below.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    auto drawer = std::make_unique<nocturne::NativeImmediateDrawer>();
    native_drawer_ = drawer.get();
    return drawer;
  }

  // Stand up a real Vulkan swapchain on the game window, reusing the same
  // rex::ui::vulkan::VulkanProvider/Presenter classes the xenos Vulkan
  // backend uses -- infrastructure reuse, not a new abstraction. This is the
  // "no GraphicsSystem" path: no CommandProcessor, no guest-GPU emulation,
  // just a presentable surface for phase 3's native command processor (and,
  // in the meantime, the SDK's own ImGui overlays) to draw into.
  void OnPreLaunchModule() override {
    REXGPU_INFO("Native renderer: OnPreLaunchModule entered");
    // VulkanProvider::CreatePresenter must run on the UI thread (see
    // GraphicsSystem::SetupPresentation for the equivalent SDK-mode call);
    // OnPreLaunchModule itself is not guaranteed to be the UI thread.
    window()->app_context().CallInUIThreadSynchronous([this] {
      // with_gpu_emulation=true so VulkanDevice enables the guest-rendering
      // device features -- tessellationShader in particular: the title-screen
      // smoke overlay is a tessellated quad patch (see
      // NativeCommandProcessor::CreateTessellationHostShaders), and without
      // the feature enabled at device creation those draws are skipped.
      vulkan_provider_ = rex::ui::vulkan::VulkanProvider::Create(
          /*with_gpu_emulation=*/true, /*with_presentation=*/true);
      if (!vulkan_provider_) {
        REXGPU_ERROR("Native renderer: failed to create the Vulkan provider");
        return;
      }
      vulkan_presenter_ = vulkan_provider_->CreatePresenter();
      if (!vulkan_presenter_) {
        REXGPU_ERROR("Native renderer: failed to create the Vulkan presenter");
        return;
      }
      REXGPU_INFO("Native renderer: Vulkan provider/presenter created, attaching to window");
      // Attaching the presenter to the window hooks up swapchain creation,
      // resize handling, and per-frame presentation automatically (Window's
      // own paint loop calls presenter->PaintFromUIThread()) -- the same
      // mechanism the xenos backend relies on, just wired manually instead of
      // through GraphicsSystem.
      window()->SetPresenter(vulkan_presenter_.get());

      if (native_drawer_ &&
          native_drawer_->InitializeVulkan(vulkan_provider_.get(), vulkan_presenter_.get())) {
        // The detached-mode SetupOverlays() call constructed imgui_drawer()
        // with a null presenter (no device existed yet), so it was never
        // registered as a UI drawer anywhere; do that now that a real
        // presenter exists so the debug/console/settings overlays paint.
        if (auto* drawer = imgui_drawer()) {
          vulkan_presenter_->AddUIDrawerFromUIThread(drawer, 0);
        }
        REXGPU_INFO("Native renderer: Vulkan swapchain ready");
      } else {
        REXGPU_ERROR("Native renderer: failed to initialize the Vulkan immediate drawer");
        return;
      }

      // Phase 3: hand decoded PM4 packets from the headless ring buffer to a
      // game-specific interpreter, which presents through this same
      // swapchain. Must be set before the guest starts submitting GPU
      // commands, per SetNativeGpuCommandCallback's contract.
      native_command_processor_ = std::make_unique<nocturne::NativeCommandProcessor>(
          vulkan_provider_.get(), vulkan_presenter_.get());
      rex::system::kernel_state()->SetNativeGpuCommandCallback(
          [this](const rex::graphics::PacketInfo& info, const uint8_t* packet_base) {
            native_command_processor_->OnPacket(info, packet_base);
          });

      // This renderer has no GraphicsSystem, so nothing ever calls the normal
      // GraphicsSystem::SetHostSwapCallback -> Runtime -> ModRegistry::
      // DispatchTick chain -- which is what actually invokes the "guest
      // frame" tick OnPostSetup registered above via RegisterTick (and any
      // mod's own RegisterTick calls). Drive it directly from this
      // renderer's own swap point instead, or every per-frame tick (F3's
      // guest FPS included) silently never fires.
      native_command_processor_->SetOnFramePresented(
          [this] { runtime()->mod_registry()->DispatchTick(); });

      // Feed the F2 shader debugger overlay from NativeCommandProcessor's own
      // shader cache -- this renderer has no GraphicsSystem/CommandProcessor,
      // so the overlay's default GraphicsSystem-based data source (see
      // ReXApp::SetupOverlays) always returns an empty snapshot here.
      // binary_replacer is intentionally left unset (falls back to the same
      // no-op GraphicsSystem path) -- see NativeCommandProcessor::
      // GetShaderDetails's doc comment for why (no per-modification
      // translation storage to replace into).
      rex::ReXApp::ShaderDebuggerOverride shader_debugger_override;
      shader_debugger_override.snapshot_provider = [this] {
        return native_command_processor_->GetShaderSnapshot();
      };
      shader_debugger_override.disable_setter = [this](uint64_t hash, bool disabled) {
        native_command_processor_->SetShaderDisabledByHash(hash, disabled);
      };
      shader_debugger_override.details_provider = [this](uint64_t hash) {
        return native_command_processor_->GetShaderDetails(hash);
      };
      shader_debugger_override.profiling_toggle = [this](bool enabled) {
        native_command_processor_->SetShaderProfilingEnabled(enabled);
      };
      shader_debugger_override.profiling_resetter = [this] {
        native_command_processor_->ResetShaderProfiling();
      };
      SetShaderDebuggerOverride(std::move(shader_debugger_override));
    });
  }

 private:
  std::atomic<uint64_t> guest_frame_count_{0};
  std::chrono::steady_clock::time_point fps_poll_time_ = std::chrono::steady_clock::now();
  uint64_t fps_poll_frame_count_ = 0;
  double fps_smoothed_ = 0.0;

  // Owned by ReXApp via immediate_drawer_; this is a non-owning back-pointer
  // used to hand it the Vulkan device once OnPreLaunchModule builds one.
  nocturne::NativeImmediateDrawer* native_drawer_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> vulkan_provider_;
  std::unique_ptr<rex::ui::Presenter> vulkan_presenter_;
  std::unique_ptr<nocturne::NativeCommandProcessor> native_command_processor_;
};
