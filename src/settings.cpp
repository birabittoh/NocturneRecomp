// nocturnerecomp - ReXGlue Recompiled Project
// See settings.h for details.

#include "settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/system/gpu_plugin.h>
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
constexpr std::array<const char*, 8> kBasicCvarNames = {
    "fullscreen",    "resolution",    "resolution_scale", "user_name",
    "user_language", "gpu_backend", "vulkan_device",     "gpu_plugin"};

// resolution_scale value that renders at "100%" (native) for a given display
// resolution. The SDK's resolution_scale is an integer EDRAM/draw
// supersampling factor (range 1-8), not a fractional multiplier, so this
// table is the source of truth for what "100%" means per resolution;
// DrawRenderScaleRow derives 50%-100% steps from it at runtime.
int ResolutionScaleFor(const std::string& resolution) {
  if (resolution == "1080p")
    return 2;
  if (resolution == "1440p")
    return 3;
  if (resolution == "4K")
    return 4;
  return 1;  // 720p, and fallback for anything unrecognized.
}

struct LanguageOption {
  const char* id;  // stringified XLanguage value, as stored by the cvar
  const char* label;
};

// XLanguage IDs per the Xbox 360 kernel's user_language cvar; note 10 is
// intentionally absent (not a valid XLanguage).
constexpr std::array kLanguageOptions = {
    LanguageOption{"1", "EN (English)"},
    LanguageOption{"2", "JA (Japanese)"},
    LanguageOption{"3", "DE (German)"},
    LanguageOption{"4", "FR (French)"},
    LanguageOption{"5", "ES (Spanish)"},
    LanguageOption{"6", "IT (Italian)"},
    LanguageOption{"7", "KO (Korean)"},
    LanguageOption{"8", "ZH (Traditional Chinese)"},
    LanguageOption{"9", "PT (Portuguese)"},
    LanguageOption{"11", "PL (Polish)"},
    LanguageOption{"12", "RU (Russian)"},
    LanguageOption{"13", "SV (Swedish)"},
    LanguageOption{"14", "TR (Turkish)"},
    LanguageOption{"15", "NB (Norwegian)"},
    LanguageOption{"16", "NL (Dutch)"},
    LanguageOption{"17", "ZH (Simplified Chinese)"},
};

// cvars rendered generically in the collapsed Advanced section, persisted to
// the app's normal cvar config (nocturnerecomp.toml).
constexpr std::array<const char*, 11> kAdvancedCvarNames = {
    "post_process_shader_enabled",
    "post_process_shader_path",
    "frame_pacer_enabled",
    "shader_dump_enabled",
    "texture_dump_enabled",
    "texture_dump_format",
    "texture_dump_skip_sizes",
    "mnk_capture_mouse",
    "mnk_mode",
    "gpu_allow_invalid_fetch_constants",
    "rando_xex_name",
};

std::vector<std::string> BasicCvarNames() {
  return std::vector<std::string>(kBasicCvarNames.begin(), kBasicCvarNames.end());
}

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
    gpu_plugin_names_ = rex::system::EnumerateGpuPlugins();
#if REX_HAS_VULKAN
    vulkan_devices_ = rex::ui::vulkan::EnumerateDevices();
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

    DrawFullscreenRow();
    DrawResolutionRow();
    DrawRenderScaleRow();
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
        dev_settings_overlay_ = std::make_unique<rex::ui::SettingsDialog>(
            imgui_drawer(), app_config_path_, input_system_, "All Settings##rexdev");
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
  void SaveAdvanced() { rex::cvar::SaveConfig(app_config_path_); }

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

  void DrawResolutionRow() {
    const auto* entry = rex::cvar::GetFlagInfo("resolution");
    if (!entry)
      return;
    static constexpr std::array<const char*, 4> kOptions = {"720p", "1080p", "1440p", "4K"};
    std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(kOptions.size()); ++i) {
      if (current == kOptions[i]) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("Resolution");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("resolution");
    if (ImGui::BeginCombo("##v", kOptions[cur_idx])) {
      for (int i = 0; i < static_cast<int>(kOptions.size()); ++i) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(kOptions[i], selected)) {
          rex::cvar::SetFlagByName("resolution", kOptions[i], /*persist=*/true);
          rex::cvar::SetFlagByName("resolution_scale",
                                   std::to_string(ResolutionScaleFor(kOptions[i])),
                                   /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  // resolution_scale is an integer cvar (range 1-8) whose named-resolution
  // steps (1/2/3/4 for 720p/1080p/1440p/4K, per ResolutionScaleFor) are the
  // only meaningful values -- each option here renders at one of the named
  // resolutions up to and including the current display resolution (e.g. at
  // 1440p: render at 720p/1080p/1440p, i.e. 33%/67%/100%). Rather than
  // sliding over the percentage itself (which lets the handle rest on
  // in-between values while dragging, since e.g. 73% is a perfectly valid
  // int even though no scale produces it), the slider's domain *is* the
  // list of valid scales, so it's mechanically impossible to drag to
  // anything else. 720p's base of 1 has only one valid scale, so the
  // slider is disabled there instead of doing nothing.
  void DrawRenderScaleRow() {
    const auto* scale_entry = rex::cvar::GetFlagInfo("resolution_scale");
    const auto* res_entry = rex::cvar::GetFlagInfo("resolution");
    if (!scale_entry || !res_entry)
      return;

    int base = ResolutionScaleFor(res_entry->getter());
    int current_scale = std::atoi(scale_entry->getter().c_str());

    std::vector<int> valid_scales;
    for (int k = 1; k <= base; ++k) {
      valid_scales.push_back(k);
    }

    int idx = 0;
    for (int i = 0; i < static_cast<int>(valid_scales.size()); ++i) {
      if (valid_scales[i] == current_scale) {
        idx = i;
        break;
      }
    }

    int max_idx = static_cast<int>(valid_scales.size()) - 1;
    if (max_idx == 0)
      return;  // Only one valid scale (720p) -- nothing to offer, hide the row.

    ImGui::PushID("render_scale_percent");

    ImGui::TextUnformatted("Render Resolution");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    bool changed = ImGui::SliderInt("##v", &idx, 0, max_idx, "");

    int display_percent = static_cast<int>(std::lround(100.0 * valid_scales[idx] / base));
    ImGui::SameLine();
    ImGui::Text("%d%%", display_percent);

    if (changed) {
      rex::cvar::SetFlagByName("resolution_scale", std::to_string(valid_scales[idx]),
                               /*persist=*/true);
      SaveBasic();
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
    std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(kLanguageOptions.size()); ++i) {
      if (current == kLanguageOptions[i].id) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("Language");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("user_language");
    if (ImGui::BeginCombo("##v", kLanguageOptions[cur_idx].label)) {
      for (int i = 0; i < static_cast<int>(kLanguageOptions.size()); ++i) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(kLanguageOptions[i].label, selected)) {
          rex::cvar::SetFlagByName("user_language", kLanguageOptions[i].id, /*persist=*/true);
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

    static constexpr const char* kNativeLabel = "Native (built-in renderer)";
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
};

}  // namespace

void ApplySettingDefaults() {
  for (const auto& d : kGameDefaults) {
    rex::cvar::SetDefaultValue(d.cvar, d.value);
  }
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateSettingsDialog(
    rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
    std::filesystem::path user_settings_path, std::filesystem::path app_config_path,
    rex::input::InputSystem* input_system) {
  return std::make_unique<CuratedSettingsDialog>(drawer, window, std::move(user_settings_path),
                                                 std::move(app_config_path), input_system);
}

}  // namespace nocturne
