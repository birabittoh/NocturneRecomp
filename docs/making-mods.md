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

Mod **source** and build tooling live in a separate repo,
[birabittoh/NocturneRecomp-Mods](https://github.com/birabittoh/NocturneRecomp-Mods)
(`src/<name>/` there), not here -- this repo only ever contains the
built/shipped `mods/<name>/` folders a player enables. Grab prebuilt mods
from that repo's releases (one zip per mod, with binaries for all three
platforms), or clone it to develop a new one; this doc otherwise describes
the mod-plugin ABI and `mod.toml` format that repo's mods target.

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
  icon.png     shown in the F1 mod manager overlay
```

Files under `game/`/`update/`/`dlc/` mirror the exact guest path they
replace, for example `mods/<mod>/game/DATA/sound/bgmusic.wma` replaces
`DATA/sound/bgmusic.wma`. Texture files are named by a 16-hex-digit content
hash (dump one with `texture_dump_enabled = true` in `nocturnerecomp.toml` to
find the hash for a texture you want to replace).

See the `NocturneRecomp-Mods` repo's `src/` directory for some working examples.

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

### 1. Scaffold the mod under `NocturneRecomp-Mods`'s `src/`

Clone [birabittoh/NocturneRecomp-Mods](https://github.com/birabittoh/NocturneRecomp-Mods);
mod **source** lives in `src/<name>/` there, separate from the built/shipped
`mods/<name>/` folder (which only exists locally after a build, or here in
this repo once you've copied a built mod over). Copy an existing mod as a
template: `src/sample_overlay/` is the minimal one:

```
src/sample_overlay/
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

`rexmod_add_plugin` (from `src/common/mod_cmake/rexmod.cmake`) builds a
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
Everything else is display metadata shown in the F1 mod manager overlay.

`platform` is *written by* `NocturneRecomp-Mods`'s `scripts/make_mods.py`,
not read by it; leave it empty in a fresh mod.toml. After a successful build
it's (re)set to a comma-separated list of whichever platform(s)
`mods/<name>/code/` currently ships a binary for (e.g. `"windows-x64"` after
a `--target windows-x64`-only build, `"windows-x64,linux-x64,linux-arm64"`
once all three have been built into the same tree). It's purely a record of
what's actually on disk, not something you set by hand.

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

- **`src/sample_overlay/`**: smallest possible template: one keybind
  (F9), one ImGui window. Start here for a new mod.
- **`src/memory_peek/`**: reads guest memory via
  `runtime->memory()->TranslateVirtual()` for a user-entered address (F10).
  A good reference for anything that inspects live guest state generically
  (no hardcoded addresses).
- **`src/music_player/`**: a full-featured example: owns a persistent
  singleton (`GetAudioPlayer()`), binds in `OnCreateDialogs`, and uses
  `OnModuleLaunched()` to scan the filesystem once KernelState exists.
