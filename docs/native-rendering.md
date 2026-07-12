# Native renderer: architecture, status, and lessons learned

Condensed reference distilled from `native-renderer-headless-boot.md` (the
full session-by-session working log — keep that for forensic detail; this
file is the fast-orientation summary). Goal: replace the general-purpose
xenos GPU plugin with a game-specific Vulkan renderer, since xenos supports
every 360 title and is heavier than SOTN alone needs.

## Status

Renders real, correct game content: KONAMI splash, title screen (moon,
spires, gargoyles, logo), and main menu (background/icons/text) all confirmed
rendering via interactive runs and RenderDoc captures. Built incrementally,
each step verified against a real capture or live run, not just code review.

## Architecture

- **Headless boot** (`config.graphics = nullptr` in `OnPreSetup`, see
  `src/nocturnerecomp_app.h`): the documented "bring your own renderer" path.
  No `GraphicsSystem`/xenos plugin loaded at all.
- **PM4 decode**: `rex::graphics::PacketDisassembler`
  (`rexglue-sdk/src/graphics/packet_disassembler.cpp`) turns ring-buffer/
  indirect-buffer PM4 packets into `PacketAction`s (mostly `RegisterWrite`).
  Duplicated into the always-linked `rexruntime` (alongside `RegisterFile`)
  since it normally only lives in the plugin-only `rexgpu-xenos` build.
  `KernelState::HeadlessWriteRegister` (`rexglue-sdk/src/system/kernel_state.cpp`)
  drives this on every `CP_RB_WPTR` write, follows `PM4_INDIRECT_BUFFER`
  targets (the *real* per-frame command stream lives inside these, not the
  top-level ring), and forwards every decoded packet to a registered
  `native_gpu_command_callback_`.
