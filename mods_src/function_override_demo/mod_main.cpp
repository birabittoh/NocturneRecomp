// function_override_demo mod - worked example of overriding the *behavior*
// of a recompiled guest function at runtime, not just poking data (see
// mods_src/ui_color for the data-poke case).
//
// Wraps the game's leaderboard write-stats driver (guest address published
// by mods_src/game_symbols as "leaderboard.write_stats_fn" -- requires that
// mod, same as ui_color does for data addresses) with a log line, then
// calls through to the original implementation so leaderboard writes still
// happen normally. This demonstrates every piece of the mod-facing API:
//   - looking up the target's guest address via the shared ModRegistry
//     (vanilla/TU-safe, same mechanism as data addresses)
//   - the REX_HOOK_RAW-style raw ctx/base replacement signature
//   - rex::runtime::FunctionDispatcher::OverrideFunction /
//     RestoreFunction, reached via runtime->function_dispatcher()
//   - calling through to the original via the PPCFunc* OverrideFunction
//     hands back, to wrap rather than fully replace
//
// See docs/making-mods.md's "Overriding a recompiled function" section.

#include <rex/system/mod_plugin.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/mod_registry.h>

namespace {

// OverrideFunction is exclusive (see function_dispatcher.h): only one mod
// can hold the override for a given address at a time, so a single global
// slot for "the original we're wrapping" is fine here.
PPCFunc* g_original_write_stats = nullptr;

// Replacement for the guest function driving XSessionWriteStats. Full
// access to guest registers/state, same as an in-repo REX_HOOK_RAW -- the
// only difference is this is installed by a mod DLL after the exe is
// already built, rather than baked in at link time.
extern "C" void FunctionOverrideDemo_WriteStats(PPCContext& ctx, uint8_t* base) {
  REXLOG_INFO("[function_override_demo] leaderboard write intercepted (r3=0x{:08X})",
              static_cast<uint32_t>(ctx.r3.u32));

  // Wrap rather than replace: call through to the original implementation
  // so leaderboard writes still happen normally.
  if (g_original_write_stats) {
    g_original_write_stats(ctx, base);
  }
}

class FunctionOverrideDemoMod : public rex::system::IModPlugin {
 public:
  explicit FunctionOverrideDemoMod(rex::Runtime* runtime) : runtime_(runtime) {}

  // Mirrors ui_color's OnModuleLaunched timing: game_symbols has already
  // published addresses by now (its OnCreateDialogs runs before this,
  // enforced by mod.toml's `requires`), and OverrideFunction itself only
  // needs the target's default table entry to exist, which happens at
  // module registration time, before any of this runs.
  void OnModuleLaunched() override {
    if (!runtime_ || !runtime_->mod_registry() || !runtime_->function_dispatcher()) {
      return;
    }

    auto addr = runtime_->mod_registry()->FindAddress("leaderboard.write_stats_fn");
    if (!addr) {
      REXLOG_WARN(
          "[function_override_demo] leaderboard.write_stats_fn not published "
          "(is game_symbols enabled and ordered first?)");
      return;
    }

    addr_ = *addr;
    if (!runtime_->function_dispatcher()->OverrideFunction(
            addr_, &FunctionOverrideDemo_WriteStats, &g_original_write_stats)) {
      REXLOG_WARN(
          "[function_override_demo] OverrideFunction failed for {:08X} (already "
          "overridden by another mod?)",
          addr_);
      addr_ = 0;
    }
  }

  void OnShutdown() override {
    if (addr_ && runtime_ && runtime_->function_dispatcher()) {
      runtime_->function_dispatcher()->RestoreFunction(addr_, g_original_write_stats);
    }
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  uint32_t addr_ = 0;
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
  return new FunctionOverrideDemoMod(ctx->runtime);
}
