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
#include <rex/discord_rpc.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/game_data_selector.h>
#include <rex/version.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/imgui_theme.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include "accent_color.h"
#include "achievements_menu.h"
#include "achievements_screen.h"
#include "fast_forward.h"
#include "font_overlay.h"
#include "fonts.generated.h"
#include "graphics_settings.h"
#include "native_options.h"
#include "host_timer_resolution.h"
#include "icon.generated.h"
#include "native_renderer.h"
#include "native_renderer_overlay.h"
#include "native_renderer_plume.h"
#include "rando_xex.h"
#include "room_presence.h"
#include "settings.h"
#include "version.generated.h"

#include <rex/system/kernel_state.h>

// Single on/off vsync toggle, consumed by the renderer (src/gpu, in progress).
REXCVAR_DEFINE_BOOL(vsync, true, "Video", "Enable vsync");

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

    // Identifies this project to the SDK's mod manager overlay ("All" tab)
    // as the goopie.xyz `recompName` to query the public mod catalog for
    // (see docs/MODS_API.md). Empty would disable the tab outright.
    config.catalog_name = "nocturnerecomp";

    // Lets the SDK's AutoUpdater (F1 mod manager overlay) check
    // github.com/birabittoh/NocturneRecomp's Releases for a newer build and
    // offer to download + apply it. "{tag}"/"{platform}" are substituted by
    // the SDK. A NOCTURNE_TU build must only ever be offered another TU
    // build (never a vanilla one, and vice versa) since the two patch
    // different base addresses so the asset name itself encodes which
    // variant this binary is. {platform} is the SDK's own naming (win-amd64
    // etc.), which is what the CI matrix publishes.
#ifdef NOCTURNE_TU
    config.update_asset_format = "nocturnerecomp-tu-{tag}-{platform}";
#else
    config.update_asset_format = "nocturnerecomp-{tag}-{platform}";
