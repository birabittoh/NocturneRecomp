// music_player mod - entry point.
//
// Bridges the F8 audio player (audio_player.cpp/h, moved here unchanged from
// the base game) to the SDK's mod code-plugin ABI. Uses zero hardcoded guest
// addresses -- it drives the emulated Xbox XMP app (app id 0xFA), not the
// guest image -- so this one DLL loads into both the vanilla and TU builds.

#include <rex/system/mod_plugin.h>

#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>

#include "audio_player.h"

namespace {

class MusicPlayerMod : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    nocturne::GetAudioPlayer().AttachDialog(drawer);
  }

  void OnModuleLaunched() override {
    // KernelState/apps are live now; scan for tracks so the player's list is
    // populated by the time the guest starts running.
    auto* ks = rex::system::kernel_state();
    if (!ks || !ks->app_manager()) {
      return;
    }
    auto* xmp = static_cast<rex::kernel::xam::apps::XmpApp*>(ks->app_manager()->FindApp(0xFA));
    if (xmp) {
      xmp->ScanFilesystem();
    }
  }
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
  nocturne::GetAudioPlayer().Bind(ctx->window, ctx->app_context);
  return new MusicPlayerMod();
}
