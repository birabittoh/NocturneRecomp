# Making mods

NocturneRecomp mods are folders under `mods/`, layered over the game's data
and, optionally, shipping native code. Two kinds of content can go in a mod,
and a single mod can mix both:

- **Asset replacement**: swap game files, textures, or shaders by mirroring
  the game's own directory layout.
- **Code**: a native DLL that hooks into the app lifecycle (register ImGui
  overlays, keybinds, read guest memory, etc.), via the SDK's mod-plugin ABI.

Mods are enabled in priority order by the `enabled_mods` key in
`nocturnerecomp.toml`; earlier entries win on conflicting files.

## Asset-only mods

An asset mod is just a folder under `mods/<name>/` with any of these
subfolders (all optional; only the ones present are used):

```
mods/<name>/
  game/        overlays the game data partition (game:\ / d:\)
  update/      overlays the update partition
  dlc/<name>/  overlays an installed DLC package
  textures/    texture replacements: <hash16>.dds or .png (flat dir)
  shaders/     shader replacements (DXBC/SPIR-V binaries)
  mod.toml     descriptive metadata (see below)
  icon.png     shown in the F6 mod manager overlay
```

Files under `game/`/`update/`/`dlc/` mirror the exact guest path they
replace, for example `mods/<mod>/game/DATA/sound/bgmusic.wma` replaces
`DATA/sound/bgmusic.wma`. Texture files are named by a 16-hex-digit content
hash (dump one with `texture_dump_enabled = true` in `nocturnerecomp.toml` to
find the hash for a texture you want to replace).

See the `mods_src_` directory for some working examples.

## Code mods

A code mod adds a `code = "<stem>"` key to `mod.toml` and ships a built DLL
at `mods/<name>/code/<stem>.dll`. At startup the SDK loads that DLL through a
versioned C ABI (`rex::system::IModPlugin`) and calls its lifecycle hooks
alongside the game's own overlays.

