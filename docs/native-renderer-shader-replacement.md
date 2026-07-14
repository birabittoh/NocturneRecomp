# Native renderer: shader dumping and replacement

Texture replacement is done (see `NativeCommandProcessor::InitializeTextureReplacement`
and the replacement lookup in `GetOrUploadTexture`, `src/native_command_processor.cpp`)
and works with the same `<mod_root>/textures/<hash16>.dds|.png` mods the real
xenos backends use. Shader dumping/replacement is now done too, via a
SPIR-V-only path scoped to this renderer (see "What was implemented" below) —
the rest of this doc is kept as the design record for why it had to be a new
mechanism rather than a small follow-up to the texture work.

## What was implemented

* `NativeCommandProcessor::InitializeShaderReplacement(mod_roots, dump_root)`
  (`native_command_processor.h`/`.cpp`), called from `nocturnerecomp_app.h`
  right next to `InitializeTextureReplacement`, using
  `runtime()->ModOverlayRoots("shaders")` (not `"textures"`) for mod roots.
* Dumping: `GetOrTranslateShader` calls `DumpShaderTranslation` right after a
  successful translation, gated on the SDK's own `shader_dump_enabled` cvar
  (shared with the D3D12/Vulkan xenos backends, so one setting controls
  dumping everywhere). Writes raw SPIR-V words to
  `<dump_root>/shaders/<hash16>.<vert|frag>.native.spv`, once per (hash,
  stage) — distinct filename suffix (`.native.spv`) from the SDK's own
  `.vk.bin.vert`/`d3d12` dumps so the two never collide in the same
  directory. Same directory also gets `<hash16>.ucode.bin.<vert|frag>` and
  `<hash16>.ucode.<vert|frag>` "for free" from `Shader::AnalyzeUcode` (SDK
  code, called for every shader by `GetOrAnalyzeShader` regardless of this
  renderer) — raw pre-translation ucode + disassembly, not SPIR-V. Originally
  those came out as `shader_<HASH16-uppercase>...`; patched
  `rexglue-sdk/src/graphics/pipeline/shader/shader.cpp`'s `DumpUcode` to drop
  the `shader_` prefix and lowercase the hex so every file in this directory
  uses the same `<hash16>` convention (lowercase, no prefix) — redeploy via
  `deploy-sdk.py` if pulling a fresh SDK checkout that predates this. Only
  `DumpUcode` was touched, not `Translation::Dump` (used by the D3D12/Vulkan
  xenos backends' own shader-storage dumps, which this renderer doesn't call
  into) — so xenos-path dump naming is unchanged.
* Replacement: `ApplyShaderReplacement`, called from `GetOrTranslateShader`
  right after dumping, looks for `<mod_root>/<hash16>.spv` across
  `shader_mod_roots_` (first root wins) and, if found, overwrites the real
  translation's `translated_binary()` in place — mirroring
  `PipelineCache::ApplyDxbcReplacement`'s "keep the real translation's
  metadata, only swap the final binary" approach, so `GetOrCreatePipelineLayout`
  still sees the correct texture/sampler binding counts. Gated purely on
  `shader_mod_roots_` being non-empty (no `shader_load_enabled` cvar check —
  that cvar is only defined in the SDK's own `TextureCache` translation unit,
  which this renderer never links in).
* Scoped to the *default* modification only (no tessellation, `kVertex` host
  vertex shader type for vertex shaders): anything else falls through to
  normal translation untouched, with a rate-limited log, per the
  "Modification-sensitivity" complication below. Binding-layout compatibility
  (the "Pipeline layout compatibility" complication below) is left as a
  documentation concern for mod authors, not solved in code, same as
  originally scoped.

## Why this wasn't a small follow-up to the texture work

## Why this isn't a small follow-up to the texture work

