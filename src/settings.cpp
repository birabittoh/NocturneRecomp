// nocturnerecomp - ReXGlue Recompiled Project
// See settings.h for details.

#include "settings.h"

#include "enhancements.h"
#include "game_data.h"
#include "graphics_settings.h"
#include "host_menu.h"
#include "jp_artwork.h"
#include "jp_voices.h"
#include "target.h"
#include "rando_runtime.h"
#include "touch_input_driver.h"
#include "ui_font.h"
#include "ui_text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "generated/nocturnerecomp_init.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/system/auto_updater.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_widgets.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/window.h>
#include <imgui.h>

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif
#if defined(__ANDROID__)
#include "android_gpu_driver.h"
#endif

namespace nocturne {

namespace {

// A modal asking to confirm a restart. Call every frame; `open` requests it.
// `id` keeps the popups of different buttons apart.
bool ConfirmRestart(const char* id, bool open, const char* message_key = "restart_confirm",
                    const char* yes_key = "restart_yes") {
  const std::string popup = std::string(Tr("settings", "restart_title")) + "##" + id;
  if (open)
    ImGui::OpenPopup(popup.c_str());
  bool confirmed = false;
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal(popup.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::TextUnformatted(Tr("settings", message_key));
    ImGui::Spacing();
    if (ImGui::Button(Tr("settings", yes_key))) {
      confirmed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("settings", "restart_cancel")))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return confirmed;
}

// Everything build.py used to write into the shipped nocturnerecomp.toml.
// These are the game's defaults, not necessarily the SDK's; kept here so
// there is one place that owns "what NocturneRecomp ships with". Keybind/
// remap/danger-zone entries have no row in the curated
// dialog below (left to the SDK's own developer panel) but still need their
// default seeded here, same as build.py used to.
struct DefaultValue {
  const char* cvar;
  const char* value;
};

constexpr std::array kGameDefaults = {
    DefaultValue{"user_language", "1"},
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
    DefaultValue{"gpu_allow_invalid_fetch_constants", "true"},
    DefaultValue{"game_data_root", "assets"},
    DefaultValue{"update_data_root", "update"},
    DefaultValue{"mods_data_root", "mods"},
    DefaultValue{"license_mask", "1"},
    // Static mirror of the mod catalog, attached to every release of the mods
    // repo; used when the Firestore backend is rate limited or down.
    DefaultValue{"mod_catalog_fallback_url",
                 "https://github.com/birabittoh/NocturneRecomp-Mods/releases/latest/"
                 "download/catalog.json"},
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
// everyone, so it gets no curated row.
// The enhancement tabs, in order: res/lang's [enhancements] section keys.
// "Other" also holds the cosmetic and game mode settings.
constexpr std::array<const char*, 4> kEnhancementTabs = {
    "section_quality_of_life", "section_fixes", "section_items_and_shop", "section_other"};

// The intro skips (intro_skips.cpp), in the Other tab's More skips section. Not
// enhancements: they work without the randomizer core, so they stay enabled
// when it's unsupported. Label and description are res/lang's [enhancements]
// <cvar> and <cvar>_desc.
constexpr std::array<const char*, 2> kIntroSkipCvarNames = {"skip_intros", "skip_richter"};

constexpr std::array<const char*, 16> kBasicCvarNames = {
    "fullscreen",       "vsync",         "user_language",
    "gpu_backend",      "vulkan_device", "audio_mute",      "audio_volume",
    "graphics_style_enhanced", "touch_controls", "touch_movement", "touch_function_keys", "touch_scale",
    "touch_opacity", "touch_hide_delay", "voices", "artwork"};

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

std::vector<std::string> BasicCvarNames() {
  return std::vector<std::string>(kBasicCvarNames.begin(), kBasicCvarNames.end());
}

// Populated once by PrewarmSettingsDialogCaches(); every CuratedSettingsDialog
// instance reads from these rather than re-running the underlying (expensive)
// enumeration itself.
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
#if REX_HAS_VULKAN
    vulkan_devices_ = g_vulkan_devices;
#endif
  }