The project ships two builds: vanilla and title-update (TU), which relocates
the whole image and shifts every guest address, and a single mod DLL has to
work with both. The best way is to avoid hardcoded addresses entirely and
read guest state generically, like `memory_peek` does. When a mod genuinely
needs a specific known address (e.g. poking a particular game setting, like
`ui_color` does), don't hardcode it or re-derive the vanilla/TU split
yourself: look it up by name from the SDK's shared mod registry instead. See
[Library mods and the shared registry](#library-mods-and-the-shared-registry)
below.

### 1. Scaffold the mod under `mods_src/`

Mod **source** lives in `mods_src/<name>/`, separate from the built/shipped
`mods/<name>/` folder. Copy an existing mod as a template: `mods_src/sample_overlay/`
is the minimal one:

```
mods_src/sample_overlay/
  CMakeLists.txt
  mod_main.cpp
  mod.toml
  icon.png
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(sample_overlay LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(sample_overlay
    mod_main.cpp
)
```

`rexmod_add_plugin` (from `mods_src/common/mod_cmake/rexmod.cmake`) builds a
shared library, sets C++23, and links `rex::runtime`, the same shared SDK
runtime the game exe links, so your mod shares its ImGui drawer, keybind
registry, and kernel state rather than getting its own copy.

`mod.toml`:

```toml
manifest_version = 1
name = "Sample Overlay"
version = "1.0.0"
description = "Minimal code-mod template: a keybind (F9) and a tiny ImGui overlay."
code = "sample_overlay"
platform = ""
```

`code` must match the CMake target name (and therefore the built DLL's stem).
Everything else is display metadata shown in the F6 mod manager overlay.

`platform` is *written by* `make_mods.py`, not read by it; leave it empty in
a fresh mod.toml. After a successful build it's (re)set to a comma-separated
list of whichever platform(s) `mods/<name>/code/` currently ships a binary
for (`"windows-x64,linux-x64"` after the default both-platform build,
`"windows-x64"` after a `--target windows`-only one, and so on). It's purely
a record of what's actually on disk, not something you set by hand.

### 2. Implement the plugin ABI

`mod_main.cpp` exports two `extern "C"` functions and returns an
`IModPlugin` subclass:

```cpp
#include <rex/system/mod_plugin.h>

class MyMod : public rex::system::IModPlugin {
 public:
  // Called once, right after the ImGui drawer/overlay stack exists.
  // Register overlays/keybinds here.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}

  // Called once KernelState is fully live (guest module about to launch).
  // Use this for anything needing kernel apps/memory (e.g. filesystem scans).
  void OnModuleLaunched() override {}

  // Called before the host shuts down. Release resources here.
  void OnShutdown() override {}
};

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new MyMod();
}
```

All three `IModPlugin` overrides are optional (no-op by default); implement
only what you use. `ModHostContext` (passed to `rex_mod_create`) gives you
`runtime`, `app_context`, `window`, and `input_system` pointers, plus
`mod_root`/`mod_name` for loading your own bundled assets. Everything else,
including registering an overlay, binding a key, and reading guest memory, goes through
the same public SDK headers the base game uses (`rex/ui/imgui_dialog.h`,
`rex/ui/keybinds.h`, `rex/system/xmemory.h`, etc.).

The SDK never unloads a mod's DLL once loaded (guest threads may still be
running plugin code at shutdown), so don't rely on static destructors
running at process exit; use `OnShutdown()` instead.

### Example mods to copy from

- **`mods_src/sample_overlay/`**: smallest possible template: one keybind
  (F9), one ImGui window. Start here for a new mod.
- **`mods_src/memory_peek/`**: reads guest memory via
  `runtime->memory()->TranslateVirtual()` for a user-entered address (F10).
  A good reference for anything that inspects live guest state generically
  (no hardcoded addresses).
- **`mods_src/music_player/`**: a full-featured example: owns a persistent
  singleton (`GetAudioPlayer()`), binds in `OnCreateDialogs`, and uses
  `OnModuleLaunched()` to scan the filesystem once KernelState exists.
- **`mods_src/game_symbols/`**: a *library mod* with no UI of its own;
  publishes reverse-engineered guest addresses into the shared mod registry
  for other mods to depend on. See
  [Library mods and the shared registry](#library-mods-and-the-shared-registry).
- **`mods_src/ui_color/`**: consumes `game_symbols`'s published address
  (`requires = "game_symbols >= 1.0.0"` in its `mod.toml`) instead of
  hardcoding or re-deriving it.
- **`mods_src/event_ping/`** and **`mods_src/event_pong/`**: a
  producer/consumer pair over the shared registry's event bus rather than
  addresses. `event_ping` has no UI: it uses `RegisterTick` to publish a
  `"sample.ping"` event once a second. `event_pong` (F11) declares
  `requires = "event_ping"`, subscribes to that event, and shows the last
  ping in an overlay -- it also republishes a couple of counters to
  `mods_src/blackboard` with no `requires` on it at all, showing that
  Publish/Subscribe coupling can be looser than the `RegisterAddress`
  pattern.
- **`mods_src/blackboard/`**: a shared key/value store (F12) any mod can
  write to purely by publishing `"blackboard.set"`/`"blackboard.delete"`/
  `"blackboard.clear"` events (bytes = `"key=value"` or `"key"`) -- no header
  or linked symbol needed, so even a binary-only third-party mod can
  participate.
- **`mods_src/bus_inspector/`** (F5): subscribes to the events above and
  logs every one it sees. Since it piles a second/third subscriber onto
  event names `event_pong` and `blackboard` already subscribe to, it
  demonstrates that `Subscribe` supports fan-out to multiple listeners
  rather than last-one-wins.

### 3. Build it

```
python scripts/make_mods.py
```

This configures and builds every `mods_src/<name>/` project and assembles the
result into `mods/<name>/` (copying the built binary to `code/<name>.dll` /
`code/lib<name>.so`, plus `mod.toml` and `icon.png`).

By default it builds **both** platforms, regardless of which OS you run it
on, so `mods/<name>/code/` ends up with both a `.dll` and a `.so`:

- Whichever platform matches your host builds directly with the local
  `clang++`, against a `sdk/win-amd64`- or `sdk/linux-amd64`-style SDK dir
  (falling back to a flat `sdk/` if that's what you already have, for example from
  `../rexglue-sdk/scripts/deploy-sdk.py`, which always deploys flat).
- The other platform is cross-built in Docker: a plain Linux container for
  the `.so`, or (from a non-Windows host) a Linux container cross-compiling
  the `.dll` via `clang-cl` + [`xwin`](https://github.com/Jake-Shadle/xwin)
  against Microsoft's redistributable CRT/SDK, matching the MSVC ABI the
  real Windows SDK build uses. Both SDKs are fetched on demand inside their
  container via `download-sdk.py --pinned`, so the first cross-build of each
  platform is slower (image build + SDK download); later runs are cached.

Useful flags:

- `--mod <name>`: build just one mod (repeatable).
- `--target {windows,linux}`: only build for this platform (repeatable);
  default is both.
- `--package`: also zip each built mod to `mods/<name>.zip` for distribution.
- `--sdk-dir <path>`: SDK root (default: `sdk`); each platform's SDK is
  expected at `<path>/win-amd64` or `<path>/linux-amd64`.

Building either platform via Docker requires `docker` on `PATH`; if it's
missing, the script errors out rather than silently skipping that platform.

Asset-only mods aren't touched by this
script; it only ever writes `mods/<name>/` for names it finds under
`mods_src/`.

### 4. Enable it

Add the mod's folder name to `enabled_mods` in `nocturnerecomp.toml`. Order
matters: earlier entries take priority when multiple mods touch the same
file:

```toml
enabled_mods = "music_player,sample_overlay,memory_peek"
```

Then run the game (`python scripts/run.py`) and press **F6** to open the mod
manager overlay; it lists every enabled mod, in load order, with its icon
and a `[code]` badge on mods that loaded a DLL. Check `logs/` if a code mod
doesn't show up loaded; the loader logs the exact reason (missing DLL, ABI
mismatch, missing exports) at startup.

`mod.toml` also supports three optional dependency fields, each a
comma-separated list of other mods' folder names:

```toml
requires   = "game_symbols"     # must be enabled AND loaded before this mod,
                                 # or the game fails to start
load_after = "some_other_mod"   # soft ordering hint: only warns if violated
conflicts  = "legacy_ui_hack"   # hard error if both this mod and any listed
                                 # one are enabled, regardless of order
```

A missing or misordered `requires` (or a violated `conflicts`) fails the game
at startup with a message naming the mods involved and the fix, instead of
silently loading in a broken state -- if the mod also uses the shared
registry (below) to depend on the other mod's data, this is what actually
guarantees that data exists by the time it's looked up. `load_after` only
warns; it doesn't gate startup.

Each `requires` entry can also pin a minimum version of the mod it names:

```toml
requires = "game_symbols >= 1.0.0"
```

The version is checked against the named mod's own `version` key (dotted
numeric, e.g. `1.0.0`; missing trailing components count as `0`, so `1.0` ==
`1.0.0`). This is a **hard failure** at Setup() only if the enabled
`game_symbols` is actually older. If `game_symbols` has no `version` key at
all (or the constraint itself isn't a valid dotted version), the check can't
be verified either way, so it's accepted with a warning rather than blocking
startup -- this keeps mods and dependencies that predate this feature
working unchanged. A bare `requires = "game_symbols"` (no `>=`) stays
unconstrained.

A mod can similarly require a minimum version of NocturneRecomp itself via
`game_version`, independent of any other mod:

```toml
game_version = "1.0.0"   # or, equivalently: game_version = ">= 1.0.0"
```

This is checked against the build's own version (`nocturnerecomp_app.h`'s
`OnPreSetup` sets it from `src/version.generated.h`, derived from the
nearest `vX.Y.Z` git tag at CMake configure time -- see `CMakeLists.txt`)
and is likewise a hard failure only when the build is actually older; if no
tag is reachable, the version falls back to `0.0.0` and the check is
accepted with a warning instead.

## Library mods and the shared registry

`rex::system::ModRegistry`, reached via `runtime->mod_registry()` from any
mod that holds a `Runtime*`, is a small registry for sharing reverse-engineered
addresses (and generic events) between mods, so that work doesn't have to be
redone, or copy-pasted, by every mod that needs it.

```cpp
// Producer: registers a name once, resolved to a vanilla or TU address
// depending on the running image (no is_patched() check needed by callers).
runtime->mod_registry()->RegisterAddress("ui.accent_color", kVanillaAddr, kTuAddr);

// Consumer: looks the address up by name instead of hardcoding it.
if (auto addr = runtime->mod_registry()->FindAddress("ui.accent_color")) {
  // use *addr
}
```

A **library mod** is a mod that only does this: no UI, no `code` consumers of
its own, just registration calls in `OnCreateDialogs`. `mods_src/game_symbols/`
is exactly that: it registers `"ui.accent_color"` (the same struct
`accent_color.cpp` reads) for other mods to depend on. `mods_src/ui_color/`
consumes it:

```toml
# mods_src/ui_color/mod.toml
code = "ui_color"
requires = "game_symbols >= 1.0.0"
```

```cpp
// mods_src/ui_color/mod_main.cpp, OnModuleLaunched (lazy lookup, not eager
// in OnCreateDialogs -- see "Ordering" below)
if (auto addr = runtime_->mod_registry()->FindAddress("ui.accent_color")) {
  addr_ = *addr;
}
```

`requires = "game_symbols"` (see above) is what makes this safe: the SDK
guarantees `game_symbols` is enabled and ordered before `ui_color`, so the
lookup can't silently return nothing because of a config mistake.

`ModRegistry` also has `Subscribe`/`Publish` for generic events (a payload of
a `uint64_t`, a `double`, and a byte span valid only for the duration of the
`Publish` call), and `RegisterTick`/`DispatchTick` for a callback fired once
per guest frame on GPU swap, useful for anything that needs to react every
frame rather than just at startup or launch. Ticks run on the
command-processor thread, not the render/UI thread.

**Ordering**: producers register in `OnCreateDialogs` (dispatched in
`enabled_mods` order, before `OnModuleLaunched`); consumers look up lazily,
on first use, rather than assuming a specific dispatch order themselves.
`requires` is what actually enforces the producer runs first, not the lookup
site.

**Threading**: `DispatchTick` (and therefore anything a tick callback
publishes) runs on the command-processor thread, not the render/UI thread,
and `Publish` invokes every subscriber synchronously on whatever thread
called it. So a `Subscribe` callback must never touch ImGui directly --
instead copy the payload (including the `bytes` span, which is only valid
for the duration of that one `Publish` call) into a mutex-guarded member,
and render from that snapshot in `OnDraw`, which does run on the UI thread.
See `mods_src/event_pong/`, `mods_src/blackboard/`, and
`mods_src/bus_inspector/` for the pattern.

**Keybind collisions**: `rex::ui::RegisterBind` has no built-in conflict
check. If two mods bind the same key, `ProcessKeyEvent` walks binds in
registration order and only the first match fires and consumes the event --
the later one is silently shadowed, not an error. Give every mod's bind both
a unique name (it doubles as the backing CVar name) and, by convention, a
unique default key.

## Both builds, one DLL

The project ships two builds (vanilla and title-update) with different guest
addresses. A code mod built as described above works with both as long as it
never hardcodes *just one* build's address: either read guest state
generically (like `memory_peek` does), or look the address up from the
shared registry (like `ui_color` does; see
[Library mods and the shared registry](#library-mods-and-the-shared-registry)
above) instead of branching on `is_patched()` itself. All the sample mods
here follow one of those two rules, so `make_mods.py`'s output loads
unchanged into either build.
