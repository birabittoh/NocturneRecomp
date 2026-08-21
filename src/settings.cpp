// nocturnerecomp - ReXGlue Recompiled Project
// See settings.h for details.

#include "settings.h"

#include "graphics_settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/system/auto_updater.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_widgets.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/window.h>
#include <imgui.h>

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif

namespace nocturne {

namespace {

// Everything build.py used to write into the shipped nocturnerecomp.toml.
// These are the game's defaults, not necessarily the SDK's; kept here so
// there is one place that owns "what NocturneRecomp ships with". Keybind/
// remap/rando_xex_name/danger-zone entries have no row in the curated
// dialog below (left to the SDK's own developer panel) but still need their
// default seeded here, same as build.py used to.
struct DefaultValue {
  const char* cvar;
  const char* value;
};

constexpr std::array kGameDefaults = {
    DefaultValue{"user_name", "Player"},
    DefaultValue{"user_language", "1"},
    DefaultValue{"gpu_plugin", "xenos"},
    DefaultValue{"gpu_backend", "any"},
    DefaultValue{"fullscreen", "false"},
    DefaultValue{"resolution", "720p"},
    DefaultValue{"resolution_scale", "1"},
    DefaultValue{"vulkan_device", "-1"},
    // Matches build.py's copy_post_process_shader destination
    // (<pkg>/shaders/crt.slang); a relative forward-slash path works on
    // both platforms.
    DefaultValue{"post_process_shader_path", "./shaders/crt.slang"},
    DefaultValue{"post_process_shader_enabled", "false"},
    DefaultValue{"frame_pacer_enabled", "false"},
    DefaultValue{"shader_dump_enabled", "false"},
    DefaultValue{"texture_dump_enabled", "false"},
    DefaultValue{"texture_dump_format", "png"},
    DefaultValue{"texture_dump_skip_sizes", "512x256,1024x512,2048x1024,1920x1080,1280x720"},
    DefaultValue{"keybind_a", "Space"},
    DefaultValue{"keybind_b", "RMB"},
    DefaultValue{"keybind_x", "LMB"},
    DefaultValue{"keybind_y", "Shift"},
    DefaultValue{"keybind_left_trigger", "M"},
    DefaultValue{"keybind_right_trigger", "R"},
    DefaultValue{"keybind_left_shoulder", "Q"},
    DefaultValue{"keybind_right_shoulder", "Control"},
    DefaultValue{"keybind_back", "E"},
    DefaultValue{"keybind_start", "Escape"},
    DefaultValue{"keybind_dpad_up", "W"},
    DefaultValue{"keybind_dpad_left", "A"},
    DefaultValue{"keybind_dpad_down", "S"},
    DefaultValue{"keybind_dpad_right", "D"},
    DefaultValue{"bind_fast_forward", "Tab"},
    DefaultValue{"bind_fast_forward_gamepad", "LThumb"},
    DefaultValue{"remap_start", "back"},
    DefaultValue{"remap_back", "start"},
    DefaultValue{"remap_left_trigger", "left_trigger"},
    DefaultValue{"rando_xex_name", "Rando.xex"},
    DefaultValue{"gpu_allow_invalid_fetch_constants", "true"},
    DefaultValue{"game_data_root", "assets"},
    DefaultValue{"update_data_root", "update"},
    DefaultValue{"mods_data_root", "mods"},
    DefaultValue{"license_mask", "1"},
    DefaultValue{"mnk_mode", "true"},
    DefaultValue{"mnk_capture_mouse", "false"},
};

// cvars persisted to the friendly settings.toml by the Basic section.
// gpu_backend/vulkan_device get custom rows (dynamic dropdowns) rather than
// the generic DrawCvarWidget path, but are still listed here so the generic
// Reset-All / restart-tracking loops cover them; GetFlagInfo/ResetToDefault
// etc. no-op harmlessly for "vulkan_device" on a build without Vulkan.
//
// host_timer_resolution_ms is absent here on purpose: 5ms is right for
// everyone, so it gets no curated row. It is listed in kAdvancedCvarNames
// instead, where the generic widget exposes it as the raw millisecond value.
constexpr std::array<const char*, 11> kBasicCvarNames = {
    "fullscreen",           "resolution", "resolution_scale", "user_name",  "user_language",
    "gpu_backend",          "vulkan_device", "gpu_plugin", "audio_mute", "audio_volume",
    "graphics_style_enhanced"};

// The SDK's resolution_scale is an integer EDRAM/draw supersampling factor
// (range 1-8), independent of the display resolution -- it's an absolute
// multiplier on the base render target, not a percentage of "native" for
// whatever Resolution happens to be set to.
constexpr int kMaxResolutionScale = 8;

// Named resolution presets offered by DrawResolutionRow, ordered ascending.
constexpr std::array<const char*, 4> kResolutionPresetsAscending = {"720p", "1080p", "1440p", "4K"};

// Vertical pixel count of each named resolution preset.
int ResolutionHeightFor(const char* option) {
  std::string_view opt(option);
  if (opt == "720p")
    return 720;
  if (opt == "1080p")
    return 1080;
  if (opt == "1440p")
    return 1440;
  if (opt == "4K")
    return 2160;
  return 720;
}

// Number of leading entries of kResolutionPresetsAscending that fit on the
// display the settings window currently resides on -- offering a preset
// taller than the desktop's own resolution just leaves the player with an
// oversized window/fullscreen mode they can't fully see.
// window->GetDesktopDisplayHeight() is routed through the SDK's Window
// abstraction (SDL under the hood on every platform), so this works on
// Windows, X11, and Wayland alike without any platform-specific code here.
// A 0 result (no window yet, or the backend couldn't answer) means "assume
// everything fits" -- fail open rather than hide presets over a query that
// simply isn't available.
int AllowedResolutionCount(rex::ui::Window* window) {
  uint32_t display_height = window ? window->GetDesktopDisplayHeight() : 0;
  if (display_height == 0)
    return static_cast<int>(kResolutionPresetsAscending.size());
  int count = 0;
  for (const char* opt : kResolutionPresetsAscending) {
    if (static_cast<uint32_t>(ResolutionHeightFor(opt)) > display_height)
      break;
    ++count;
  }
  return std::max(count, 1);
}

// audio_volume is stored (and applied to samples by the SDL audio driver) as
// linear amplitude, but human loudness perception is roughly logarithmic --
// a linear slider (amplitude == percent/100) would spend most of its travel
// on barely-perceptible changes near the top end and cram all the audible
// range into the last few percent at the bottom. Map the displayed 0-100%
// through a dB curve instead: -40dB at 0% (quiet enough to treat as silence
// below) up to 0dB (full amplitude) at 100%, evenly spaced in dB rather than
// in amplitude.
// The conversions themselves are declared in settings.h and defined below,
// outside this anonymous namespace, since native_options.cpp shares them.
constexpr double kMinVolumeDb = -40.0;

// XLanguage IDs per the Xbox 360 kernel's user_language cvar; note 10 is
// intentionally absent (not a valid XLanguage).
constexpr std::array kLanguageOptions = {
    LanguageOption{"1", "English"},
    LanguageOption{"2", "Japanese"},
    LanguageOption{"3", "German"},
    LanguageOption{"4", "French"},
    LanguageOption{"5", "Spanish"},
    LanguageOption{"6", "Italian"},
};

// Extra entries appended by mods via the "settings.language_option" event
// (see RegisterLanguageOptionsListener). Owns its own strings, since the
// event's payload.bytes is only valid for the duration of Publish().
struct ModLanguageOption {
  std::string id;
  std::string label;
};
std::vector<ModLanguageOption> g_mod_language_options;

// Extra native-options string translations mods register via the
// "settings.native_string" event (see RegisterNativeStringListener in
// settings.h). Keyed by "<XLanguage id>:<key>"; owns its own UTF-16
// storage, since the event's payload.bytes is only valid for the duration
// of Publish().
std::unordered_map<std::string, std::u16string> g_native_string_translations;

// Decodes UTF-8 to UTF-16 (BMP code points as a single unit, astral ones as
// a surrogate pair). Mod-published label text is expected to be valid
// UTF-8; an invalid lead byte is skipped rather than corrupting the rest of
// the string.
std::u16string Utf8ToUtf16(std::string_view s) {
  std::u16string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const auto byte_at = [&](size_t offset) {
      return static_cast<unsigned char>(s[i + offset]);
    };
    const unsigned char c0 = byte_at(0);
    uint32_t cp;
    size_t len;
    if (c0 < 0x80) {
      cp = c0;
      len = 1;
    } else if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
      cp = ((c0 & 0x1Fu) << 6) | (byte_at(1) & 0x3Fu);
      len = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
      cp = ((c0 & 0x0Fu) << 12) | ((byte_at(1) & 0x3Fu) << 6) | (byte_at(2) & 0x3Fu);
      len = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
      cp = ((c0 & 0x07u) << 18) | ((byte_at(1) & 0x3Fu) << 12) | ((byte_at(2) & 0x3Fu) << 6) |
           (byte_at(3) & 0x3Fu);
      len = 4;
    } else {
      ++i;
      continue;
    }
    if (cp <= 0xFFFF) {
      out.push_back(static_cast<char16_t>(cp));
    } else if (cp <= 0x10FFFF) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FFu)));
    }
    i += len;
  }
  return out;
}

