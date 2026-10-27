// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <memory>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/platform.h>
#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
#include <rex/discord_rpc.h>
#endif
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
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
#include "jp_voices.h"
#include "fonts.generated.h"
#include "game_data.h"
#include "graphics_settings.h"
#ifdef NOCTURNE_EMBED_GUEST_IMAGE
#include "guest_image.h"
#endif
#include "host_menu.h"
#include "language_switch.h"
#include "movie_player.h"
#include "native_options.h"
#include "host_timer_resolution.h"
#include "icon.generated.h"
#include "intro_screen.h"
#include "intro_text.h"
#include "native_renderer.h"
#include "native_renderer_overlay.h"
#include "native_renderer_plume.h"
#include "player_inspector.h"
#include "player_status.h"
#include "rando_menu.h"
#include "rando_runtime.h"
#include "room_presence.h"
#include "save_slots.h"
#include "settings.h"
#include "target.h"
#include "touch_controls_overlay.h"
#include "touch_layout.h"
#include "ui_font.h"
#include "ui_text.h"
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
    // the SDK. {platform} is the SDK's own naming (win-amd64 etc.), which is
    // what the CI matrix publishes.
    config.update_asset_format = "nocturnerecomp-{tag}-{platform}";
    config.update_repo = "birabittoh/NocturneRecomp";
    // Called after FinishGameDataPaths has latched the mounted version.
    config.game_targets = nocturne::GameTargets;

    // One-shot toast shown top-left as the game starts
    config.startup_hint = "Press F4 to open settings.";

    // This project renders the guest itself (src/gpu), so the SDK stays
    // headless. An older config file can still name a plugin; clearing it here,
    // the last point before ReXApp decides whether to call LoadGpuPlugin, is
    // what stops the SDK loading one on top of the native renderer.
    config.graphics = nullptr;
    config.gpu_plugin.clear();
  }

#ifdef NOCTURNE_EMBED_GUEST_IMAGE
  // The linked-in image is World's; JP runs on it too, with its own data.
  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = nocturne::MountGuestImage(runtime()->file_system());
  }