- **`src/game_symbols/`**: a *library mod* with no UI of its own;
  publishes reverse-engineered guest addresses into the shared mod registry
  for other mods to depend on. See
  [Library mods and the shared registry](#library-mods-and-the-shared-registry).
- **`src/ui_color/`**: consumes `game_symbols`'s published address
  (`requires = "game_symbols >= 1.0.0"` in its `mod.toml`) instead of
  hardcoding or re-deriving it.
- **`src/function_override_demo/`**: wraps a recompiled guest
  *function*'s behavior at runtime, rather than poking a data field like
  `ui_color`. See
  [Overriding a recompiled function](#overriding-a-recompiled-function).
- **`src/event_ping/`** and **`src/event_pong/`**: a
  producer/consumer pair over the shared registry's event bus rather than
  addresses. `event_ping` has no UI: it uses `RegisterTick` to publish a
  `"sample.ping"` event once a second. `event_pong` (F11) declares
  `requires = "event_ping"`, subscribes to that event, and shows the last
  ping in an overlay -- it also republishes a couple of counters to
  `src/blackboard` with no `requires` on it at all, showing that
  Publish/Subscribe coupling can be looser than the `RegisterAddress`
  pattern.
- **`src/blackboard/`**: a shared key/value store (F12) any mod can
  write to purely by publishing `"blackboard.set"`/`"blackboard.delete"`/
  `"blackboard.clear"` events (bytes = `"key=value"` or `"key"`) -- no header
  or linked symbol needed, so even a binary-only third-party mod can
  participate.
- **`src/bus_inspector/`** (F5): subscribes to the events above and
  logs every one it sees. Since it piles a second/third subscriber onto
  event names `event_pong` and `blackboard` already subscribe to, it
  demonstrates that `Subscribe` supports fan-out to multiple listeners
  rather than last-one-wins.
- **`src/xex_patch_potion/`** and **`src/xex_patch_red_rust/`**:
  two independent, no-UI mods that each patch a different item
  name/description baked into `default.xex`'s static data directly in
  guest memory at startup, and coexist with no conflict. See
  [Patching static game text/data](#patching-static-game-textdata) below,
  and `src/common/include/rexmod/text_patch.h` for the shared helper
  both of them call instead of duplicating the read-only-page-unlock/
  zero-fill logic.

### 3. Build it

```
python scripts/make_mods.py
```

run from `NocturneRecomp-Mods`, not this repo. It configures and builds
every `src/<name>/` project and assembles the result into
`mods/<name>/code/<platform>/` (`<name>.dll` / `lib<name>.so`, plus
`mod.toml` and `icon.png` at the mod root). See that repo's README and the
script's own `--help`/docstring for flags (`--mod`, `--target
{windows-x64,linux-x64,linux-arm64}`, `--package`, `--sdk-dir`) and
cross-build details. Once built, copy `mods/<name>/` into this repo's
`mods/` as-is: `LoadModPlugin` checks `code/<platform>/<stem>...` (matching
the running host) before falling back to a flat `code/<stem>...`, so a mod
folder carrying every platform side by side (as a multi-platform
distribution from that repo's releases does) loads correctly with no
flattening step needed. A locally-built, single-platform mod's flat
`code/<stem>...` still works too.

Prebuilt mods (all three platforms, already zipped one-per-mod) are
attached to that repo's [releases](https://github.com/birabittoh/NocturneRecomp-Mods/releases)
if you just want to install one rather than build it yourself.

### 4. Enable it

Add the mod's folder name to `enabled_mods` in `nocturnerecomp.toml`. Order
matters: earlier entries take priority when multiple mods touch the same
file:

```toml
enabled_mods = "music_player,sample_overlay,memory_peek"
```

Then run the game (`python scripts/run.py`) and press **F1** to open the mod
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
its own, just registration calls in `OnCreateDialogs`. `src/game_symbols/`
is exactly that: it registers `"ui.accent_color"` (the same struct
`accent_color.cpp` reads) for other mods to depend on. `src/ui_color/`
consumes it:

```toml
# src/ui_color/mod.toml
code = "ui_color"
requires = "game_symbols >= 1.0.0"
```

```cpp
// src/ui_color/mod_main.cpp, OnModuleLaunched (lazy lookup, not eager
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
See `src/event_pong/`, `src/blackboard/`, and
`src/bus_inspector/` for the pattern.

**Keybind collisions**: `rex::ui::RegisterBind` auto-resolves collisions
rather than silently shadowing one bind. If two mods both default to the
same key, the later-loaded one (lower `enabled_mods` priority) is moved to
the next free key from a small pool (F5-F12, then F13-F24 as overflow),
logged at WARN, and shown with a "moved" badge in the F1 mod manager. If the
pool is exhausted the bind is left on its requested key but flagged as an
unresolved conflict rather than silently colliding. A key the user has
explicitly set (via config or the F1 overlay's click-to-rebind control) is
never auto-moved. Give every mod's bind both a unique name (it doubles as
the backing CVar name) and, by convention, a unique default key -- the
auto-reassignment is a safety net, not a reason to stop picking distinct
defaults.

The F1 mod manager overlay also lists, per mod, the cvars it defines or
overrides (old -> new value) and flags cvars that two mods have set to
different values, so a silent `SetFlagByName` clash is at least visible --
this is detection only; the underlying cvar is still last-write-wins.

**Overlay visibility, the gamepad overlay menu, and window titles**:
`RegisterBind` takes two optional trailing parameters: `is_visible` -- a
`std::function<bool()>` returning whether the thing this bind toggles is
currently shown -- and `window_title`, the exact string your overlay passes to
`ImGui::Begin` (including any `##id` suffix):

```cpp
rex::ui::RegisterBind(
    "bind_sample_overlay", "F9", "Toggle sample overlay",
    [this] { visible_ = !visible_; },
    [this] { return visible_; },
    "Sample##overlay");
```

Passing `is_visible` costs nothing extra and makes your overlay show up, with
its live shown/hidden state, in two places: the F1 mod manager's per-mod
keybind list, and the gamepad-triggered overlay menu (default **Y**, with an
**Insert** keyboard fallback for controller-less testing; both are ordinary
rebindable binds) that lists every overlay -- vanilla and mod -- with
`is_visible` set, grouped by owner, selectable to toggle without touching a
keyboard.

Passing `window_title` as well makes your overlay fully gamepad-navigable
inside the SDK's two input modes (**Gameplay**, where the pad drives the game
as normal, and **UI**, where it drives the overlays -- toggled by the guide
button, with a **Home** keyboard fallback since guide is frequently
intercepted by Steam/the OS before it reaches the game). In UI mode, one
overlay is "active": left stick/D-pad and A drive ImGui's built-in gamepad nav
inside it, B closes it, Y opens/activates the overlay menu, X cycles the
active overlay among all currently-shown ones, right stick moves its window,
and left-trigger + right stick resizes it. Without `window_title` your overlay
can still be *toggled* from the overlay menu, it just can't be focused, moved,
or resized by the gamepad controller (`rex::ui::GamepadUiController`, see
`gamepad_ui.h`) the way the six base-app overlays are. A bind whose effective
key is a gamepad button name (set via the settings overlay, the mod manager's
rebind control, or by editing `nocturnerecomp.toml` directly) is dispatched on
press while in Gameplay mode only -- in UI mode the gamepad controller owns
the pad for navigation instead, so gamepad-keyed binds don't fire there.

**This requires rebuilding every mod, not just ones adopting `is_visible`/
`window_title`.** `RegisterBind` is a regular (mangled, not `extern "C"`)
exported symbol in `rexruntime(rd).dll`; adding a parameter -- even an
optional one with a default value -- changes that mangled name. Old code
compiles against the new header unchanged (the default fills in the missing
argument), but an **already-built** mod DLL that was never recompiled still
imports the old symbol signature, which no longer exists in the new DLL's
export table, and fails to load (not a graceful skip -- an OS-level
missing-entry-point failure). Rebuild every mod (`make_mods.py`, or the direct
`cmake --build` invocation if you're bypassing it for a config match -- see
"Both builds, one DLL" below) any time you update the SDK, whether or not
you're using anything new it added.

## Overriding a recompiled function

Everything above changes *data* the game reads. `rex::runtime::FunctionDispatcher`,
reached via `runtime->function_dispatcher()`, additionally lets a mod replace
the *behavior* of any recompiled guest function for every caller -- direct
(`bl`) and indirect (`bctrl`/function-pointer) call sites alike -- even
though the exe was already built and linked before the mod loaded.

```cpp
// 1. Look up the target's guest address the same way as a data address --
//    via the shared registry, so the lookup is vanilla/TU-safe. A library
//    mod publishes function addresses with RegisterAddress exactly like
//    data addresses (see src/game_symbols's "leaderboard.write_stats_fn").
auto addr = runtime->mod_registry()->FindAddress("leaderboard.write_stats_fn");

// 2. Replacement signature matches REX_HOOK_RAW: full ctx/base access.
PPCFunc* original = nullptr;
extern "C" void MyReplacement(PPCContext& ctx, uint8_t* base) {
  // ... do something, then optionally call through to `original` to wrap
  // rather than fully replace.
}

// 3. Install it. OverrideFunction hands back the function pointer that was
// active before the override, so a wrapper can call through to it.
runtime->function_dispatcher()->OverrideFunction(*addr, &MyReplacement, &original);

// 4. Undo it (e.g. in OnShutdown()) with the same original pointer.
runtime->function_dispatcher()->RestoreFunction(*addr, original);
```

`src/function_override_demo/` is a complete worked example: it
overrides the game's leaderboard write-stats driver, logs the call, and
calls through to the original so leaderboard writes still happen normally.

A few things that differ from the data-poke case:

- **Overrides are exclusive, not chainable.** If another mod already holds
  the override for an address, `OverrideFunction` fails (returns `false`
  and logs) rather than silently stacking on top of it -- unlike keybind
  collisions (below), which silently shadow. Only the mod that installed
  an override can undo it via `RestoreFunction`, and only by passing back
  the exact original pointer `OverrideFunction` returned.
- **Safe to call anytime**, not just during startup/registration:
  `OverrideFunction`/`RestoreFunction` take the dispatcher's own lock and
  are safe to call from `OnModuleLaunched`, a tick callback, or later. A
  guest thread already inside the old function body when the override
  lands finishes running it; the *next* call to that address sees the
  replacement.
- **The target must already have a default table entry**, which every
  recompiled function gets automatically when its module registers (before
  any mod's `OnModuleLaunched` runs), so in practice this just works for
  any guest function address -- there's nothing to opt in ahead of time.

## Patching static game text/data

Item/enemy names, descriptions, and similar flavor text aren't loaded from
a separate asset file the VFS can overlay -- they're baked into
`default.xex`'s static data (`.rdata`), copied into guest memory once at
startup by the same loader that runs the game's code. Byte layout, text
encoding, and the AES/compression format `default.xex` ships in on disk
are documented in `extracted/README.md`.

Two ways to change one of these strings:

**Replace `default.xex` itself** (`mods/<name>/game/default.xex`).
`XexModule::ReadImage` in the SDK doesn't verify a signature on the base
image load, and accepts a plain, unencrypted/uncompressed XEX2 file
(`scripts/re/rebuild_xex_unencrypted.py` builds one). A mod's
`game/default.xex` **replaces the whole file** with no merging: if two
mods each ship one, `enabled_mods` order picks a single winner and the
other mod's edits are gone, whether or not they touched the same bytes.
Use this only when a mod is the sole thing expected to touch game text,
or needs a structural change a same-length string swap can't do.

**Patch guest memory from a code mod instead**: any number of mods can
each own a different address with no conflict, the same way
`src/ui_color` pokes the accent-color struct. Use
`src/common/include/rexmod/text_patch.h`'s `ApplyTextPatch`
(description fields, plain ASCII) or `ApplyNameFieldPatch` (name fields,
which use a "big first letter" font encoding -- see
`extracted/README.md`) from `OnModuleLaunched()`. Requirements:

1. **The guest address must come from scanning a live, running process**
   (`scripts/re/scan_guest_memory.py`), not from offline-decrypting
   `default.xex` and computing a file offset. A title that ships a
   `default.xexp` (a title-update delta patch -- check the startup log
   for `XEX patch applied successfully`) has that patch applied over the
   base image on every launch, which can shift addresses after the
   patched region by an amount offline decryption of the base file alone
   won't account for.
2. **The target field is very likely in read-only memory.** Writing to it
   in-process access-violates (`0xC0000005`) unless the page is unlocked
   first: `runtime->memory()->LookupHeap(addr)->Protect(addr, size,
   kMemoryProtectRead | kMemoryProtectWrite, &old_protect)` before the
   write, then `Protect(addr, size, old_protect)` after --
   `ApplyTextPatch`/`ApplyNameFieldPatch` already do this.
3. **Field length must be measured from what actually follows the string
   in memory**, not assumed from `len(text) + 1`: some fields end in a
   single null before the next field, some have no terminator at all
   (butting directly against the next field or an entry-end marker), and
   the pattern isn't consistent enough to assume without checking the
   specific field being patched.

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