bool IsKnownLanguageId(std::string_view id) {
  for (const auto& opt : kLanguageOptions) {
    if (id == opt.id)
      return true;
  }
  for (const auto& opt : g_mod_language_options) {
    if (id == opt.id)
      return true;
  }
  return false;
}

// cvars rendered generically in the collapsed Advanced section, persisted to
// the app's normal cvar config (nocturnerecomp.toml).
constexpr std::array<const char*, 9> kAdvancedCvarNames = {
    "post_process_shader_enabled",
    "post_process_shader_path",
    "frame_pacer_enabled",
    // Windows only, so absent from the registry elsewhere; DrawAdvancedRow and
    // the generic Reset-All / save loops no-op on a name that is not
    // registered, so listing it unconditionally is safe.
    "host_timer_resolution_ms",
    "shader_dump_enabled",
    "texture_dump_enabled",
    "texture_dump_format",
    "texture_dump_skip_sizes",
    "rando_xex_name",
};

std::vector<std::string> BasicCvarNames() {
  return std::vector<std::string>(kBasicCvarNames.begin(), kBasicCvarNames.end());
}

// Populated once by PrewarmSettingsDialogCaches(); every CuratedSettingsDialog
// instance reads from these rather than re-running the underlying (expensive)
// enumeration itself.
std::vector<std::string> g_gpu_plugin_names;
#if REX_HAS_VULKAN
std::vector<rex::ui::vulkan::DeviceInfo> g_vulkan_devices;
#endif

