// sample_overlay mod - smallest possible mod code-plugin template.
//
// Registers a keybind and a tiny ImGui dialog to prove the mod-plugin ABI
// works end to end: DLL loads, OnCreateDialogs runs, the overlay draws every
// frame. Copy this mod as a starting point for new code mods.

#include <rex/system/mod_plugin.h>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

class SampleOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit SampleOverlayDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    rex::ui::RegisterBind("bind_sample_overlay", "F9", "Toggle sample overlay",
                          [this] { visible_ = !visible_; });
  }

  ~SampleOverlayDialog() override { rex::ui::UnregisterBind("bind_sample_overlay"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }
    ++frame_count_;

    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sample Overlay", &visible_)) {
      ImGui::TextUnformatted("Hello from a code mod!");
      ImGui::Text("Frames drawn: %llu", static_cast<unsigned long long>(frame_count_));
      ImGui::TextDisabled("mods_src/sample_overlay - press F9 to close");
    }
    ImGui::End();
  }

 private:
  bool visible_ = false;
  uint64_t frame_count_ = 0;
};

class SampleOverlayMod : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<SampleOverlayDialog>(drawer);
  }

 private:
  std::unique_ptr<SampleOverlayDialog> dialog_;
};

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new SampleOverlayMod();
}