 protected:
  void OnDraw(ImGuiIO& /*io*/) override {
    const UiFontScope font;
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin((std::string(Tr("settings", "title")) + "##rex").c_str(), nullptr,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (AnyPendingRestart()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
      ImGui::TextWrapped("%s", Tr("settings", "restart_banner"));
      ImGui::PopStyleColor();
      ImGui::SameLine();
      const bool open_confirm =
          ImGui::SmallButton((std::string(Tr("settings", "restart_now")) + "##restart").c_str());
      // Not Relaunch()+RequestClose() inline: under the native renderer this
      // overlay draws on the guest's render thread, and closing the window from
      // there hangs. RestartNow() marshals the close onto the UI thread.
      if (ConfirmRestart("confirm_restart", open_confirm))
        RestartNow();
      ImGui::Separator();
    }

    if (ImGui::BeginTabBar("##settings_tabs")) {
      if (ImGui::BeginTabItem(Tr("settings", "tab_general"))) {
        DrawGeneralTab();
        ImGui::EndTabItem();
      }
      // One tab per group of enhancements, labelled like its old section.
      for (const char* section : kEnhancementTabs) {
        if (ImGui::BeginTabItem(Tr("enhancements", section))) {
          DrawEnhancementsTab(section);
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button(Tr("settings", "reset_all"))) {
      for (const char* name : kBasicCvarNames) {
        rex::cvar::ResetToDefault(name);
      }
      for (const auto& name : EnhancementCvarNames()) {
        rex::cvar::ResetToDefault(name);
      }
      for (const char* name : kIntroSkipCvarNames) {
        rex::cvar::ResetToDefault(name);
      }
      SaveBasic();
      SaveEnhancements();
      SaveIntroSkips();
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
    if (ImGui::Button(dev_settings_overlay_ ? Tr("settings", "close_all_settings")
                                         : Tr("settings", "all_settings"))) {
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
  // that simply differs from the SDK's factory default (e.g. GPU Backend
  // set to Vulkan) doesn't trip this on a fresh launch. See SetFlagByNameImpl's
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
    return false;
  }

  void DrawGeneralTab() {
    DrawUpdateSection();

    ImGui::SeparatorText(Tr("settings", "section_game"));
    DrawLanguageRow();
    DrawReleaseChoiceRow("voices", Tr("settings", "voices"), JpVoicesAvailable());
    DrawReleaseChoiceRow("artwork", Tr("settings", "artwork"), JpArtworkAvailable());
    DrawStartScreenRow();

    ImGui::SeparatorText(Tr("settings", "section_audio"));
    DrawAudioMuteRow();
    DrawAudioVolumeRow();

    ImGui::SeparatorText(Tr("settings", "section_graphics"));
#if !REX_PLATFORM_ANDROID
    // Android is always fullscreen.
    DrawFullscreenRow();
#endif
    DrawVsyncRow();
    DrawGraphicsStyleRow();
    // The native renderer reads both: gpu_backend picks D3D12 or Vulkan (only
    // Windows has the choice) and vulkan_device the adapter when Vulkan is in use.
#ifdef _WIN32
    DrawGpuBackendRow();
#endif
#if REX_HAS_VULKAN
#ifdef _WIN32
    if (rex::cvar::GetFlagByName("gpu_backend") == "vulkan")
#endif
      DrawVulkanDeviceRow();
#endif

    ImGui::SeparatorText(Tr("settings", "section_controls"));
    DrawTouchRows();
  }

  // The on-screen pad (see touch_layout.h). Its sliders only show while it is on.
  void DrawTouchRows() {
    const auto* entry = rex::cvar::GetFlagInfo("touch_controls");
    if (!entry)
      return;
    ImGui::TextUnformatted(Tr("settings", "touch_controls"));
    ImGui::SameLine(180.0f);
    ImGui::PushID("touch_controls");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true))
      SaveBasic();
    ImGui::PopID();
    if (entry->getter() != "true")
      return;
    DrawTouchMovementRow();
    DrawTouchFunctionKeysRow();
    DrawTouchSliderRow("touch_scale", Tr("settings", "touch_scale"), 0.5f, 2.0f, "%.2f");
    DrawTouchSliderRow("touch_opacity", Tr("settings", "touch_opacity"), 0.1f, 1.0f, "%.2f");
    // Whole seconds; the top step means the pad never hides.
    DrawTouchSliderRow("touch_hide_delay", Tr("settings", "touch_hide_delay"), 1.0f, 11.0f, "%.0f", /*never_at_max=*/true);
  }

  void DrawTouchFunctionKeysRow() {
    const auto* entry = rex::cvar::GetFlagInfo("touch_function_keys");
    if (!entry)
      return;
    ImGui::TextUnformatted(Tr("settings", "touch_function_keys"));
    ImGui::SameLine(180.0f);
    ImGui::PushID("touch_function_keys");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true))
      SaveBasic();
    ImGui::PopID();
  }

  void DrawTouchMovementRow() {
    const std::string current = rex::cvar::GetFlagByName("touch_movement");
    ImGui::TextUnformatted(Tr("settings", "touch_movement"));
    ImGui::SameLine(180.0f);
    ImGui::PushID("touch_movement");
    if (ImGui::RadioButton(Tr("settings", "touch_movement_dpad"), current != "stick")) {
      rex::cvar::SetFlagByName("touch_movement", "dpad", /*persist=*/true);
      SaveBasic();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(Tr("settings", "touch_movement_stick"), current == "stick")) {
      rex::cvar::SetFlagByName("touch_movement", "stick", /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawTouchSliderRow(const char* name, const char* label, float min, float max,
                          const char* format, bool never_at_max = false) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;
    float value = std::clamp(float(std::atof(entry->getter().c_str())), min, max);
    if (never_at_max)
      value = std::round(value);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID(name);
    const bool never = never_at_max && value >= max;
    if (ImGui::SliderFloat("##v", &value, min, max, never ? Tr("settings", "touch_never") : format,
                           ImGuiSliderFlags_AlwaysClamp)) {
      if (never_at_max)
        value = std::round(value);
      char text[16];
      std::snprintf(text, sizeof(text), "%.2f", value);
      rex::cvar::SetFlagByName(name, text, /*persist=*/true);
    }
    // Keeps the pad drawn while it is being resized or faded, so the change shows.
    if (ImGui::IsItemActive())
      touch::TouchKeepVisible();
    if (ImGui::IsItemDeactivatedAfterEdit())
      SaveBasic();
    ImGui::PopID();
  }

  // Restarts into the start screen, where the game files can be deleted or
  // another source picked.
  void DrawStartScreenRow() {
    const bool open_restart =
        ImGui::Button((std::string(Tr("settings", "start_screen")) + "##start_screen").c_str());
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", Tr("settings", "start_screen_tip"));
    if (ConfirmRestart("confirm_start_screen", open_restart)) {
      RestartToStartScreen();
    }
    ImGui::SameLine();
    const bool open_quit =
        ImGui::Button((std::string(Tr("settings", "quit_game")) + "##quit_game").c_str());
    if (ConfirmRestart("confirm_quit", open_quit, "quit_confirm", "quit_yes"))
      QuitNow();
  }

  // The randomizer core's standalone patches (see enhancements.h), one tab per
  // section. The runtime picks a change up on the next guest frame, so these
  // tabs only edit the cvars.
  void DrawEnhancementsTab(const std::string_view section) {
    const auto runtime = GetRandoRuntime().state();
    const float wrap_width = 400.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
    if (!runtime.supported) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
      ImGui::TextUnformatted(runtime.unsupported_reason.c_str());
      ImGui::PopStyleColor();
    } else if (GetRandoRuntime().WantedEnhancements() != runtime.enhancements) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
      ImGui::TextUnformatted(Tr("enhancements", "applying"));
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();

    ImGui::BeginDisabled(!runtime.supported);
    const rando::Options* seed = runtime.active ? &runtime.active->options : nullptr;
    DrawEnhancementToggles(section, seed);
    if (section == "section_other") {
      ImGui::SeparatorText(Tr("enhancements", "section_cosmetic"));
      DrawCosmeticSection(seed);
      ImGui::SeparatorText(Tr("enhancements", "section_game_mode"));
      DrawGameModeSection(seed);
    }
    ImGui::EndDisabled();
    if (section == "section_other") {
      ImGui::SeparatorText(Tr("enhancements", "section_more_skips"));
      DrawIntroSkipToggles();
    }
  }

  // skip_intros takes effect at the next launch, skip_richter at the next new
  // game; their descriptions say so.
  void DrawIntroSkipToggles() {
    for (const char* cvar : kIntroSkipCvarNames) {
      const auto* entry = rex::cvar::GetFlagInfo(cvar);
      if (!entry) {
        continue;
      }
      ImGui::PushID(cvar);
      ImGui::BeginGroup();
      bool on = entry->getter() == "true";
      if (rex::ui::ToggleSwitch("##v", &on)) {
        rex::cvar::SetFlagByName(cvar, on ? "true" : "false", /*persist=*/true);
        SaveIntroSkips();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(Tr("enhancements", cvar));
      ImGui::EndGroup();
      EnhancementTooltip(Tr("enhancements", (std::string(cvar) + "_desc").c_str()),
                         SeedUse::kNone);
      ImGui::PopID();
    }
  }

  // The on/off rows of `section`: the list's entries from the one that opens
  // it up to the next that opens another.
  void DrawEnhancementToggles(const std::string_view section, const rando::Options* seed) {
    bool in_section = false;
    for (const auto& e : EnhancementList()) {
      if (e.section) {
        in_section = section == e.section;
      }
      if (!in_section) {
        continue;
      }
      const auto* entry = rex::cvar::GetFlagInfo(e.cvar);
      if (!entry) {
        continue;
      }
      ImGui::PushID(e.cvar);
      const SeedUse use = seed ? e.seed_use(*seed) : SeedUse::kNone;
      ImGui::BeginGroup();
      bool on = entry->getter() == "true";
      if (rex::ui::ToggleSwitch("##v", &on)) {
        rex::cvar::SetFlagByName(e.cvar, on ? "true" : "false", /*persist=*/true);
        SaveEnhancements();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(Tr("enhancements", e.cvar));
      DrawSeedMarker(use);
      ImGui::EndGroup();
      EnhancementTooltip(Tr("enhancements", (std::string(e.cvar) + "_desc").c_str()), use);
      ImGui::PopID();
    }
  }

  void DrawCosmeticSection(const rando::Options* seed) {
    const rando::Enhancements current = CurrentEnhancements();
    ImGui::PushID(kAlchemyVatsCvar);
    const SeedUse vats_use = seed ? AlchemyVatsSeedUse(*seed) : SeedUse::kNone;
    bool shuffle_vats = current.alchemy_vats != 0;
    ImGui::BeginGroup();
    if (rex::ui::ToggleSwitch("##v", &shuffle_vats)) {
      SetAlchemyVatsShuffle(shuffle_vats ? NewShuffleSeed() : 0);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(Tr("enhancements", "shuffle_vats"));
    DrawSeedMarker(vats_use);
    ImGui::EndGroup();
    EnhancementTooltip(Tr("enhancements", "shuffle_vats_desc"), vats_use);
    if (shuffle_vats) {
      ImGui::SameLine();
      if (ImGui::SmallButton(Tr("enhancements", "reshuffle"))) {
        SetAlchemyVatsShuffle(NewShuffleSeed());
      }
    }
    ImGui::PopID();

    ImGui::Spacing();
    ImGui::TextUnformatted(Tr("enhancements", "wing_smash_color"));
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    const int color = current.wing_smash_color;
    if (ImGui::BeginCombo("##wing_smash_color", WingSmashColorName(color))) {
      for (int i = 0; i <= rando::kWingSmashColorCount; ++i) {
        if (ImGui::Selectable(WingSmashColorName(i), i == color) && i != color) {
          rex::cvar::SetFlagByName(kWingSmashColorCvar, std::to_string(i), /*persist=*/true);
          SaveEnhancements();
        }
      }
      ImGui::EndCombo();
    }
  }

  void DrawGameModeSection(const rando::Options* seed) {
    const rando::GameMode mode = CurrentEnhancements().game_mode;
    // A running game keeps the mode it was started or saved in.
    const auto running = GetRandoRuntime().state().game_mode;
    if (running && *running != mode) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400.0f);
      ImGui::Text(Tr("enhancements", "mode_running"), GameModeName(*running));
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
    }
    const SeedUse mode_use = seed ? GameModeSeedUse(*seed, mode) : SeedUse::kNone;
    const char* kModeNote = Tr("enhancements", "mode_note");
    ImGui::BeginGroup();
    ImGui::TextUnformatted(Tr("enhancements", "mode"));
    DrawSeedMarker(mode_use);
    ImGui::EndGroup();
    EnhancementTooltip(GameModeDescription(mode), mode_use,
                       mode != rando::GameMode::kNormal ? kModeNote : nullptr);
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##game_mode", GameModeName(mode))) {
      for (int i = 0; i < static_cast<int>(rando::GameMode::kCount); ++i) {
        const auto m = static_cast<rando::GameMode>(i);
        if (ImGui::Selectable(GameModeName(m), m == mode) && m != mode) {
          rex::cvar::SetFlagByName(kGameModeCvar, std::to_string(i), /*persist=*/true);
          SaveEnhancements();
        }
        EnhancementTooltip(GameModeDescription(m),
                           seed ? GameModeSeedUse(*seed, m) : SeedUse::kNone,
                           m != rando::GameMode::kNormal ? kModeNote : nullptr);
      }
      ImGui::EndCombo();
    } else {
      EnhancementTooltip(GameModeDescription(mode), mode_use,
                         mode != rando::GameMode::kNormal ? kModeNote : nullptr);
    }
  }

  // A short note after a row's label on what the active seed does about it;
  // EnhancementTooltip spells it out.
  static void DrawSeedMarker(SeedUse use) {
    if (use == SeedUse::kNone) {
      return;
    }
    ImGui::SameLine();
    if (use == SeedUse::kIncluded) {
      ImGui::TextDisabled("%s", Tr("enhancements", "in_seed"));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
      ImGui::TextUnformatted(Tr("enhancements", "not_applied"));
      ImGui::PopStyleColor();
    }
  }

  // Hover help for the item just drawn (also while the tab is disabled): the
  // description, `note` if any, and what the active seed does about it.
  static void EnhancementTooltip(const char* description, SeedUse use,
                                 const char* note = nullptr) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                              ImGuiHoveredFlags_AllowWhenDisabled) ||
        !ImGui::BeginTooltip()) {
      return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(description);
    if (note) {
      ImGui::TextDisabled("%s", note);
    }
    if (use == SeedUse::kIncluded) {
      ImGui::TextDisabled("%s", Tr("enhancements", "tip_in_seed"));
    } else if (use == SeedUse::kConflicts) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
      ImGui::TextUnformatted(Tr("enhancements", "tip_conflicts"));
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }

  void SetAlchemyVatsShuffle(uint32_t shuffle) {
    rex::cvar::SetFlagByName(kAlchemyVatsCvar, std::to_string(shuffle), /*persist=*/true);
    SaveEnhancements();
  }

  // A nonzero rand() seed for the vat shuffle.
  static uint32_t NewShuffleSeed() {
    uint32_t v = 0;
    while (!v) {
      v = std::random_device{}();
    }
    return v;
  }

  void SaveBasic() { rex::cvar::SaveConfigSubset(user_settings_path_, BasicCvarNames()); }
  void SaveEnhancements() {
    rex::cvar::SaveConfigSubset(user_settings_path_, EnhancementCvarNames());
  }
  void SaveIntroSkips() {
    std::vector<std::string> names(kIntroSkipCvarNames.begin(), kIntroSkipCvarNames.end());
    rex::cvar::SaveConfigSubset(user_settings_path_, names);
  }
  // Game self-update (see rex::system::AutoUpdater), surfaced here rather
  // than the SDK's mod manager overlay (F1) since a player who never touches
  // mods should still be told about an available update
  void DrawUpdateSection() {
    if (!rex::system::AutoUpdater::SupportsSelfUpdate())
      return;

    if (!update_check_requested_) {
      update_check_requested_ = true;
      auto_updater_.CheckAsync();
    }

    // A previous session already downloaded and staged an update (whether or
    // not this one ever calls CheckAsync/InstallAsync again); offer the
    // restart regardless of auto_updater_'s own in-memory state.
    // InstallRoot, not the exe folder: on macOS the whole .app is swapped.
    const auto install_root = rex::system::AutoUpdater::InstallRoot();
    if (rex::system::AutoUpdater::HasPendingSelfUpdate(install_root)) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.55f, 1.0f));
      ImGui::TextWrapped("%s", Tr("settings", "update_downloaded"));
      ImGui::PopStyleColor();
      ImGui::SameLine();
#if REX_PLATFORM_ANDROID
      // The system installer takes it from here and replaces the app itself;
      // closing the game would only dismiss the installer prompt.
      if (ImGui::SmallButton((std::string(Tr("settings", "install_update")) + "##autoupdate").c_str()))
        rex::system::AutoUpdater::ApplyAndRestart(install_root, {});
#else
      const bool open_confirm = ImGui::SmallButton(
          (std::string(Tr("settings", "restart_apply")) + "##autoupdate").c_str());
      if (ConfirmRestart("confirm_update", open_confirm)) {
        // This install root contains the running executable itself,
        // which stays locked for this process's whole lifetime (see
        // AutoUpdater::ApplyAndRestart's contract). The spawned helper
        // outlives this process, applies the swap, and launches the new exe.
        if (rex::system::AutoUpdater::ApplyAndRestart(install_root,
                                                       rex::filesystem::GetExecutablePath()))
          QuitNow();
      }
#endif
      ImGui::Separator();
      return;
    }

    auto install = auto_updater_.InstallSnapshot();
    if (install.in_progress) {
      if (install.total_bytes > 0) {
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%.0f",
                      100.0 * static_cast<double>(install.downloaded_bytes) /
                          static_cast<double>(install.total_bytes));
        ImGui::TextDisabled("%s", Tr("settings", "downloading_percent", percent).c_str());
      } else {
        ImGui::TextDisabled("%s", Tr("settings", "downloading"));
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
    ImGui::TextWrapped("%s", Tr("settings", "update_available", info->version).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("settings", "download_update"))) {
      auto_updater_.InstallAsync(*info, install_root);
    }
    ImGui::Separator();
  }

  void DrawFullscreenRow() {
    const auto* entry = rex::cvar::GetFlagInfo("fullscreen");
    if (!entry)
      return;
    ImGui::TextUnformatted(Tr("settings", "fullscreen"));
    ImGui::SameLine(180.0f);
    ImGui::PushID("fullscreen");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawVsyncRow() {
    const auto* entry = rex::cvar::GetFlagInfo("vsync");
    if (!entry)
      return;
    ImGui::TextUnformatted(Tr("settings", "vsync"));
    ImGui::SameLine(180.0f);
    ImGui::PushID("vsync");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawAudioMuteRow() {
    const auto* entry = rex::cvar::GetFlagInfo("audio_mute");
    if (!entry)
      return;
    ImGui::TextUnformatted(Tr("settings", "mute_audio"));
    ImGui::SameLine(180.0f);
    ImGui::PushID("audio_mute");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  // Voices and artwork: whose release to use. Only with both installed is
  // there a choice; otherwise the row is disabled and shows the one installed.
  // Both apply live (see jp_voices.h, jp_artwork.h); "Auto" follows the language.
  void DrawReleaseChoiceRow(const char* cvar, const char* label, bool available) {
    const auto* entry = rex::cvar::GetFlagInfo(cvar);
    if (!entry)
      return;

    static const std::array<std::string, 3> kValues = {"auto", "jp", "world"};
    auto label_for = [](const std::string& id) -> std::string {
      if (id == "jp")
        return "JP";
      if (id == "world")
        return "World";
      return Tr("settings", "auto");
    };

    const std::string current =
        available ? entry->getter() : (TargetReleaseId() == "xbla-jp" ? "jp" : "world");
    ImGui::TextUnformatted(label);
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID(cvar);
    ImGui::BeginDisabled(!available);
    if (ImGui::BeginCombo("##v", label_for(current).c_str())) {
      for (const auto& value : kValues) {
        const bool selected = value == current;
        if (ImGui::Selectable(label_for(value).c_str(), selected)) {
          rex::cvar::SetFlagByName(cvar, value, /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", Tr("settings", "choice_needs_both"));
    ImGui::PopID();
  }

  // audio_volume is a Double cvar (0.0-1.0 linear amplitude, applied directly
  // to samples by the SDL audio driver); DrawCvarWidget's generic Double path
  // is a plain InputDouble box, not a slider, so this draws its own row
  // instead, displaying and editing a perceptually-spaced percentage (see
  // VolumeAmplitudeFromPercent) rather than the raw amplitude directly.
  void DrawAudioVolumeRow() {
    const auto* entry = rex::cvar::GetFlagInfo("audio_volume");
    if (!entry)
      return;
    int percent = VolumePercentFromAmplitude(std::atof(entry->getter().c_str()));

    ImGui::TextUnformatted(Tr("settings", "volume"));
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

  void DrawGraphicsStyleRow() {
    const auto* entry = rex::cvar::GetFlagInfo("graphics_style_enhanced");
    if (!entry)
      return;
    ImGui::TextUnformatted(Tr("settings", "enhanced_graphics"));
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

    ImGui::TextUnformatted(Tr("settings", "language"));
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

  void DrawGpuBackendRow() {
    const auto* entry = rex::cvar::GetFlagInfo("gpu_backend");
    if (!entry)
      return;

    static const std::array<std::string, 2> gpu_backend_names_ = {"d3d12", "vulkan"};
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

    ImGui::TextUnformatted(Tr("settings", "gpu_backend"));
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
      static const std::string kAuto = Tr("settings", "auto");
      return real_idx < 0 ? kAuto : vulkan_devices_[real_idx].name;
    };

    ImGui::TextUnformatted(Tr("settings", "vulkan_device"));
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
        if (ImGui::Selectable(Tr("settings", "auto"), selected)) {
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

  rex::ui::Window* window_;
  std::filesystem::path user_settings_path_;
  std::filesystem::path app_config_path_;
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

namespace {
rex::ui::Window* g_window = nullptr;
std::filesystem::path g_user_settings_path;
}  // namespace

void MarkCvarLive(std::string_view name) {
  for (auto& entry : rex::cvar::GetRegistry()) {
    if (entry.name == name) {
      entry.lifecycle = rex::cvar::Lifecycle::kHotReload;
    }
  }
}

void BindWindowTarget(rex::ui::Window* window, std::filesystem::path user_settings_path) {
  g_window = window;
  g_user_settings_path = std::move(user_settings_path);
  // PersistWindowSize only records the size for the next launch; the window
  // already is that size, so saving it is no reason to restart.
  MarkCvarLive("window_width");
  MarkCvarLive("window_height");
}

void PersistWindowSize() {
  if (g_user_settings_path.empty() || g_window == nullptr)
    return;
  // Fullscreen size is the display's, not a window size worth coming back to.
  if (g_window->IsFullscreen())
    return;
  // Logical, because that is the unit window_width/window_height are read in;
  // the renderer's extent is in physical pixels and would grow the window on
  // every launch on a scaled display.
  const uint32_t width = g_window->GetActualLogicalWidth();
  const uint32_t height = g_window->GetActualLogicalHeight();
  if (width == 0 || height == 0 || width > 8192 || height > 8192)
    return;
  auto* width_entry = rex::cvar::GetFlagInfo("window_width");
  auto* height_entry = rex::cvar::GetFlagInfo("window_height");
  if (!width_entry || !height_entry)
    return;
  const std::string want_width = std::to_string(width);
  const std::string want_height = std::to_string(height);
  if (width_entry->getter() == want_width && height_entry->getter() == want_height)
    return;
  // Outside kBasicCvarNames, so Reset All leaves the window size alone.
  rex::cvar::SetFlagByName("window_width", want_width, /*persist=*/true);
  rex::cvar::SetFlagByName("window_height", want_height, /*persist=*/true);
  rex::cvar::SaveConfigSubset(g_user_settings_path, {"window_width", "window_height"});
}

bool RestartNow() {
#if REX_PLATFORM_ANDROID
  // Replaces the process itself, so there is no window left to close here.
  return g_window && RestartAndroidApp();
#else
  if (!g_window || !rex::platform::process::Relaunch())
    return false;
  // RequestClose ends up in WindowSDL::RequestCloseImpl, which destroys the
  // window directly, so it has to run on the thread that owns the window.
  // Callers are not: the native Options screen runs on the guest CPU thread,
  // and the F4 overlay on the guest's render thread. Closing from there hangs,
  // leaving the old window up next to the relaunched one.
  return QuitNow();
#endif
}

bool RestartToStartScreen() {
  if (g_user_settings_path.empty())
    return false;
  const auto flag = StartScreenFlagPath(g_user_settings_path);
  RequestStartScreenOnNextLaunch(flag, true);
  if (RestartNow())
    return true;
  RequestStartScreenOnNextLaunch(flag, false);
  return false;
}

bool QuitNow() {
  if (!g_window)
    return false;
  rex::ui::Window* window = g_window;
  window->app_context().CallInUIThread([window] { window->RequestClose(); });
  return true;
}

bool CallInWindowThread(std::function<void()> work) {
  if (!g_window)
    return false;
  return g_window->app_context().CallInUIThread(std::move(work));
}

void PrewarmSettingsDialogCaches() {
#if REX_HAS_VULKAN
#if defined(__ANDROID__)
  // Would load the stock driver next to the custom one adrenotools brings in.
  if (!CustomVulkanDriverSelected())
#endif
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
        AddUiString(static_cast<uint32_t>(payload.u64), "native", key, value);
      });
}

const char16_t* FindNativeStringTranslation(uint32_t language_id, std::string_view key) {
  const char* text = FindModUiString(language_id, "native", key);
  return text ? CachedUtf16(text) : nullptr;
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateSettingsDialog(
    rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
    std::filesystem::path user_settings_path, std::filesystem::path app_config_path,
    rex::input::InputSystem* input_system) {
  return std::make_unique<CuratedSettingsDialog>(drawer, window, std::move(user_settings_path),
                                                 std::move(app_config_path), input_system);
}

}  // namespace nocturne

// The pause menu's "Quit game" row (formerly "Return to Arcade", strings 31 and
// 48; see font_overlay.cpp). Its handler, in the function table at 0x820099C8,
// is a thunk that tail-calls XLaunchNewImage("XLNI_DASH_ARCADE", 0) to return
// to the Xbox dashboard, which has nowhere to go here. Close the game instead,
// as the settings dialog's Quit Game button does.
REX_HOOK_RAW(sub_82577430) {
  if (!nocturne::QuitNow())
    REXLOG_WARN("[quit] no window to close");
}