class CuratedSettingsDialog : public rex::ui::ImGuiDialog {
 public:
  CuratedSettingsDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                        std::filesystem::path user_settings_path,
                        std::filesystem::path app_config_path,
                        rex::input::InputSystem* input_system)
      : rex::ui::ImGuiDialog(drawer),
        window_(window),
        user_settings_path_(std::move(user_settings_path)),
        app_config_path_(std::move(app_config_path)),
        input_system_(input_system) {
    gpu_plugin_names_ = g_gpu_plugin_names;
#if REX_HAS_VULKAN
    vulkan_devices_ = g_vulkan_devices;
#endif
  }

 protected:
  void OnDraw(ImGuiIO& /*io*/) override {
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin("Settings##rex", nullptr,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (AnyPendingRestart()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
      ImGui::TextWrapped("Some changes require a restart to take effect.");
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::SmallButton("Restart Now")) {
        if (rex::platform::process::Relaunch() && window_) {
          window_->RequestClose();
        }
      }
      ImGui::Separator();
    }

    DrawUpdateSection();

    DrawFullscreenRow();
    DrawResolutionRow();
    DrawRenderScaleRow();
    DrawGraphicsStyleRow();
    DrawAudioMuteRow();
    DrawAudioVolumeRow();
    DrawUserNameRow();
    DrawLanguageRow();
    DrawGpuPluginRow();
    // gpu_plugin is kRequiresRestart, so a change made just above hasn't
    // taken effect yet -- this session is still running whatever renderer
    // was active at launch. DrawGpuBackendRow queries the *selected*
    // plugin's supported backends by actually loading its DLL
    // (QuerySupportedBackends), which collides with an already-live native
    // renderer's own Vulkan device/swapchain and crashes if run before the
    // restart. Skip it while gpu_plugin has a pending restart.
    bool gpu_plugin_restart_pending = IsPendingRestart("gpu_plugin");
    if (!gpu_plugin_restart_pending && rex::cvar::GetFlagByName("gpu_plugin") != "") {
      DrawGpuBackendRow();
#if REX_HAS_VULKAN
      if (rex::cvar::GetFlagByName("gpu_backend") == "vulkan") {
        DrawVulkanDeviceRow();
      }
#endif
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Advanced")) {
      for (const char* name : kAdvancedCvarNames) {
        DrawAdvancedRow(name);
      }
    }

    ImGui::Separator();
    if (ImGui::Button("Reset All to Defaults")) {
      for (const char* name : kBasicCvarNames) {
        rex::cvar::ResetToDefault(name);
      }
      for (const char* name : kAdvancedCvarNames) {
        rex::cvar::ResetToDefault(name);
      }
      SaveBasic();
      SaveAdvanced();
    }
    ImGui::SameLine();
    // Opens the SDK's own full cvar browser (the same one bind_settings/F4
    // would show if settings_manager_enabled were false) for anything not
    // surfaced above. It's a separate top-level ImGuiDialog -- constructing
    // it registers it with the drawer (see ImGuiDialog's ctor), so it starts
    // drawing/receiving input immediately, independent of this dialog; this
    // button just toggles that lifetime, mirroring bind_settings's own
    // open/close toggle in rex_app.cpp. Given a distinct window_title
    // ("All Settings##rexdev") so its ImGui window doesn't share an ID with
    // this dialog's own "Settings##rex" -- same ID would merge both
    // dialogs' draws into a single squeezed window instead of two.
    if (ImGui::Button(dev_settings_overlay_ ? "Close All Settings" : "All Settings...")) {
      if (dev_settings_overlay_) {
        dev_settings_overlay_.reset();
      } else {
        // config_path here is where the SDK's dialog writes ("Save to
        // config"), not where it reads from; cvars are already loaded from
        // app_config_path_ at boot (see ReXApp::SetupEnvironment). Pointing
        // saves at user_settings_path_ instead keeps <game>.toml read-only:
        // it can still be hand-edited for dev-only setup, but nothing the
        // running game does ever writes to it.
        dev_settings_overlay_ = std::make_unique<rex::ui::SettingsDialog>(
            imgui_drawer(), user_settings_path_, input_system_, "All Settings##rexdev");
      }
    }

    ImGui::End();
  }

 private:
  // GetPendingRestartFlags() only tracks cvars that were actually changed at
  // runtime (settings UI, console, mods) this session -- values applied
  // while loading a config file at boot don't count, so a saved preference
  // that simply differs from the SDK's factory default (e.g. Resolution set
  // to 1080p) doesn't trip this on a fresh launch. See SetFlagByNameImpl's
  // mark_restart parameter in the SDK's cvar.cpp.
  bool IsPendingRestart(const char* name) {
    auto pending = rex::cvar::GetPendingRestartFlags();
    return std::find(pending.begin(), pending.end(), name) != pending.end();
  }

  bool AnyPendingRestart() {
    auto pending = rex::cvar::GetPendingRestartFlags();
    auto is_tracked = [&pending](const char* name) {
      return std::find(pending.begin(), pending.end(), name) != pending.end();
    };
    for (const char* name : kBasicCvarNames) {
      if (is_tracked(name))
        return true;
    }
    for (const char* name : kAdvancedCvarNames) {
      if (is_tracked(name))
        return true;
    }
    return false;
  }

  void SaveBasic() { rex::cvar::SaveConfigSubset(user_settings_path_, BasicCvarNames()); }
  // Advanced rows used to persist to app_config_path_ (<game>.toml). That
  // file is now read-only from the game's own UI (the "All Settings..."
  // browser saves to settings.toml too, see above); <game>.toml can still be
  // hand-edited for dev-only setup, but nothing here writes to it anymore.
  void SaveAdvanced() {
    std::vector<std::string> names(kAdvancedCvarNames.begin(), kAdvancedCvarNames.end());
    rex::cvar::SaveConfigSubset(user_settings_path_, names);
  }

  // Game self-update (see rex::system::AutoUpdater), surfaced here rather
  // than the SDK's mod manager overlay (F1) since a player who never touches
  // mods should still be told about an available update
  void DrawUpdateSection() {
    if (!update_check_requested_) {
      update_check_requested_ = true;
      auto_updater_.CheckAsync();
    }

    // A previous session already downloaded and staged an update (whether or
    // not this one ever calls CheckAsync/InstallAsync again); offer the
    // restart regardless of auto_updater_'s own in-memory state.
    if (rex::system::AutoUpdater::HasPendingSelfUpdate(rex::filesystem::GetExecutableFolder())) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.55f, 1.0f));
      ImGui::TextWrapped("An update has been downloaded.");
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::SmallButton("Restart & Apply##autoupdate")) {
        // This install root contains the running executable itself,
        // which stays locked for this process's whole lifetime (see
        // AutoUpdater::ApplyAndRestart's contract). The spawned helper
        // outlives this process, applies the swap, and launches the new exe.
        if (rex::system::AutoUpdater::ApplyAndRestart(rex::filesystem::GetExecutableFolder(),
                                                       rex::filesystem::GetExecutablePath()) &&
            window_) {
          window_->RequestClose();
        }
      }
      ImGui::Separator();
      return;
    }

    auto install = auto_updater_.InstallSnapshot();
    if (install.in_progress) {
      if (install.total_bytes > 0) {
        ImGui::TextDisabled("Downloading update... %.0f%%",
                            100.0 * static_cast<double>(install.downloaded_bytes) /
                                static_cast<double>(install.total_bytes));
      } else {
        ImGui::TextDisabled("Downloading update...");
      }
      ImGui::Separator();
      return;
    }
    if (install.done && !install.ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
      ImGui::TextWrapped("%s", install.message.c_str());
      ImGui::PopStyleColor();
      ImGui::Separator();
      return;
    }

    if (auto_updater_.state() != rex::system::UpdateCheckState::kUpdateAvailable) {
      return;  // kIdle/kChecking/kUpToDate/kFailed: nothing worth showing.
    }
    auto info = auto_updater_.Available();
    if (!info) {
      return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.55f, 1.0f));
    ImGui::TextWrapped("Update available: v%s", info->version.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Download Update")) {
      auto_updater_.InstallAsync(*info, rex::filesystem::GetExecutableFolder());
    }
    ImGui::Separator();
  }

  void DrawFullscreenRow() {
    const auto* entry = rex::cvar::GetFlagInfo("fullscreen");
    if (!entry)
      return;
    ImGui::TextUnformatted("Fullscreen");
    ImGui::SameLine(180.0f);
    ImGui::PushID("fullscreen");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawAudioMuteRow() {
    const auto* entry = rex::cvar::GetFlagInfo("audio_mute");
    if (!entry)
      return;
    ImGui::TextUnformatted("Mute Audio");
    ImGui::SameLine(180.0f);
    ImGui::PushID("audio_mute");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  // audio_volume is a Double cvar (0.0-1.0 linear amplitude, applied directly
  // to samples by the SDL audio driver); DrawCvarWidget's generic Double path
  // is a plain InputDouble box, not a slider, so this draws its own row the
  // same way DrawRenderScaleRow does for resolution_scale -- displaying and
  // editing a perceptually-spaced percentage (see VolumeAmplitudeFromPercent)
  // rather than the raw amplitude directly.
  void DrawAudioVolumeRow() {
    const auto* entry = rex::cvar::GetFlagInfo("audio_volume");
    if (!entry)
      return;
    const auto* mute_entry = rex::cvar::GetFlagInfo("audio_mute");
    if (mute_entry && mute_entry->getter() == "true")
      return;

    int percent = VolumePercentFromAmplitude(std::atof(entry->getter().c_str()));

    ImGui::TextUnformatted("Volume");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("audio_volume");
    bool changed = ImGui::SliderInt("##v", &percent, 0, 100, "%d%%");
    if (changed) {
      rex::cvar::SetFlagByName("audio_volume", std::to_string(VolumeAmplitudeFromPercent(percent)),
                               /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawResolutionRow() {
    const auto* entry = rex::cvar::GetFlagInfo("resolution");
    if (!entry)
      return;
    const auto& kOptions = kResolutionPresetsAscending;
    int count = std::clamp(AllowedResolutionCount(window_), 1, static_cast<int>(kOptions.size()));
    std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < count; ++i) {
      if (current == kOptions[i]) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("Resolution");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("resolution");
    int idx = cur_idx;
    // Format string is a fixed label, not a %d placeholder -- SliderInt just
    // displays it verbatim, same trick DrawRenderScaleRow uses for "50%" etc.
    if (ImGui::SliderInt("##v", &idx, 0, count - 1, kOptions[idx])) {
      rex::cvar::SetFlagByName("resolution", kOptions[idx], /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  // resolution_scale is an integer cvar (range 1-kMaxResolutionScale), a
  // plain EDRAM/draw supersampling factor entirely independent of the
  // Resolution cvar -- it no longer expresses itself as a percentage of the
  // current display resolution.
  void DrawRenderScaleRow() {
    const auto* scale_entry = rex::cvar::GetFlagInfo("resolution_scale");
    if (!scale_entry)
      return;

    int current_scale = std::atoi(scale_entry->getter().c_str());
    int idx = std::clamp(current_scale, 1, kMaxResolutionScale);

    ImGui::PushID("render_scale_percent");

    ImGui::TextUnformatted("Render Scale");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    bool changed = ImGui::SliderInt("##v", &idx, 1, kMaxResolutionScale, "%dx");

    if (changed) {
      rex::cvar::SetFlagByName("resolution_scale", std::to_string(idx), /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawGraphicsStyleRow() {
    const auto* entry = rex::cvar::GetFlagInfo("graphics_style_enhanced");
    if (!entry)
      return;
    ImGui::TextUnformatted("Enhanced Graphics");
    ImGui::SameLine(180.0f);
    ImGui::PushID("graphics_style_enhanced");
    bool enhanced = entry->getter() == "true";
    // rex::ui::ToggleSwitch directly rather than DrawCvarWidget's generic
    // bool path: that one calls rex::cvar::SetFlagByName itself, which --
    // unlike this row -- doesn't apply the change to guest memory or persist
    // it (see RequestGraphicsStyle's doc comment). ToggleSwitch matches the
    // pill-toggle look DrawCvarWidget uses for every other Boolean row
    // (Mute Audio, Fullscreen, ...) without going through its own apply path.
    if (rex::ui::ToggleSwitch("##v", &enhanced)) {
      RequestGraphicsStyle(enhanced);
    }
    ImGui::PopID();
  }

  void DrawUserNameRow() {
    const auto* entry = rex::cvar::GetFlagInfo("user_name");
    if (!entry)
      return;
    ImGui::TextUnformatted("Player Name");
    ImGui::SameLine(180.0f);
    ImGui::PushID("user_name");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawLanguageRow() {
    const auto* entry = rex::cvar::GetFlagInfo("user_language");
    if (!entry)
      return;

    std::vector<LanguageOption> options = GetLanguageOptions();

    std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
      if (current == options[i].id) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("Language");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("user_language");
    if (ImGui::BeginCombo("##v", options[cur_idx].label)) {
      for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(options[i].label, selected)) {
          rex::cvar::SetFlagByName("user_language", options[i].id, /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  // Backend support is a property of the *selected GPU plugin*, not a fixed
  // set every plugin shares -- gpu_backend's own `.allowed(...)` list
  // includes "any" and every backend rex_gpu_create could theoretically
  // accept, regardless of what the active plugin actually implements. This
  // queries rex::system::QuerySupportedBackends(gpu_plugin) instead, caching
  // per plugin name since it loads/unloads the plugin DLL to ask; the row
  // only renders when that plugin actually offers more than one backend --
  // with zero or one, there's no meaningful choice to present.
  void DrawGpuBackendRow() {
    const auto* entry = rex::cvar::GetFlagInfo("gpu_backend");
    const auto* plugin_entry = rex::cvar::GetFlagInfo("gpu_plugin");
    if (!entry || !plugin_entry)
      return;

    std::string plugin_name = plugin_entry->getter();
    if (plugin_name != gpu_backend_query_plugin_) {
      gpu_backend_query_plugin_ = plugin_name;
      gpu_backend_names_ = rex::system::QuerySupportedBackends(plugin_name);
    }
    if (gpu_backend_names_.size() < 2)
      return;

    auto label_for = [](const std::string& id) -> std::string {
      if (id == "d3d12")
        return "D3D12";
      if (id == "vulkan")
        return "Vulkan";
      return id;
    };

    std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(gpu_backend_names_.size()); ++i) {
      if (current == gpu_backend_names_[i]) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("GPU Backend");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("gpu_backend");
    if (ImGui::BeginCombo("##v", label_for(gpu_backend_names_[cur_idx]).c_str())) {
      for (int i = 0; i < static_cast<int>(gpu_backend_names_.size()); ++i) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(label_for(gpu_backend_names_[i]).c_str(), selected)) {
          rex::cvar::SetFlagByName("gpu_backend", gpu_backend_names_[i], /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  // gpu_plugin names a rexgpu-<name>[postfix].dll staged next to the
  // executable; unlike gpu_backend it has no fixed `.allowed(...)` list
  // since the valid set depends on what's actually staged there, so this
  // combo is populated from rex::system::EnumerateGpuPlugins() instead of
  // going through the generic DrawCvarWidget (which would fall back to a
  // plain text field for an unconstrained string cvar). "" is also a valid
  // value here, distinct from anything EnumerateGpuPlugins() returns: it
  // selects this game's own native renderer (see OnPreLaunchModule) instead
  // of routing through a gpu_plugin DLL, so it gets its own leading entry.
  void DrawGpuPluginRow() {
    const auto* entry = rex::cvar::GetFlagInfo("gpu_plugin");
    if (!entry)
      return;

    static constexpr const char* kNativeLabel = "experimental";
    auto label_for = [&](const std::string& id) -> const char* {
      return id.empty() ? kNativeLabel : id.c_str();
    };

    std::string current = entry->getter();

    ImGui::TextUnformatted("GPU Plugin");
    if (!entry->description.empty() && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", entry->description.c_str());
    }
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("gpu_plugin");
    if (ImGui::BeginCombo("##v", label_for(current))) {
      {
        bool selected = current.empty();
        ImGui::PushID(-1);
        if (ImGui::Selectable(kNativeLabel, selected)) {
          rex::cvar::SetFlagByName("gpu_plugin", "", /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      for (int i = 0; i < static_cast<int>(gpu_plugin_names_.size()); ++i) {
        bool selected = (current == gpu_plugin_names_[i]);
        ImGui::PushID(i);
        if (ImGui::Selectable(gpu_plugin_names_[i].c_str(), selected)) {
          rex::cvar::SetFlagByName("gpu_plugin", gpu_plugin_names_[i], /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

#if REX_HAS_VULKAN
  // vulkan_device is a raw index into the physical-device list the Vulkan
  // provider enumerates at graphics setup time (-1 = auto-select). Entries
  // flagged is_duplicate_of_earlier are the same physical device as an
  // earlier entry (a driver/ICD quirk, not a second GPU) -- skipped here
  // since offering them would be a redundant, indistinguishable choice, not
  // just a duplicate label; the earlier entry's index selects the exact same
  // device.
  void DrawVulkanDeviceRow() {
    const auto* entry = rex::cvar::GetFlagInfo("vulkan_device");
    if (!entry || vulkan_devices_.empty())
      return;

    int current = std::atoi(entry->getter().c_str());
    auto label_for = [this](int real_idx) -> const std::string& {
      static const std::string kAuto = "Auto";
      return real_idx < 0 ? kAuto : vulkan_devices_[real_idx].name;
    };

    ImGui::TextUnformatted("Vulkan Device");
    if (!entry->description.empty() && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", entry->description.c_str());
    }
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("vulkan_device");
    if (ImGui::BeginCombo("##v", label_for(current).c_str())) {
      {
        bool selected = (current < 0);
        ImGui::PushID(-1);
        if (ImGui::Selectable("Auto", selected)) {
          rex::cvar::SetFlagByName("vulkan_device", "-1", /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      for (int i = 0; i < static_cast<int>(vulkan_devices_.size()); ++i) {
        if (vulkan_devices_[i].is_duplicate_of_earlier)
          continue;
        bool selected = (current == i);
        ImGui::PushID(i);
        if (ImGui::Selectable(vulkan_devices_[i].name.c_str(), selected)) {
          rex::cvar::SetFlagByName("vulkan_device", std::to_string(i), /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
#endif

  void DrawAdvancedRow(const char* name) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;

    bool read_only = (entry->lifecycle == rex::cvar::Lifecycle::kInitOnly);
    ImGui::PushID(name);
    if (read_only)
      ImGui::BeginDisabled();

    ImGui::TextUnformatted(name);
    if (!entry->description.empty() && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", entry->description.c_str());
    }
    ImGui::SameLine(240.0f);
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveAdvanced();
    }

    if (read_only)
      ImGui::EndDisabled();
    ImGui::PopID();
  }

  rex::ui::Window* window_;
  std::filesystem::path user_settings_path_;
  std::filesystem::path app_config_path_;
  std::vector<std::string> gpu_plugin_names_;
  std::string gpu_backend_query_plugin_;  // Cache key for gpu_backend_names_.
  std::vector<std::string> gpu_backend_names_;
#if REX_HAS_VULKAN
  std::vector<rex::ui::vulkan::DeviceInfo> vulkan_devices_;
#endif
  rex::input::InputSystem* input_system_;
  std::unique_ptr<rex::ui::SettingsDialog> dev_settings_overlay_;

  rex::system::AutoUpdater auto_updater_;
  bool update_check_requested_ = false;
};

}  // namespace

double VolumeAmplitudeFromPercent(int percent) {
  if (percent <= 0)
    return 0.0;
  if (percent >= 100)
    return 1.0;
  double db = kMinVolumeDb * (100 - percent) / 100.0;
  return std::pow(10.0, db / 20.0);
}

int VolumePercentFromAmplitude(double amplitude) {
  if (amplitude <= 0.0)
    return 0;
  double db = 20.0 * std::log10(amplitude);
  if (db <= kMinVolumeDb)
    return 0;
  return std::clamp(static_cast<int>(std::lround(100.0 - db * 100.0 / kMinVolumeDb)), 0, 100);
}

void ApplySettingDefaults() {
  for (const auto& d : kGameDefaults) {
    rex::cvar::SetDefaultValue(d.cvar, d.value);
  }
}

void PrewarmSettingsDialogCaches() {
  g_gpu_plugin_names = rex::system::EnumerateGpuPlugins();
#if REX_HAS_VULKAN
  g_vulkan_devices = rex::ui::vulkan::EnumerateDevices();
#endif
}

void RegisterLanguageOptionsListener(rex::system::ModRegistry* registry) {
  if (!registry)
    return;
  registry->Subscribe("settings.language_option",
                       [](const rex::system::ModRegistry::EventPayload& payload) {
                         std::string id = std::to_string(payload.u64);
                         if (IsKnownLanguageId(id)) {
                           REXLOG_WARN(
                               "[settings] ignoring duplicate settings.language_option id {} "
                               "(already registered)",
                               id);
                           return;
                         }
                         std::string label(reinterpret_cast<const char*>(payload.bytes.data()),
                                            payload.bytes.size());
                         if (label.empty()) {
                           REXLOG_WARN(
                               "[settings] ignoring settings.language_option id {} with empty "
                               "label",
                               id);
                           return;
                         }
                         g_mod_language_options.push_back({std::move(id), std::move(label)});
                       });
}

std::vector<LanguageOption> GetLanguageOptions() {
  // g_mod_language_options's strings outlive this call (only ever appended
  // to, never cleared/reallocated-away during the app's lifetime), so
  // c_str() pointers here stay valid for the caller's use.
  std::vector<LanguageOption> options(kLanguageOptions.begin(), kLanguageOptions.end());
  for (const auto& mod_opt : g_mod_language_options) {
    options.push_back({mod_opt.id.c_str(), mod_opt.label.c_str()});
  }
  return options;
}

void RegisterNativeStringListener(rex::system::ModRegistry* registry) {
  if (!registry)
    return;
  registry->Subscribe(
      "settings.native_string", [](const rex::system::ModRegistry::EventPayload& payload) {
        std::string_view kv(reinterpret_cast<const char*>(payload.bytes.data()),
                             payload.bytes.size());
        const size_t eq = kv.find('=');
        if (eq == std::string_view::npos) {
          REXLOG_WARN("[settings] ignoring malformed settings.native_string payload (no '=')");
          return;
        }
        const std::string_view key = kv.substr(0, eq);
        const std::string_view value = kv.substr(eq + 1);
        if (key.empty() || value.empty()) {
          REXLOG_WARN(
              "[settings] ignoring settings.native_string payload with empty key or value");
          return;
        }
        std::string map_key = std::to_string(payload.u64) + ":" + std::string(key);
        if (g_native_string_translations.contains(map_key)) {
          REXLOG_WARN(
              "[settings] ignoring duplicate settings.native_string {} for language {} (already "
              "registered)",
              key, payload.u64);
          return;
        }
        g_native_string_translations.emplace(std::move(map_key), Utf8ToUtf16(value));
      });
}

const char16_t* FindNativeStringTranslation(uint32_t language_id, std::string_view key) {
  const auto it =
      g_native_string_translations.find(std::to_string(language_id) + ":" + std::string(key));
  return it != g_native_string_translations.end() ? it->second.c_str() : nullptr;
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateSettingsDialog(
    rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
    std::filesystem::path user_settings_path, std::filesystem::path app_config_path,
    rex::input::InputSystem* input_system) {
  return std::make_unique<CuratedSettingsDialog>(drawer, window, std::move(user_settings_path),
                                                 std::move(app_config_path), input_system);
}

}  // namespace nocturne
