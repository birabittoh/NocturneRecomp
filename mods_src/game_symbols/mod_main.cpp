// game_symbols mod - a "library mod": no UI, no code of its own to react to
// anything, just published guest addresses other mods can depend on instead
// of re-deriving (or copy-pasting) the same reverse-engineered constants.
//
// Registers every known address in OnCreateDialogs (dispatched before
// OnModuleLaunched, and before any consumer's own OnCreateDialogs runs a
// lazy lookup) via rex::system::ModRegistry, reached through
// ctx->runtime->mod_registry(). Consumers add `requires = "game_symbols"` to
// their own mod.toml (see mods_src/ui_color/mod.toml) so the SDK guarantees
// this mod is enabled and ordered first, instead of relying on convention.
//
// See docs/making-mods.md's "Library mods and the shared registry" section.

#include <rex/system/mod_plugin.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>

namespace {

// Guest addresses of the live Settings -> Accent Color struct (three
// consecutive big-endian uint32 fields, R/G/B, each 0-15)
constexpr uint32_t kAccentAddrVanilla = 0x83173CC8u;
constexpr uint32_t kAccentAddrTU = 0x83173A88u;

class GameSymbolsMod : public rex::system::IModPlugin {
 public:
  explicit GameSymbolsMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* /*drawer*/) override {
    if (runtime_ && runtime_->mod_registry()) {
      runtime_->mod_registry()->RegisterAddress("ui.accent_color", kAccentAddrVanilla,
                                                kAccentAddrTU);
    }
  }

 private:
  rex::Runtime* runtime_ = nullptr;
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
  return new GameSymbolsMod(ctx->runtime);
}