#endif

  // The module is mapped and every function has its default dispatcher
  // entry, but no guest code has run: the point to restore the active
  // randomizer seed, so a randomized save loads into its own world. See
  // src/rando_runtime.h.
  void OnPostLoadXexImage() override {
    nocturne::GetRandoRuntime().Bind(runtime(), user_data_root());
    // The start screen animates the save icons from the image's set.
    nocturne::CacheSaveIcons(user_data_root(), [](uint32_t address) {
      return REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint8_t*>(address);
    });

    // Runtime (and therefore mod_registry()) exists now but mod plugins
    // haven't loaded yet -- ConstructRuntime calls OnPostLoadXexImage, then
    // loads mod plugins and dispatches their OnCreateDialogs afterward (see
    // rex_app.cpp). This is the last point to subscribe before a mod's
    // OnCreateDialogs might publish a "settings.language_option" or
    // "settings.native_string" or "settings.ui_string" event. See settings.h.
    nocturne::RegisterLanguageOptionsListener(runtime()->mod_registry());
    nocturne::RegisterNativeStringListener(runtime()->mod_registry());
    nocturne::RegisterUiStringListener(runtime()->mod_registry());
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
    return true;
  }

  // Game data is resolved here rather than in SetupEnvironment because the
  // window exists by now, so the start screen can draw: it lists the installed
  // versions, extracts new ones from an XBLA package, and starts them.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    // Before the start screen, which can sit open for a long while.
    window()->SetIcon(nocturne::kIconPNG, nocturne::kIconPNGSize);
    // The SDK titles the window after the app identifier ("nocturnerecomp",
    // lowercase, since that name also drives the config file and user data
    // dir and can't be freely changed). Override the display-only title here.
    window()->SetTitle("NocturneRecomp " + std::string(REXGLUE_BUILD_TITLE));
    // The overlays record into the host frame, so the renderer needs the
    // drawer that produces them, the start screen's included.
    nocturne::PlumeSetOverlayDrawer(imgui_drawer());
    nocturne::InitNativeRenderer(window());
    nocturne::BindIntroScreen(imgui_drawer(), &app_context(), user_data_root());
    nocturne::BindWindowTarget(window(), user_settings_path());

    nocturne::GameDataOptions options;
    options.config_path = config_path();
    options.start_screen_flag = nocturne::StartScreenFlagPath(user_settings_path());
    options.user_settings_path = user_settings_path();
    options.game_data_root = defaults.game_data_root;
    nocturne::BindIntroLanguageConfig(user_settings_path());
    if (nocturne::UsePreparedGameData(options))
      return FinishGameDataPaths();

    // Asynchronous, so the main loop runs and feeds the start screen input.
    nocturne::AskForGameData(options, app_context(), [this, resume](bool ready) {
      if (!ready) {
        // Teardown takes a moment; nothing is left to show meanwhile.
        window()->Hide();
        app_context().QuitFromUIThread();
        return;
      }
      resume(FinishGameDataPaths());
    });
    return std::nullopt;
  }

  // The runtime mounts one version folder (World's, unless JP is the only
  // one); JP's is read beside it for what only it has. game_data_root stays
  // the library root in the config, since the settings save every cvar to it.
  rex::PathConfig FinishGameDataPaths() {
    nocturne::ReleaseIntroScreen();
    RefreshPathDefaultsIfCvarsChanged();
    rex::PathConfig paths = resolved_path_defaults();
    const nocturne::GameDirectories& dirs = nocturne::StartedGameDirectories();
    paths.game_data_root = dirs.mounted;
    nocturne::DetectTarget(paths.game_data_root, dirs.jp);
    return paths;
  }

  // Closing from the start screen quits through it; while game files are
  // extracted, closing waits for the extraction to finish.
  bool OnWindowCloseRequested() override {
    if (nocturne::IsExtractingGameData()) {
      REXLOG_INFO("Close requested while installing game files; ignoring until it finishes");
      return false;
    }
    if (nocturne::RequestIntroQuit())
      return false;
    return true;
  }

  std::unique_ptr<rex::ui::ImGuiDialog> OnCreateUserSettingsOverlay() override {
    // No runtime yet on the start screen, so no input system to rebind with.
    auto* input = runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system())
                            : nullptr;
    return nocturne::CreateSettingsDialog(imgui_drawer(), window(), user_settings_path(),
                                          config_path(), input);
  }

  void OnPostSetup() override {
    // Runs the settings overlay's Vulkan device enumeration now,
    // rather than paying for it on the player's first F4 press (the SDK
    // destroys and recreates the overlay on every close/open, so without this
    // it re-ran on every single open). See PrewarmSettingsDialogCaches.
    nocturne::PrewarmSettingsDialogCaches();

    // Bridge the guest "Achievements" pause-menu entry (XamShowAchievementsUI,
    // intercepted in achievements_menu.cpp) to the SDK's built-in achievements
    // overlay: guest A opens it (and pauses the game), controller B closes it.
    // Wired here because window(), app_context() and the input system are all
    // live after setup.
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    nocturne::Achievements().Bind(window(), &app_context(), input_sys);

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

    // Typed player-status API (relics, items, equipment) and its debug inspector
    // (I). See src/player_status.h.
    nocturne::player::GetPlayerStatus().Bind(runtime());
    nocturne::GetPlayerInspector().Bind(runtime());

    // Formerly the fast_forward mod; moved in-app (see fast_forward.h) since
    // it's a base feature, not optional content.
    nocturne::GetFastForward().Bind(window(), input_sys, runtime());

    // Formerly the resolution_preset_native mod; moved in-app (see
    // graphics_settings.h) since it's a base feature, not optional content.
    nocturne::GetGraphicsSettings().Bind(runtime());

    // Adds VSync, Fullscreen and Language rows to the game's own settings screen, alongside its
    // stock Graphics/Volume/Change Screen Size rows (see native_options.h).
    nocturne::GetNativeOptions().Bind(runtime());

    // Lets a user_language change apply on the next frame instead of on the
    // next launch (see language_switch.h).
    nocturne::BindLanguageSwitch(runtime());

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

    // With World mounted, play JP's voice banks unless English ones are
    // forced (see jp_voices.h).
    nocturne::InstallJpVoices(runtime());

    // The constructor is what registers the Android back button bind.
    host_menu_ = std::make_unique<nocturne::HostMenu>(window());

    // On-screen pad. Registered on every platform; the touch_controls cvar (on
    // by default only on Android) decides whether it is shown.
    if (input_sys) {
      auto touch = std::make_unique<nocturne::touch::TouchInputDriver>(nullptr, 0);
      touch->Setup();
      touch->SetLayoutProvider(&nocturne::BuildTouchLayout);
      touch_controls_overlay_ = std::make_unique<nocturne::touch::TouchControlsOverlay>(
          imgui_drawer(), touch.get(), input_sys);
      input_sys->AddDriver(std::move(touch));
    }

    // The JP release's story movies, drawn over the game (see movie_player.h).
    nocturne::BindMovies(runtime());
    movie_overlay_ = nocturne::CreateMovieOverlay(imgui_drawer());

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
    nocturne::GetPlayerInspector().Attach(drawer);
    nocturne::GetRandoMenu().Attach(drawer, user_data_root());
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
    // The touch pad's button letters: sans serif, like the controller's own.
    // Found by name (see touch_controls_overlay.cpp's FaceButtonFont).
    {
      static const ImWchar kFaceButtonGlyphs[] = {'0', '9', 'A', 'Z', 0};
      ImFontConfig face_cfg = cfg;
      face_cfg.OversampleH = face_cfg.OversampleV = 2;
      std::strncpy(face_cfg.Name, "Touch controls", sizeof(face_cfg.Name));
      atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(nocturne::kRobotoMediumTTF),
                                  static_cast<int>(nocturne::kRobotoMediumTTFSize), 96.0f,
                                  &face_cfg, kFaceButtonGlyphs);
    }
    // The start screen's font: large enough to stay sharp when the screen
    // scales it down (the atlas is static, so scaling up would blur), and only
    // the characters its text uses, so the Japanese glyphs stay a handful.
    {
      static ImVector<ImWchar> intro_glyphs;
      if (intro_glyphs.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(atlas->GetGlyphRangesDefault());
        builder.AddText(nocturne::IntroGlyphText().c_str());
        builder.BuildRanges(&intro_glyphs);
      }
      static const ImWchar kLatin[] = {0x0020, 0x00FF, 0};
      ImFontConfig intro_cfg = cfg;
      std::strncpy(intro_cfg.Name, nocturne::kIntroFontName, sizeof(intro_cfg.Name));
      atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(nocturne::kPTSerifRegularTTF),
                                  static_cast<int>(nocturne::kPTSerifRegularTTFSize), 64.0f,
                                  &intro_cfg, kLatin);
      ImFontConfig merge = cfg;
      merge.MergeMode = true;
      atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(nocturne::kDotGothic16SubsetTTF),
                                  static_cast<int>(nocturne::kDotGothic16SubsetTTFSize), 64.0f,
                                  &merge, intro_glyphs.Data);
    }
    // The Japanese overlays: PT Serif has no kana. Added before it so PT Serif
    // stays the default; the overlays switch to this one with UiFontScope.
    {
      ImFontConfig ja_cfg = cfg;
      static const ImWchar kRanges[] = {0x0020, 0xFFEF, 0};
      ja_cfg.GlyphRanges = kRanges;
      nocturne::SetJapaneseFont(atlas->AddFontFromMemoryTTF(
          const_cast<unsigned char*>(nocturne::kDotGothic16SubsetTTF),
          static_cast<int>(nocturne::kDotGothic16SubsetTTFSize), 16.0f, &ja_cfg));
    }
    atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(nocturne::kPTSerifRegularTTF),
                                static_cast<int>(nocturne::kPTSerifRegularTTFSize), 16.0f, &cfg);
  }

  // The overlay's whole color palette is mathematically derived (see
  // rex::ui::ApplyAccentTheme) from this single accent. Fallback used until
  // nocturne::AccentColor can read the player's own in-game accent color
  // setting (R/G/B, 0-15 each) from save data -- KernelState doesn't exist yet
  // at this point, so this matches the game's own shipped default (0, 0, 8).
  static constexpr ImVec4 kDefaultAccentColor = ImVec4(0.00f, 0.00f, 8.0f / 15.0f, 1.00f);

  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override {
    (void)ui_style;
    rex::ui::ApplyAccentTheme(imgui_style, kDefaultAccentColor);
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
    // Normally up since OnFinalizePaths; idempotent.
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

#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
    rex::discord_rpc::Stop();
#endif
    nocturne::ReleaseHostTimerResolution();
  }

 private:
  std::atomic<uint64_t> guest_frame_count_{0};
  std::chrono::steady_clock::time_point fps_poll_time_ = std::chrono::steady_clock::now();
  uint64_t fps_poll_frame_count_ = 0;
  double fps_smoothed_ = 0.0;
  // Back-button menu on Android. See host_menu.h.
  std::unique_ptr<nocturne::HostMenu> host_menu_;
  // Draws the on-screen pad; the driver it reads is owned by the input system.
  std::unique_ptr<rex::ui::ImGuiDialog> touch_controls_overlay_;
  std::unique_ptr<rex::ui::ImGuiDialog> movie_overlay_;

  std::filesystem::path user_settings_path() const { return user_data_root() / "settings.toml"; }
};
