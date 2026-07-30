// nocturnerecomp - ReXGlue Recompiled Project
//
// Game-curated settings: the game's own defaults for SDK cvars, and a small
// player-facing settings overlay (Fullscreen/Resolution, plus a collapsed
// Advanced section) that replaces the SDK's developer settings panel on F4
// when `settings_manager_enabled = true`. See rex::cvar::SetDefaultValue,
// rex::cvar::SaveConfigSubset, and rex::ui::DrawCvarWidget in the SDK for the
// generic mechanism this builds on.

#pragma once

#include <filesystem>
#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace rex::input {
class InputSystem;
}  // namespace rex::input

namespace nocturne {

// Overrides the SDK's built-in cvar defaults with the game's own. Call once,
// before rex::ReXApp::SetupEnvironment() (i.e. before any config file is
// loaded), so a saved config or CLI/env override still takes precedence.
void ApplySettingDefaults();

// Creates the curated settings overlay. `user_settings_path` is where the
// friendly settings (Fullscreen, Resolution) are persisted;
// `app_config_path` is where everything else (the Advanced section) is
// persisted, matching the SDK's normal cvar config file. `window` is used
// by the "Restart Now" button on the pending-restart banner: it relaunches
// the process (rex::platform::process::Relaunch) then requests `window`
// close so the new instance picks up the just-changed cvars. `input_system`
// is forwarded to the SDK's own rex::ui::SettingsDialog, opened on demand
// via the "Advanced (Developer) Settings" button, so its gamepad rebind
// capture works the same as it does from F4; may be null.
std::unique_ptr<rex::ui::ImGuiDialog> CreateSettingsDialog(
    rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
    std::filesystem::path user_settings_path, std::filesystem::path app_config_path,
    rex::input::InputSystem* input_system = nullptr);

}  // namespace nocturne
