# Making mods

NocturneRecomp mods are folders under `mods/`, layered over the game's data
and, optionally, shipping native code. Two kinds of content can go in a mod,
and a single mod can mix both:

- **Asset replacement** — swap game files, textures, or shaders by mirroring
  the game's own directory layout.
- **Code** — a native DLL that hooks into the app lifecycle (register ImGui
  overlays, keybinds, read guest memory, etc.), via the SDK's mod-plugin ABI.

Mods are enabled in priority order by the `enabled_mods` key in
`nocturnerecomp.toml`; earlier entries win on conflicting files.

## Asset-only mods

An asset mod is just a folder under `mods/<name>/` with any of these
subfolders (all optional — only the ones present are used):

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
replace — e.g. `mods/hdost/game/DATA/sound/bgmusic.wma` replaces
`DATA/sound/bgmusic.wma`. Texture files are named by a 16-hex-digit content
hash (dump one with `texture_dump_enabled = true` in `nocturnerecomp.toml` to
find the hash for a texture you want to replace).

See `mods/hdost/` and `mods/badapple/` for two working examples.

## Code mods

A code mod adds a `code = "<stem>"` key to `mod.toml` and ships a built DLL
at `mods/<name>/code/<stem>.dll`. At startup the SDK loads that DLL through a
versioned C ABI (`rex::system::IModPlugin`) and calls its lifecycle hooks
alongside the game's own overlays — no game-specific addresses required, so
the same DLL works with both the vanilla and title-update builds.

### 1. Scaffold the mod under `mods_src/`

Mod **source** lives in `mods_src/<name>/`, separate from the built/shipped
`mods/<name>/` folder. Copy an existing mod as a template — `mods_src/sample_overlay/`
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
shared library, sets C++23, and links `rex::runtime` — the same shared SDK
runtime the game exe links, so your mod shares its ImGui drawer, keybind
registry, and kernel state rather than getting its own copy.

`mod.toml`:

```toml
manifest_version = 1
name = "Sample Overlay"
version = "1.0.0"
description = "Minimal code-mod template: a keybind (F9) and a tiny ImGui overlay."
code = "sample_overlay"
```

`code` must match the CMake target name (and therefore the built DLL's stem).
Everything else is display metadata shown in the F6 mod manager overlay.

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

All three `IModPlugin` overrides are optional (no-op by default) — implement
only what you use. `ModHostContext` (passed to `rex_mod_create`) gives you
`runtime`, `app_context`, `window`, and `input_system` pointers, plus
`mod_root`/`mod_name` for loading your own bundled assets. Everything else —
registering an overlay, binding a key, reading guest memory — goes through
the same public SDK headers the base game uses (`rex/ui/imgui_dialog.h`,
`rex/ui/keybinds.h`, `rex/system/xmemory.h`, etc.).

The SDK never unloads a mod's DLL once loaded (guest threads may still be
running plugin code at shutdown), so don't rely on static destructors
running at process exit — use `OnShutdown()` instead.

### Example mods to copy from

- **`mods_src/sample_overlay/`** — smallest possible template: one keybind
  (F9), one ImGui window. Start here for a new mod.
- **`mods_src/memory_peek/`** — reads guest memory via
  `runtime->memory()->TranslateVirtual()` for a user-entered address (F10).
  A good reference for anything that inspects live guest state generically
  (no hardcoded addresses).
- **`mods_src/music_player/`** — a full-featured example: owns a persistent
  singleton (`GetAudioPlayer()`), binds in `OnCreateDialogs`, and uses
  `OnModuleLaunched()` to scan the filesystem once KernelState exists.

### 3. Build it

```
python scripts/make_mods.py
```

This configures and builds every `mods_src/<name>/` project against the SDK
in `sdk/` and assembles the result into `mods/<name>/` (copying the built DLL
to `code/<name>.dll`, plus `mod.toml` and `icon.png`). Useful flags:

- `--mod <name>` — build just one mod (repeatable).
- `--package` — also zip each built mod to `mods/<name>.zip` for distribution.
- `--sdk-dir <path>` — point at a different SDK checkout (default: `sdk`).

Asset-only mods (`mods/hdost`, `mods/badapple`) aren't touched by this
script — it only ever writes `mods/<name>/` for names it finds under
`mods_src/`.

### 4. Enable it

Add the mod's folder name to `enabled_mods` in `nocturnerecomp.toml`. Order
matters — earlier entries take priority when multiple mods touch the same
file:

```toml
enabled_mods = "music_player,sample_overlay,memory_peek,hdost,badapple"
```

Then run the game (`python scripts/run.py`) and press **F6** to open the mod
manager overlay — it lists every enabled mod, in load order, with its icon
and a `[code]` badge on mods that loaded a DLL. Check `logs/` if a code mod
doesn't show up loaded; the loader logs the exact reason (missing DLL, ABI
mismatch, missing exports) at startup.

## Both builds, one DLL

The project ships two builds (vanilla and title-update) with different guest
addresses. A code mod built as described above works with both **as long as
it never hardcodes a guest address** — read guest state generically (like
`memory_peek` does) instead. All the sample/extracted mods here follow that
rule, so `make_mods.py`'s output loads unchanged into either build.