#endif
    config.update_repo = "birabittoh/NocturneRecomp";

    // One-shot toast shown top-left as the game starts
    config.startup_hint = "Press F4 to open settings.";

    // This project renders the guest itself (src/gpu), so the SDK stays
    // headless. An older config file can still name a plugin; clearing it here,
    // the last point before ReXApp decides whether to call LoadGpuPlugin, is
    // what stops the SDK loading one on top of the native renderer.
    config.graphics = nullptr;
    config.gpu_plugin.clear();
  }

  // Redirect the boot module to a randomizer-patched xex (rando_xex_name
  // cvar) before the loader maps the image -- see src/rando_xex.h for why
  // this must happen here and not as a post-launch guest-memory patch.
  void OnLoadXexImage(std::string& module_path) override {
    rando_active_ = nocturne::MaybeApplyRandoXex(game_data_root(), module_path,
                                                 &rando_base_xex_, &rando_patched_xex_);
  }

  // The rando image's .text patches don't take effect from the image alone
  // (recompiled code never re-reads .text) -- once the module is loaded and
  // every function has its default dispatcher entry, diff the images and
  // override the patched functions with interpreter thunks. See
  // src/rando_xex.h.
  void OnPostLoadXexImage() override {
    if (rando_active_) {
      nocturne::InstallRandoOverrides(runtime(), rando_base_xex_, rando_patched_xex_);
    }

    // Runtime (and therefore mod_registry()) exists now but mod plugins
    // haven't loaded yet -- ConstructRuntime calls OnPostLoadXexImage, then
    // loads mod plugins and dispatches their OnCreateDialogs afterward (see
    // rex_app.cpp). This is the last point to subscribe before a mod's
    // OnCreateDialogs might publish a "settings.language_option" or
    // "settings.native_string" event. See settings.h.
    nocturne::RegisterLanguageOptionsListener(runtime()->mod_registry());
    nocturne::RegisterNativeStringListener(runtime()->mod_registry());
  }

  bool SetupEnvironment() override {
    // Game defaults for SDK cvars must land before the SDK loads any config
    // file, so a saved/CLI/env override still wins.
    nocturne::ApplySettingDefaults();

    if (!rex::ReXApp::SetupEnvironment())
      return false;

    // User-facing settings (Fullscreen, VSync, ...) live in their own
    // file, separate from the advanced cvars in the app's normal config, and
    // are loaded last so they win over both.
    if (std::filesystem::exists(user_settings_path()))
      rex::cvar::LoadConfig(user_settings_path());

    // The guest is always told 1280x720 (its video mode is what the title lays
    // its UI out for); the host renders at the window's size on top of that, so
    // a "resolution" saved by an older build must not change the video mode.
    rex::cvar::SetFlagByName("resolution", "720p", /*persist=*/false);

    // Both config files are loaded by now, so the cvar holds the user's value.
    // This used to be an unconditional timeBeginPeriod(1) in OnPreSetup; it has
    // to run after the config load to be configurable at all, and the pacer
    // does not sleep until well after this. See host_timer_resolution.h.
    nocturne::ApplyHostTimerResolution();

    rex::system::GameDataSelectorSettings settings;
    settings.default_xex_sha256 = "26a58b074c5dd6185b77a8111a0012866d11cba674b4b0810d79dbf07ad68aa6";
    settings.is_xbla = true;
    settings.config_path = config_path();

#ifdef NOCTURNE_TU
    settings.title_update_sha256 = "226420345845a2077ee362ad68e8d444afce549a5eeb7f8c43aebae232fb4b8c";
#endif

    return rex::system::GameDataSelector::EnsureGameData(settings);
  }

  std::unique_ptr<rex::ui::ImGuiDialog> OnCreateUserSettingsOverlay() override {
    return nocturne::CreateSettingsDialog(
        imgui_drawer(), window(), user_settings_path(), config_path(),
        static_cast<rex::input::InputSystem*>(runtime()->input_system()));
  }

  void OnPostSetup() override {
    // Runs the settings overlay's Vulkan device enumeration now,
    // rather than paying for it on the player's first F4 press (the SDK
    // destroys and recreates the overlay on every close/open, so without this
    // it re-ran on every single open). See PrewarmSettingsDialogCaches.
    nocturne::PrewarmSettingsDialogCaches();
    nocturne::BindWindowTarget(window(), user_settings_path());

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

    // Discord Rich Presence: shows the castle room the player is currently
    // in, updated once per guest frame. See src/room_presence.h/.cpp.
    nocturne::GetRoomPresence().Bind(ks, runtime());

    // Formerly the fast_forward mod; moved in-app (see fast_forward.h) since
    // it's a base feature, not optional content.
    nocturne::GetFastForward().Bind(window(), input_sys, runtime());

    // Formerly the resolution_preset_native mod; moved in-app (see
    // graphics_settings.h) since it's a base feature, not optional content.
    nocturne::GetGraphicsSettings().Bind(runtime());

    // Adds VSync, Fullscreen and Language rows to the game's own settings screen, alongside its
    // stock Graphics/Volume/Change Screen Size rows (see native_options.h).
    nocturne::GetNativeOptions().Bind(runtime());

    // Points the Achievements menu entries at the game's own unused
    // CScreenAchievement front-end page instead of the SDK overlay (see
    // achievements_screen.h). Must come after NativeOptions::Bind: it leans on
    // that file's pre-game watchdog suppression, which is only installed there.
    nocturne::GetAchievementsScreen().Bind(runtime());

    // Drawing the achievements in-game means drawing them with the title's own
    // font, and font_ja.xpr is a subset built only from strings_ja.bin -- it
    // has no glyph for 33 of the characters the Japanese achievement text
    // uses. Compose the player's font with our own supplemental glyph art in
    // memory (see font_overlay.h); their file is never modified. Runs before
    // the guest starts, so it is in place by the title's first font load.
    nocturne::InstallJapaneseFontOverlay(runtime());

    // The renderer loads no GPU plugin, so the SDK's per guest frame callback
    // (which drives the mod registry's tick) never fires; the swap hook does it.
    nocturne::SetGuestFrameCallback([this] { runtime()->mod_registry()->DispatchTick(); });

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
    nocturne::GetFastForward().AttachWatcher(drawer);
  }

  // Replace the SDK's default pixel font with a serif face that fits the
  // game's gothic aesthetic. Fonts must be registered here (before the atlas
  // texture is baked and uploaded) -- adding a font afterward, e.g. from a
  // mod at runtime, leaves it without backing GPU texture data and crashes
  // the first time it's used to render.
  //
  // Also registers ImGui's embedded fixed-width font (ProggyClean) first,
  // purely so mods needing column-aligned text (hex dumps, tables) have a
  // monospace option: look it up via ImGui::GetIO().Fonts->Fonts[0]. It must
  // be added before PT Serif: the SDK sets io.FontDefault to whichever font
  // is added last, and PT Serif is meant to be the UI's default font.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    atlas->AddFontDefault();

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

  // Detached overlay mode: with no GPU plugin the SDK creates no presenter and
  // asks the app for a drawer instead. Returning null would leave the window
  // without F3/F4 and every other overlay. See native_renderer_overlay.h.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return nocturne::CreatePlumeImmediateDrawer();
  }

  // Stand up the Plume device and swap chain on the game window. The window
  // exists by now and the guest has not started, so no D3D call can arrive
  // ahead of the backend. Swap chain creation goes through the UI thread.
  void OnPreLaunchModule() override {
    nocturne::PlumeSetOverlayDrawer(imgui_drawer());
    window()->app_context().CallInUIThreadSynchronous(
        [this] { nocturne::InitNativeRenderer(window()); });
  }

  // The host swap chain follows the window. Only records the request: this
  // arrives on the UI thread, and every other swap chain call happens on the
  // guest thread at present time.
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    nocturne::PlumeNotifyResize(pixel_width, pixel_height);
  }

  void OnShutdown() override {
    // Tear the host renderer down before the window goes away.
    nocturne::ShutdownPlumeBackend();

    rex::discord_rpc::Stop();
    nocturne::ReleaseHostTimerResolution();
  }

 private:
  bool rando_active_ = false;
  std::filesystem::path rando_base_xex_;
  std::filesystem::path rando_patched_xex_;
  std::atomic<uint64_t> guest_frame_count_{0};
  std::chrono::steady_clock::time_point fps_poll_time_ = std::chrono::steady_clock::now();
  uint64_t fps_poll_frame_count_ = 0;
  double fps_smoothed_ = 0.0;

  std::filesystem::path user_settings_path() const { return user_data_root() / "settings.toml"; }
};
