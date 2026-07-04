# Dumping and replacing assets

This covers the texture and shader dump/replace workflow in detail: the
cvars, the dump file layout, and how to turn a dumped file into a mod. For
the mod folder layout itself (`mods/<name>/textures/`, `enabled_mods`,
`mod.toml`, etc.), see [making-mods.md](making-mods.md) first; this doc
assumes you already know how asset-only mods are structured.

All of this is generic SDK functionality (`sdk/include/rex/graphics/...`),
not game-specific code; it works the same regardless of which title the SDK
is compiled for.

## Cvars

All of these go in `nocturnerecomp.toml`:

```toml
texture_dump_enabled = true
texture_dump_skip_sizes = "512x256,640x360,1280x720"
texture_dump_format = "png"
shader_dump_enabled = true
texture_replace_enabled = false
```

| cvar | default | purpose |
|---|---|---|
| `texture_dump_enabled` | `false` | dump every texture the game uploads to `dumps/textures/` |
| `texture_dump_format` | `"dds"` | `"dds"` (lossless, raw compressed blocks) or `"png"` (software-decompressed RGBA8) |
| `texture_dump_skip_sizes` | unset | comma-separated `WxH` list to skip dumping (cuts noise from UI/font atlases, solid-color fills, etc.) |
| `texture_replace_enabled` | `true` | whether mod texture overrides are applied at all |
| `shader_dump_enabled` | `false` | dump every shader the game compiles/uses to `dumps/shaders/` |
| `shader_load_enabled` | `true` | whether mod shader overrides (DXBC/SPIR-V) are applied |
| `mods_dump_root` | unset | override where dumps are written (default: `<exe folder>/dumps`) |

Toggling `texture_dump_enabled` or `shader_dump_enabled` requires a restart:
uncomment the line, run `python scripts/run.py`, and play through the
content you want to capture.

## Dumping textures

With `texture_dump_enabled = true`, every texture the game uploads gets
written to `dumps/textures/` as:

```
<hash16>_<width>x<height>_<format>.dds   (or .png)
```

`<hash16>` is a 16-hex-digit XXH3 hash of the raw guest texture data
(untiled, still in its native big-endian layout); this is the same hash
mods use to key texture replacements, so a dumped filename tells you
directly what to name your replacement.

- **DDS mode** (default) is lossless: compressed formats (BC1/BC3/BC5/etc.)
  are written out as their raw blocks under a standard FOURCC DDS header, so
  the file round-trips exactly and opens fine in most texture tools.
- **PNG mode** software-decompresses BC1/BC3/BC5 to RGBA8 before writing;
  handy for editing in a normal image editor, but not a byte-exact dump.

Use `texture_dump_skip_sizes` to filter out sizes you don't care about (e.g.
`512x256,640x360,1280x720` for common UI/fullscreen buffer sizes) so
`dumps/textures/` doesn't fill up with things you'll never replace.

## Replacing textures

Drop a replacement into a mod's `textures/` folder, named by the same
16-hex-digit hash as the dump:

```
mods/<name>/textures/<hash16>.dds
mods/<name>/textures/<hash16>.png
```

- DDS replacements can be any power-of-two resolution; you're not limited
  to the original size.
- PNG replacements are loaded as RGBA8 (via stb_image).
- `texture_replace_enabled` must be `true` (it is by default) for any of
  this to take effect.
- If multiple enabled mods ship a replacement for the same hash, the one
  from the mod earliest in `enabled_mods` wins.
- Mods are re-scanned when the mod list changes, so you can add/replace
  files in a mod's `textures/` folder and toggle mods without restarting the
  hash index (though the game itself needs to re-touch the texture to see
  the new file).

## Dumping shaders

With `shader_dump_enabled = true`, `dumps/shaders/` fills up with, per
shader:

- `shader_<ucode_hash>.ucode.vert` / `.frag`: disassembled Xenos microcode,
  human-readable.
- `shader_<ucode_hash>.ucode.bin.vert` / `.frag`: the raw microcode binary.
- `shader_<hash>_<backend>.vk.bin.vert` / `.d3d.bin.vert`: the
  backend-translated shader binary actually bound to the pipeline (SPIR-V or
  DXBC depending on which graphics backend is active).

`dumps/shaders.toml` accumulates one entry per shader you've seen, keyed by
the ucode hash:

```toml
[<ucode_hash_hex>]
name = "<name>"
disabled = false
```

You can hand-edit `name` to label shaders as you identify them, and flip
`disabled = true` to force a shader off (useful for isolating which draw
call a visual effect comes from).

### Shader Debugger overlay

If you're doing this interactively rather than by hand-editing
`shaders.toml`, the in-game Shader Debugger overlay lists shaders live,
lets you toggle `disabled` per-shader, and can hot-patch a shader's
translated binary in place (recompiling from edited HLSL via D3DCompile, or
loading a raw translated binary) without restarting; useful for iterating
on a replacement before you commit it to a mod's `shaders/` folder.

## Replacing shaders

Drop the backend-translated binary into a mod's `shaders/` folder, named by
the shader hash used in the dump filename:

```
mods/<name>/shaders/<hash>_<backend>.dxbc   (D3D12)
mods/<name>/shaders/<hash>_<backend>.spv    (Vulkan)
```

The first mod root (in `enabled_mods` order) with a matching file wins.
`shader_load_enabled` must be `true` (default) for overrides to be picked
up.

Because the replacement format is the *compiled* shader binary (DXBC/SPIR-V),
not source, the practical workflow is: dump, edit the disassembly or patch
via the Shader Debugger overlay to get something working, then export/copy
the resulting binary into your mod's `shaders/` folder under the original
hash name.

## Other assets (files, sound, etc.)

Anything that isn't a texture or shader, such as game files, DLC content, or audio,
is replaced by mirroring the guest path under a mod's `game/`, `update/`,
or `dlc/<name>/` folder, as covered in
[making-mods.md](making-mods.md#asset-only-mods). There's no separate
"dump" step for these: the guest path is already whatever's on the game
disc/partition, so you get the original file straight from the extracted
game data rather than from a runtime dump.

## Packaging a dump into a mod

None of `scripts/make_mods.py`, `build.py`, or `run.py` touch asset-only
mods; asset replacement is just files on disk read directly by the SDK's
runtime. To turn a dump into a shareable mod:

1. Copy the dumped/edited file into `mods/<name>/textures/` or
   `mods/<name>/shaders/`, keeping the hash-based filename.
2. Add a `mod.toml` (see making-mods.md) and optional `icon.png`.
3. Add `<name>` to `enabled_mods` in `nocturnerecomp.toml`.
4. `python scripts/run.py`, then press **F6** to confirm the mod loaded.

If you want to distribute it, zip the `mods/<name>/` folder the same way
`make_mods.py --package` does for code mods; asset mods just don't need
building first.
