# NocturneRecomp

Static recompilation of **Castlevania: Symphony of the Night** (Xbox Live Arcade) for Windows
and Linux, built on the [ReXGlue SDK](https://github.com/birabittoh/rexglue-sdk).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any copyrighted code, data, or assets. You provide your own legally dumped game.

Feel free to visit NocturneRecomp's [official Discord server](https://discord.gg/DJe2pXMH7S) if you need any help.

# Get the game on [Goopie](https://goopie.xyz/#/library/nocturnerecomp)!

## Using a prebuilt release

Get the latest stable build from the [Releases](../../releases/latest) page.

Nightly builds are available from [CI artifacts](https://nightly.link/birabittoh/NocturneRecomp/workflows/ci/main?preview).

Just extract the archive, run the executable and it will prompt you to extract the game.

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
git clone https://github.com/birabittoh/NocturneRecomp
cd NocturneRecomp
```

### 2. Download the ReXGlue SDK

```bash
python scripts/download-sdk.py --pinned
```

### 3. Provide your game

Place your legally dumped XBLA package (the `LIVE`/STFS file) into `game/`, then extract it into `assets/`:

```bash
python scripts/extract_game.py
```

`assets/default.xex` must exist before running codegen.

### 4. Build

Use this script:

```bash
python scripts/build.py
```

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.