- **Game-specific interpreter**: `nocturne::NativeCommandProcessor`
  (`src/native_command_processor.{h,cpp}`), registered via
  `KernelState::SetNativeGpuCommandCallback`. Consumes `PacketAction`s into a
  local `RegisterFile`, translates real Xenos microcode (captured from
  `PM4_IM_LOAD`/`_IMMEDIATE`) via `SpirvShaderTranslator`, builds Vulkan
  pipelines/descriptor sets by hand (no `CommandProcessor`/`PipelineCache`
  dependency — deliberately reimplements just enough of the real Vulkan
  backend's layout), and presents through a real swapchain.
- **Presentation**: `NativeImmediateDrawer` (`src/native_immediate_drawer.h`)
  + a `VulkanProvider`/`Presenter` built manually in `OnPreLaunchModule()`
  (same mechanism `GraphicsSystem::SetupPresentation` uses for xenos, wired
  by hand). Frame draws accumulate into an offscreen color target, copied to
  the swapchain's guest-output image on swap.
- **Vertex/texture data**: shaders read vertex data via raw SSBO loads from a
  512MB host-visible buffer mirroring guest physical memory 1:1 — **not**
  `VkVertexInputAttributeDescription` bindings (matches the real backend:
  Xenos shaders always use dynamic/SSBO-based vertex fetch).
- **Descriptor layout**: set 0 = shared-memory SSBO, set 1 = 5 uniform
  buffers (system/float-vertex/float-pixel/bool-loop/fetch constants, binding
  index == `SpirvShaderTranslator::ConstantBuffer` enum value), sets 2/3 =
  separate `SAMPLED_IMAGE`+`SAMPLER` bindings per stage (not
  `COMBINED_IMAGE_SAMPLER`) — currently one texture per stage.

## Key lessons (apply these first before re-deriving from scratch)

1. **Two independent decode paths exist in `rexglue-sdk`; know which one is
   live.** `src/graphics/command_processor.cpp` (the Vulkan/D3D12 plugin
   backend's `CommandProcessor` class) is **dead code** for this renderer —
   a red herring chased at least once. The real path is
   `PacketDisassembler` → `KernelState::HeadlessWriteRegister` →
   `NativeCommandProcessor::OnPacket`.
2. **`PacketDisassembler`/`register_file.cpp` were ported from Xenia in a
   hurry — expect other stub/placeholder cases like the `PM4_LOAD_ALU_CONSTANT`
   one fixed in step 19.** Worth auditing other packet-type cases if a future
   bug smells like "decoded state doesn't match what the guest actually
   wrote."
3. **`PPCContext` is one mutable struct shared by the whole call stack** —
   `lldb frame select N` changes displayed source line, not which register
   values you read; `ctx.r31` always reflects the *innermost* currently
   executing function. To inspect a specific frame's own values, use a
   mid-asm hook at the exact instruction, not `frame select` + register read.
4. **Never edit `generated/*.cpp`** — codegen output, silently
   overwritten-or-kept-stale by `build.py`. Inject guest-side debug logic via
   `[[midasm_hook]]` entries in `nocturnerecomp_config.toml` +
   `src/nocturnerecomp_hooks.cpp` instead (see wiki:
   `../rexglue-sdk-wiki/Mid-ASM-Hooks.md`).
5. **Physical vs. virtual guest addresses matter and are easy to swap.**
   `VdEnableRingBufferRPtrWriteBack`'s pointer, `PM4_INDIRECT_BUFFER`
   targets, and `PM4_LOAD_ALU_CONSTANT`'s address are all *physical*
   (`& 0x1FFFFFFF`, use `Memory::TranslatePhysical`) — using them raw or via
   `TranslateVirtual` causes crashes or garbage decode.
6. **Periodic/banded texture corruption with some regions decoding perfectly
   → an integer-factor pitch/stride bug, not a wrong tiling algorithm**
   (diagnostic signature from step 14: tiled addressing partially aliases
   under an exact-multiple pitch error).
7. **RenderDoc capture recipe**: install's Vulkan hooking must be manually
   enabled once in qrenderdoc Settings (off by default) — this, not
   injection timing, was the blocker for a working capture. Use the
   *officially installed* `renderdoccmd.exe` to launch/inject; a self-built
   `renderdoc.pyd` is fine (only option — installed builds don't ship one)
   for `CreateTargetControl`/`TriggerCapture` against an already-injected
   process. `--fullscreen=false` always. Don't commit `.rdc` files.
8. **`fetch_constant`/register decode reliability**: several bugs across
   this project trace back to "a specific draw's decoded address/constants
   are corrupt or zero" — always verify against a real capture
   (`get_cbuffer_contents`, `disassemble_shader`) rather than trusting the
   log's own claimed values, and check both the *decode* path
   (`PacketDisassembler`) and the *upload* path (native command
   processor's per-draw constant buffer population) independently.

## Bugs found and fixed (chronological, condensed)

| # | Symptom | Root cause | Fix |
|---|---|---|---|
| 1 | Headless boot = total silence | vblank interrupt never pumped (no `GraphicsSystem` worker thread) | `KernelState`-level headless vblank thread + dispatcher (SDK) |
| 2 | Still silent after #1; guest thread spinning 100% CPU | Guest waits on GPU ring read-pointer that nothing advances (no real GPU) | Two `[[midasm_hook]]` bypasses (`HeadlessRingWaitBypass`) at the two wait-loop primitives found via lldb backtraces, gated on `graphics_system() == nullptr` |
| 3 | Headless PM4 decode crashed intermittently / misaligned | Decode cursor tied to per-call wptr delta; speculative packet-body read past a partially-copied buffer | Persistent `headless_ring_buffer_decode_pos_` cursor; always copy a full-ring + max-packet-size padded window |
| 4 | Headless PM4 decode showed only keepalive traffic, no draws | Decoder never followed `PM4_INDIRECT_BUFFER` targets — real per-frame stream lives inside those | Follow IB targets (physical-masked), dedup by (ptr,len), recursion-capped |
| 5 | Nothing rendered — every quad-list/rect-list draw skipped | `kQuadList`/`kRectangleList` aren't native Vulkan primitives | Quad-list: host-synthesized triangle-list index buffer. Rect-list: dedicated `HostVertexShaderType::kRectangleListAsTriangleStrip` translation + triangle-strip w/ primitive restart, replicating real `PrimitiveProcessor` |
| 6 | Transient descriptor pool exhausted once quad-lists actually drew | Pool sized for 64 draws/frame, previously never exercised | Sized for 4096 draws/frame |
| 7 | One shader load took ~20s, repeatedly | Corrupt/garbage-decoded draw + unbounded shader translation | Capped `shader_cache_` at 64 entries; new-beyond-cap shaders dropped (mitigates, doesn't root-cause) |
| 8 | Still nothing rendered, real draws executing per logs | Two compounding bugs, see #9 below | (deep dive step, see next rows) |
| 9 | ″ | `SystemConstants` hardcoded/wrong (`ndc_scale`, flags) instead of register-driven | Real register-driven `TryDraw` computation, matching `draw_util::GetHostViewportInfo`'s two branches |
| 10 | Even hand-injected known-good vertex data rasterized nothing | (ruled out: NDC math, vertex decode, primitive-topology-specific issues — pointed at generic Vulkan-plumbing bug) | Motivated getting RenderDoc working (see lessons #7) |
| 11 | Every vertex fetched from the same address (all draws identical) | `vertex_index_min`/`vertex_index_max` never populated (zero-clamped every index to 0) | Read `VGT_MIN_VTX_INDX`/`VGT_MAX_VTX_INDX` into `SystemConstants` |
| 12 | Still nothing after #11; `shader_discarded` at every pixel | Alpha-test flag bits never set; `0` decodes as "always fail," not "always pass" | Set alpha-test flag bits from `RB_COLORCONTROL`/`RB_ALPHA_REF`, matching real backend |
| 13 | Black squares over blue clear color | 1x1 white placeholder texture — `k_DXT4_5`/tiled formats not yet supported (not a new bug, expected fallback) | (addressed by #14-15) |
| 14 | ″ | No CPU BC1/2/3 decoder existed; tiled addressing unimplemented | `DecompressDXT5Block` (from-spec BC3 decoder) + reused (already-linked, previously uncalled) `texture_util::GetTiledOffset2D` for all formats, not just DXT |
| 15 | Colorful noise band across one texture | DXT tiled pitch used texel-granularity value directly as block-granularity pitch (4x off — block width not divided out) | `pitch_blocks = (fetch.pitch * 32) / 4` (block width), not `fetch.pitch * 32` |
| 16 | No blending — all draws opaque overwrite | `VkPipelineColorBlendAttachmentState` was a bare zero-init struct | Real `RB_BLENDCONTROL0`/`RB_COLOR_MASK` → `VkBlendFactor`/`VkBlendOp` translation, folded into pipeline cache key |
| 17 | One specific quad genuinely renders black | (not a bug — guest explicitly wrote opaque-black vertex color; confirmed via vertex data trace) | N/A |
| 18 | Everything using interpolated vertex data (color/UV) reads zero | `interpolator_mask` left at its "default" (zero) value — no interpolators ever declared as read/written by either shader stage | Compute real mask via `Shader::writes_interpolators()` & `GetInterpolatorInputMask()`, matching the real backend's formula; required splitting shader analysis from translation (`GetOrAnalyzeShader` new, cached independently) since the mask depends on *both* paired shaders |
| 19 | Everything still literally all-black despite #16-18 all being correct fixes | `color_exp_bias` (a final per-RT output multiplier) left at `0.0` instead of the correct default `1.0` — multiplied every draw's final color by zero, unconditionally | Compute `exp2f(RB_COLOR_INFO.color_exp_bias)` per real formula (bit-manipulation of `1.0f`'s IEEE-754 exponent) |
| 20 | Main menu text invisible (background/icons fine) | `PM4_LOAD_ALU_CONSTANT` (the packet type SOTN's UI uses for its dynamic-vfetch fetch constant) never actually read guest memory — hardcoded `0xDEADBEEF` stub left mid-port in `PacketDisassembler` | Threaded an optional `memory::Memory*` through `DisasmPacket`/`DisasmPacketType3`; `LOAD_ALU_CONSTANT` now does a real `TranslatePhysical`+`load_and_swap` read. **Fixed in `rexglue-sdk`, not this repo.** |

## Known open items (as of step 19)

1. Root cause of the pathological ~20s shader-translation stall (#7 above)
   still open — a specific draw's decoded address/size is itself
   garbage/unstable across resubmits; bounded, not fixed.
2. Remaining `SystemConstants` fields still zeroed/default (point sprite
   sizing, user clip planes, gamma conversion, EDRAM/poly-offset) — fine for
   current 2D UI content, will matter for 3D gameplay.
3. Temporary per-draw diagnostic logging (`sysconst draw#N`, `vfetch draw#N`,
   `DebugDumpColorTarget`) is still in `native_command_processor.cpp`,
   clearly marked — low-impact to leave, should eventually be removed or
   `REXCVAR`-gated.
4. `k_16_16_16_16` texture format still unsupported (low priority — rare in
   observed content).
5. The `0x1f6f8000` full-screen EDRAM-resolve target's purpose is still
   unidentified (fires every frame, real but unrelated to bugs chased so
   far).

## Quick reference: build/run/debug

```
# Normal build + run
python scripts/build.py
python scripts/run.py            # NOT ./nocturnerecomp.exe directly —
                                  # run.py supplies required --game_data_root/--license_mask

# Debug build with real guest+SDK symbols (see CLAUDE.md for the DLL-naming
# workaround this currently requires)
cd ../rexglue-sdk && python scripts/deploy-sdk.py --config RelWithDebInfo --project NocturneRecomp
cd ../NocturneRecomp && python scripts/build.py --debug

# SDK changes: edit in ../rexglue-sdk, then redeploy before rebuilding here
cd ../rexglue-sdk && python scripts/deploy-sdk.py --project NocturneRecomp
cd ../NocturneRecomp && python scripts/build.py
```

Logs: `logs/nocturnerecomp_NNN[.n].log` (rotating file sink — short runs'
early lines may land in a `.n` backup file, not the live one). Not
gitignored assumptions aside, `rm -f logs/*.log` before a run keeps things
readable.

RenderDoc MCP tools (`open_capture`, `find_draws`, `get_draw_call_state`,
`get_post_vs_data`, `pixel_history`, `disassemble_shader`,
`get_cbuffer_contents`, `read_texture_pixels`) are the primary tool for
"pipeline looks right on paper but produces wrong output" bugs — see lesson
#7 for the capture recipe.

For full narrative detail, exact code snippets, and session-by-session
reasoning behind each fix above, see `native-renderer-headless-boot.md`.
