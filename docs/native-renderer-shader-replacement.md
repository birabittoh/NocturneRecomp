# Native renderer: wiring up shader replacement (not yet done)

Texture replacement is done (see `NativeCommandProcessor::InitializeTextureReplacement`
and the replacement lookup in `GetOrUploadTexture`, `src/native_command_processor.cpp`)
and works with the same `<mod_root>/textures/<hash16>.dds|.png` mods the real
xenos backends use. Shader replacement is not — this is what's left, and why
it's a materially different problem.

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

Use the same hash the F2 shader debugger overlay already computes and
displays: `XXH3UcodeHash` (`native_command_processor.cpp:1190`), an XXH3 hash
over the raw big-endian ucode dwords. This is deliberately the same
convention as the real backend's `Shader::ucode_data_hash()`
(`d3d12/pipeline_cache.cpp:470`, `XXH3_64bits` over `ucode_dwords.data()`,
`ucode_byte_count` bytes) — confirmed in this renderer's own code comment at
`native_command_processor.h:62-70` ("a hash from the old xenos-plugin
renderer's F2 overlay is directly comparable to one reported here"). That
means a mod author could plausibly ship *both* a DXBC replacement (for
D3D12/xenos users) and a SPIR-V replacement (for this renderer) under the
same hash, in parallel folders, without the two schemes colliding.

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
