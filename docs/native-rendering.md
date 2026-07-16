# Native renderer

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

## Key lessons

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