The SDK's only shader-replacement implementation is
`PipelineCache::ApplyDxbcReplacement` (`rexglue-sdk/src/graphics/d3d12/pipeline_cache.cpp:1496`),
which swaps in raw **DXBC** bytes keyed by `ucode_hash`, and is **D3D12-only**.
There is no equivalent anywhere in the Vulkan backend
(`include/rex/graphics/vulkan/pipeline_cache.h` only has shader *dump*
plumbing, no `ApplySpirvReplacement`). NocturneRecomp's native renderer is
Vulkan/SPIR-V-only, so there's nothing to reuse the way `rex::graphics::
TextureReplacement` was reused for textures — a real, standalone-enough class
usable by both backends' `TextureCache`. Shader replacement needs a genuinely
new mechanism, most naturally scoped to raw SPIR-V modules rather than DXBC
(which this renderer never produces or consumes).

Decided when this was scoped (see conversation this doc was written from):
build a **new SPIR-V-only replacement path scoped to this renderer**, not a
DXBC→SPIR-V bridge and not an attempt to make existing DXBC shader mods work
here unmodified. If cross-backend shader mod compatibility turns out to
matter later, that's a separate, bigger investigation (spirv-cross reverse
compilation, or asking the SDK to gain a real Vulkan replacement path) — flag
it back to the user rather than assuming it during implementation.

## Hash to key on

Use `Shader::ucode_data_hash()` directly (read, not recomputed) — every hash
consumer in this file (`GetShaderSnapshot`/`GetShaderDetails`, the F2
debugger overlay, `GetOrTranslateShader`'s dump/replacement lookup,
`TryDraw`'s `last_active_*_ucode_xxh3_` tracking) now reads this one field
instead of each recomputing its own hash. It's set once, in
`GetOrAnalyzeShader`, to `XXH3_64bits` over the **raw, guest/big-endian ucode
bytes exactly as captured** — matching the real backend's own
`Shader::ucode_data_hash()` convention (`d3d12/pipeline_cache.cpp:470`,
`vulkan/pipeline_cache.cpp:974`: `XXH3_64bits` over `host_address` straight
from guest memory, *before* any byteswap). That means a mod author can ship
*both* a DXBC replacement (for D3D12/xenos users) and a SPIR-V replacement
(for this renderer) under the same hash, in parallel folders, without the two
schemes colliding — and a hash read off this renderer's F2 overlay is
directly usable to name a `.spv` mod file.

An earlier version of this code got this wrong in a way worth flagging:
`Shader`'s constructor byteswaps `ucode_dwords` into host-native order before
storing it as `ucode_data()` (for the translator's own convenience), so
hashing `shader->ucode_data()` instead of the raw pre-swap bytes silently
produces a *different* hash than the real backend's — even though it's the
same `XXH3_64bits` algorithm. `GetShaderSnapshot`/`GetShaderDetails`/
`last_active_*_ucode_xxh3_` originally did exactly that (hashed
`ucode_data()`), which not only broke parity with the real backend but also
disagreed with this feature's own dump/replacement hash (which correctly
used the raw pre-swap bytes) — so the F2 overlay's displayed hash didn't even
match what `GetOrTranslateShader` was keying dumps/replacements by, in the
same renderer. Fixed by computing the hash exactly once, in
`GetOrAnalyzeShader`, over the right (raw) bytes, and storing it on the
`Shader` object itself so nothing downstream can recompute it wrong again.

## Where to hook it in

`GetOrTranslateShader` (`native_command_processor.cpp:1252`) is the
analogous function to `GetOrUploadTexture` for this: it's the one place that
turns raw ucode into a `VkShaderModule`, called for every vertex/pixel shader
a draw binds, and already caches by a hash that folds in ucode + modification
inputs (host vertex shader type, interpolator mask, tessellation mode) — see
the big comment on `TranslatedShader` in `native_command_processor.h:156-183`
for why those modification inputs are part of the cache key and can't be
skipped.

The natural place for a replacement check is right before the real call to
`SpirvShaderTranslator` inside `GetOrTranslateShader`: hash the raw ucode
(`XXH3UcodeHash`, not the FNV-1a `HashUcode` used for the *cache* key — see
the doc comment on why those two hashes serve different purposes and aren't
interchangeable), look up a replacement SPIR-V module for that hash, and if
found, create the `VkShaderModule` directly from those SPIR-V words instead
of running the translator. If no replacement is found, fall through to the
existing translation path unchanged — same "never make an otherwise-working
shader disappear" rule the texture path follows.

Two real complications specific to shaders, not present for textures:

1. **Modification-sensitivity.** A replacement SPIR-V module has to already
   match this specific draw's interpolator mask / host vertex shader type /
   tessellation mode, or binding layout mismatches will crash validation or
   silently misrender. Realistically this narrows initial support to the
   *default* modification (kVertex, no tessellation) — skip (fall through to
   translation) for anything else, at least initially, with a rate-limited
   log matching this file's existing "skip unsupported case" pattern (see
   `GetOrUploadTexture`'s format allow-list logging for the idiom to copy).
2. **Pipeline layout compatibility.** `GetOrCreatePipelineLayout`
   (`native_command_processor.h:243`) builds descriptor set layouts from
   *this renderer's own* introspection of `SpirvShaderTranslator`'s output
   (texture/sampler counts, binding indices — see the big comment above that
   declaration). A hand-authored or third-party-compiled replacement SPIR-V
   module won't carry that same metadata unless its author replicates the
   exact binding convention `SpirvShaderTranslator` uses (set 0 = shared
   memory SSBO, set 1 = 5 constant UBOs by `ConstantBuffer` enum value, sets
   2/3 = per-stage texture/sampler bindings — see the class comment atop
   `NativeCommandProcessor` in `native_command_processor.h`). This needs to be
   documented for mod authors, not solved in code — there's no way to infer
   binding counts from an opaque `.spv` blob without also disassembling it
   (`SPIRV-Tools` is already linked, via `spirv-tools/libspirv.h`, so
   reflection is possible if this needs to be made robust later).

## Wiring convention to match

Mirror the texture path exactly for the mod-discovery side:
`runtime()->ModOverlayRoots("shaders")` (not `"textures"`) gives mod roots in
priority order, same as `InitializeTextureReplacement`'s call site in
`nocturnerecomp_app.h`. Add an analogous
`NativeCommandProcessor::InitializeShaderReplacement(mod_roots, dump_root)`
called right next to the existing `InitializeTextureReplacement` call. Since
there's no SDK class to reuse (unlike `rex::graphics::TextureReplacement`),
this likely means a small renderer-local replacement index (hash → file path,
`<mod_root>/<hash16>.spv`, first root wins per hash — same priority-ordering
rule the texture pipeline uses) rather than a new SDK-side class, unless a
second consumer for it materializes later.

## Sanity-check the fix the same way the texture one was found broken

The texture replacement path initially looked wired up correctly (mod roots
discovered, one replacement indexed, log lines present) but silently missed
in practice: `GetOrUploadTexture`'s hand-rolled `source_size_bytes` estimate
(pitch×height, including last-row padding) didn't byte-for-byte match the
real backend's `TextureKey::GetGuestLayout().base.level_data_extent_bytes`
(the actual tiling/alignment-aware upper bound — see
`texture_util::GetGuestTextureLayout`, `rexglue-sdk/src/graphics/pipeline/
texture/util.cpp:202`), so the content hash was computed over a
slightly-wrong byte range and never matched what real dumps/mods were keyed
against. The fix was to call that exact same layout function instead of
reimplementing the size math. Expect an equivalent trap for shader hashing:
`XXH3UcodeHash`'s byte count must exactly match `ucode_byte_count` as the
real backend computes it (guest instruction count × word size, endian-swapped
consistently) — verify by hashing a shader you can also inspect via the F2
debugger overlay (already wired up in this renderer, see
`GetShaderSnapshot`/`GetShaderDetails`) and confirming the two hashes agree,
the same cross-check this renderer's own doc comments call out as already
proven for that overlay.
