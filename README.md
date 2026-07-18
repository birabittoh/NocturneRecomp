# NocturneRecomp

Static recompilation of **Castlevania: Symphony of the Night** (Xbox Live Arcade) for Windows
and Linux, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any copyrighted code, data, or assets. You provide your own legally dumped game.

Feel free to visit NocturneRecomp's [official Discord server](https://discord.gg/DJe2pXMH7S) if you need any help.

# Get the game on [Goopie](https://goopie.xyz)!

## Using a prebuilt release

Using Goopie is preferable, as it makes it trivial to manage the game's assets, versions, mods, achievements, leaderboards, etc.

If you still want to go the hard way, do this:

1. Install Python if you don't have it already
2. Extract the release you just downloaded
3. Create a `game` directory next to the game executable
4. Copy `9F2DAA064D494AA82B43B65362C59E9B89A88F8F58` inside the `game` directory
5. Run `python scripts/extract_game.py` from the release directory
6. Optional: if you also downloaded a title update (TU) package and build, copy it inside `game` and extract it with:

   ```bash
   python scripts/extract_tu.py --base assets/default.xex game/TU_1C42227_000010G000000.00000000000G4
   ```

Finally, run the game executable to play the game.

## Building from scratch (development)

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
python scripts/download-sdk.py
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
# Vanilla
python scripts/build.py

# Title Update
python scripts/build.py --tu /path/to/TU_*
```

### 5. Run

```bash
python scripts/run.py
```

This runs the freshly built executable with the correct CLI arguments
(`--game_data_root=assets`, `--license_mask=1`).

Any extra arguments are forwarded to the executable, e.g.:

```bash
python scripts/run.py --vulkan_device 1
```

## Options

Options can be persisted by adding them to `nocturnerecomp.toml` next to the game executable, for example:

```toml
vulkan_device = 1 # NVIDIA GPU
user_language = 1 # English
```

### Keyboard & mouse

Keyboard and mouse controls are enabled by default. All bindings are overridable in the **F4** menu or `nocturnerecomp.toml`. For example:

```toml
keybind_a = "F"
keybind_left_trigger = "LControl"
mnk_sensitivity = 0.5
```

Mouse sensitivity is controlled by `mnk_sensitivity` (default `1.0`).

### GPU selection

If you have multiple GPUs, you can force a specific one:

```bash
python scripts/run.py --vulkan_device 1
```

List available devices by running the game without the flag.

### Logging

The game writes logs into the `logs` directory by default, but you can configure it.

```bash
python scripts/run.py --log_file nocturne.log --log_level debug
```

## Adding a hook

1. Find the guest address in `default.xex`.
2. Add to `nocturnerecomp_config.toml`:

   ```toml
   [functions]
   0x8XXXXXXX = {name = "MyFunction"}
   ```

3. Implement in `src/nocturnerecomp_hooks.cpp` (create if it doesn't exist, and add it to `CMakeLists.txt`):

   ```cpp
   void MyFunction(PPCContext& ctx, uint8_t* base) {
       // your logic
   }
   ```

4. Re-run codegen and rebuild.

## Adding a midasm hook (inline patch)

```toml
[[midasm_hook]]
address = 0x8XXXXXXX
name = "MyHook"
registers = ["r3"]
return = true
```

Implement in `src/nocturnerecomp_hooks.cpp`:

```cpp
void MyHook(PPCRegister& r3) {
    r3.u32 = 1;
}
```

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.
