// nocturnerecomp - cycles the in-game "Change Screen Size..." screen's
// stretch through a few preset percentages via the X button (repurposed --
// see graphics_settings.cpp for why), and relabels that screen's "Default"
// prompt to a localized "Presets".
#pragma once

#include <memory>

namespace rex {
class Runtime;
namespace ui {
class ImGuiDrawer;
}  // namespace ui
}  // namespace rex

namespace nocturne {

class GraphicsSettingsToastDialog;  // toast-only ImGui dialog, defined in the .cpp

class GraphicsSettings {
 public:
  GraphicsSettings();
  ~GraphicsSettings();

  // Installs the guest function overrides. Call once runtime() is live
  // (OnPostSetup) -- unlike a mod's OnModuleLaunched, all addresses here are
  // hardcoded rather than looked up via mod_registry, since this is the
  // game's own pinned build, not a third-party mod.
  void Bind(rex::Runtime* runtime);

  // Creates the preset-cycled toast notification dialog. Call once the
  // ImGui drawer is live (OnCreateDialogs).
  void AttachWatcher(rex::ui::ImGuiDrawer* drawer);

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<GraphicsSettingsToastDialog> toast_dialog_;
};

// Process-wide instance shared between the app hooks.
GraphicsSettings& GetGraphicsSettings();

}  // namespace nocturne
