# NocturneRecomp

Static recompilation of **Castlevania: Symphony of the Night** (Xbox Live Arcade) for Windows
and Linux, built on the [ReXGlue SDK](https://github.com/birabittoh/rexglue-sdk).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any NocturneRecomp code, data, or assets. You provide your own legally dumped ISO.

## Using a pre-built release

> Nocturne Recomp is available on [Goopie](https://goopie.xyz)!

Get the latest stable build from the [Releases](../../releases/latest) page.

Nightly builds are available from [CI artifacts](https://nightly.link/birabittoh/NocturneRecomp/workflows/ci/main).

Just place the downloaded executable next to the extracted `assets` directory and run it.

## Building from scratch

### 0. Install dependencies

#### Linux (Arch/CachyOS)
```bash
paru -S clang20 cmake ninja vulkan-headers extract-xiso
```

#### Windows
```powershell
scoop install llvm cmake ninja
```

### 1. Clone

```bash
git clone <this-repo-url>
cd NocturneRecomp
```

### 2. Download the ReXGlue SDK

```bash
python scripts/download-sdk.py
```

This downloads the latest nightly and installs it into `sdk/<platform>/`.

### 3. Provide your game

Extract your legally dumped ISO directly into `assets/`:

```bash
extract-xiso -d assets "NocturneRecomp (PAL).iso"
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

### Title update (`--tu`)

The retail title update relocates the whole executable, so it needs its own
recompilation. Pass the TU package(s) (the `TU_*` STFS file the console
downloaded, or a directory of them) to `--tu`:

```bash
python scripts/build.py --tu TU_1C42227_000010G000000.00000000000G4
```

The build script picks the package whose embedded patch matches your
`assets/default.xex` (by signature), stages it as the sibling
`assets/default.xexp` (codegen and the runtime both apply it automatically),
extracts the TU's bundled data into `update/`, and recompiles using
`nocturnerecomp_tu_config.toml` (the patched image's codegen hints — the vanilla
`nocturnerecomp_config.toml` addresses don't apply). `scripts/run.py` mounts
`update/` as the `update:` device automatically, so the same run command works
for both builds. Switching back to a plain `python scripts/build.py` clears the
staged patch and rebuilds the vanilla image.

You can also run the extractor standalone (e.g. to inspect a package):

```bash
python scripts/extract_tu.py --base assets/default.xex TU_*
```

## Options

Options can be persisted by adding them to `nocturnerecomp.toml` next to the game executable, for example:

```toml
vulkan_device = 1 # NVIDIA GPU
user_language = 1 # English
```

### Keyboard & mouse

Keyboard and mouse controls are enabled by default. All bindings are overridable via `nocturnerecomp.toml` (or CLI flags). For example:

```toml
keybind_a = "F"
keybind_left_trigger = "LControl"
mnk_sensitivity = 0.5
```

Mouse sensitivity is controlled by `mnk_sensitivity` (default `1.0`).

### GPU selection

If you have multiple GPUs, you can force a specific one:

```bash
./nocturnerecomp --vulkan_device 1
```

List available devices by running the game without the flag.

### Logging

The game writes logs into the `logs` directory by default, but you can configure it.

```bash
./nocturnerecomp --log_file nocturne.log --log_level debug
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

- [ReXGlue SDK](https://github.com/birabittoh/rexglue-sdk)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.

The recompiled game code produced at build time contains symbols and logic
from NocturneRecomp and is **not** redistributable. Do not share
`default.xex`, the `generated/` directory, or any built binary that links
against them.
