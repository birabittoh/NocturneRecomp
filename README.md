# NocturneRecomp

Static recompilation of **Castlevania: Symphony of the Night** (Xbox Live Arcade) for Windows,
Linux, macOS and Android, built on the [ReXGlue SDK](https://github.com/birabittoh/rexglue-sdk).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64 and arm64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any copyrighted code, data, or assets. You provide your own legally dumped game.

Feel free to visit NocturneRecomp's [official Discord server](https://discord.gg/DJe2pXMH7S) if you need any help.

# Get the game on [Goopie](https://goopie.xyz/#/library/nocturnerecomp)!

## Using a prebuilt release

Get the latest stable build from the [Releases](../../releases/latest) page.

Nightly builds are available from [CI artifacts](https://nightly.link/birabittoh/NocturneRecomp/workflows/ci/main?preview).

| Platform | Download | First run |
|---|---|---|
| Windows | `nocturnerecomp-<tag>-win-amd64.zip` | extract the archive and run `nocturnerecomp.exe` |
| Linux | `nocturnerecomp-<tag>-linux-amd64.tar.gz` / `-linux-arm64.tar.gz` | extract the archive and run `nocturnerecomp` |
| macOS | `nocturnerecomp-<tag>-mac-arm64.dmg` / `-mac-amd64.dmg` | drag the app to Applications; the build is not notarized, so right-click it and choose **Open** the first time |
| Android | `nocturnerecomp-<tag>-android-arm64.apk` | sideload the APK (allow installs from your browser or file manager) |

### The start screen

The game opens on a start screen. Pick your legally dumped XBLA package (the
`LIVE`/STFS file, which has no extension) with **Select File**, then
**Extract**.

Two releases are recognized, and each is installed into its own folder of
the game library. Either one is enough to play, and there is nothing to pick:
with both installed the game uses them together, World's files plus what the
Japanese release has.

| Release | Folder | Adds |
|---|---|---|
| Castlevania: Symphony of the Night (Xbox Live Arcade, World) | `assets/xbla-world` | the base game |
| Dracula X: Nocturne in the Moonlight (Xbox Live Arcade, Japan) | `assets/xbla-jp` | the story movies (opening, castle collapse, ending), the Japanese voices, its own artwork (the Dracula X logo and the PS1 side's text graphics), and its Japanese text when the game language is Japanese |

With both installed, the settings overlay (F4) has two more rows on the
General tab, and both apply without a restart:

- **Force English Voices**: World's voices instead of JP's (off by default).
- **Artwork**: **Auto** (JP's while the language is Japanese, World's
  otherwise), **JP** or **World**.

A JP-only install plays too, with everything coming from the Japanese
release.

The start screen also has a **Mods** page and a **Saves** page (export a
save as a zip, import one into an empty slot, delete). It is skipped once a
version is installed; use **Restart to Start Screen** in the settings
overlay (F4) to come back to it, or set `show_start_screen = true` to see it
on every launch.

## Making it portable

By default saves, options and caches live in your platform user folder
(`Documents/nocturnerecomp` on Windows). To keep them next to the executable
instead, create an empty `portable.txt` beside it.

The root cvars in `nocturnerecomp.toml` (or `--name=value` on the command
line) override each location. Any root left empty falls back to its default.

```toml
game_data_root   = "assets"    # one folder per installed version (default: assets)
user_data_root   = "user"      # saves and profiles (default: Documents/nocturnerecomp)
cache_root       = "cache"     # shader cache (default: <user_data_root>/cache)
mods_data_root   = "mods"      # mod folders (default: mods)
mods_dump_root   = "dumps"     # dumped textures and shaders (default: dumps)
update_data_root = "update"    # extracted title update files (default: update)
```

## Troubleshooting

### Android: the game crashes or will not start

Many Adreno phones ship a Vulkan driver too old for the renderer, and Android
offers no way to update it. The game can load a custom driver instead, such as
Mesa's Turnip drivers, packaged as the same `.zip` files other Android
emulators use.

1. Download a Turnip driver package to the phone.
2. In game, press back and choose **GPU driver...**.
3. Tap **Install...**, pick the `.zip`, then **OK**.
4. Let the game close, then start it again.

If the game never gets far enough to open the menu, start it again after the
crash: when the previous start did not finish, the same dialog appears before
the renderer loads, so you can install a driver or switch back to the system
one. See [docs/custom-gpu-drivers.md](docs/custom-gpu-drivers.md) for details.

## Building from scratch

### 0. Install dependencies

#### Linux (Arch/CachyOS)
```bash
paru -S clang20 cmake ninja vulkan-headers
```

#### Windows
```powershell
scoop install llvm cmake ninja
```

### 1. Clone

```bash
git clone --recursive https://github.com/birabittoh/NocturneRecomp
cd NocturneRecomp
```

If you already cloned without `--recursive`, fetch the submodules with:

```bash
git submodule update --init --recursive
```

### 2. Download the ReXGlue SDK

```bash
python scripts/download-sdk.py --pinned
```

### 3. Provide your game

Place your legally dumped World XBLA package (the `LIVE`/STFS file) into `game/`, then extract it:

```bash
python scripts/extract_game.py
```

This writes the same layout the start screen does: a World package goes into
`assets/xbla-world/`, a JP one into `assets/xbla-jp/` (run it once per
package to install both). World's also makes `assets/default.xex` a copy of
its executable, which codegen and the build read (the guest shaders come from
`assets/xbla-world/MEDIA`), so building needs World even though the game plays
with JP alone. Pass a package path to pick one when `game/` holds several, and
`--force` to replace an existing version folder.

An older flat `assets/` tree still works: the game moves it into
`assets/xbla-world/` the first time it runs.

### 4. Build

```bash
python scripts/build.py
```

Without `--release` the script builds RelWithDebInfo. `--package` builds the
release archive for the host platform; macOS `.dmg` files come from
`scripts/package-mac.py` and the Android APK from the Gradle project in
`android/` (see `.github/workflows/_build.yml` for the exact steps).

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- [MottZilla's Randomizer](https://github.com/MottZilla/SOTN_XB_RANDO/)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.
