# Smoke invisibility investigation — RESOLVED (2026-07-14)

## Actual root cause: hardware tessellation

The title-screen smoke is **not a plain quad — it's a tessellated
4-control-point quad patch**. The draw's `VGT_DRAW_INITIATOR.major_mode` is
explicit and `VGT_OUTPUT_PATH_CNTL.path_select` selects `kTessellationEnable`,
so the guest "vertex shader" is really a **quad-domain domain shader**: on real
Xenos it receives the tessellation coordinates in `r0.xy` and the 4 control
point indices in `r0.z`/`r1.xyz` (`kQuadDomainCPIndexed`), then vfetches each
control point itself and warps a ~600-vertex grid (the wavy mist motion).

Translating that ucode as a plain vertex shader (index in `r0.x`) made every
invocation read control point index 0 (`r0.z`/`r1.z` are zero-initialized on
the host), collapsing all vertices onto vertex 0 — a zero-area primitive that
rasterizes nothing, deterministically, live and under capture. This is also
why the xenos plugin only shows the smoke on its **D3D12** backend (which has
hull shaders and the `XEVERTEXID` passthrough VS) and not on Vulkan: the SDK's
Vulkan backend never got the tessellation plumbing.

**The decisive evidence** was a RenderDoc capture of the xenos D3D12 backend
(`smoke_xenos.rdc`): the smoke draw has hull+domain shaders bound,
4-control-point patch topology, and 600 post-tessellation vertices.

## The fix (this repo, `native` branch)

1. `nocturnerecomp_app.h`: create the Vulkan provider with
   `with_gpu_emulation=true` so `VulkanDevice` enables the
   `tessellationShader` device feature.
2. `native_command_processor.{h,cpp}`:
   - Mirror `VGT_DRAW_INITIATOR` into the register file in `OnDraw` (the PM4
     decode path never routed it through a `RegisterWrite` action).
   - Detect tessellated quad-list draws (explicit major mode +
     `kTessellationEnable` + discrete/continuous `VGT_HOS_CNTL.tess_mode`).
   - Translate the guest VS with
     `HostVertexShaderType::kQuadDomainCPIndexed` (+ tessellation_mode for
     the spacing execution mode) — the SDK's `SpirvShaderTranslator` already
     supports emitting it as a SPIR-V tessellation *evaluation* shader.
   - Run it through a real 4-stage pipeline: a hand-assembled passthrough VS
     (`gl_VertexIndex` → float at location 0), a hand-assembled quad TCS that
     gathers the patch's 4 control point indices into the per-patch
     `xe_in_patch_control_point_indices` input the TES expects and sets all
     tessellation levels from a push constant (`VGT_HOS_MAX_TESS_LEVEL + 1.0`,
     pushed per draw), `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST` with
     `patchControlPoints=4`, drawn non-indexed.
   - **Removed the "NaN-w fix" below — it was actively wrong.** In this
     domain shader's vertex layout, dword 3 is the packed ubyte4 vertex
     *color* (`0xFFFFFFFF` = opaque white), not `position.w`. Overwriting it
     with the bytes of 1.0f recolored the smoke to (128,0,0,63)/255, which
     after the draw's `src*(1-dstRGB) + dst` blend changed pixels by <1% —
     invisible. (Also: the blend-equation theory below misdecodes
     `RB_BLENDCONTROL 0x01090109` — bits [12:8]/[28:24] are `kOne`, not
     `kOneMinusDstColor`; it's a screen-style additive blend and was never
     the problem.)
   - Decode the texture fetch constant's `clamp_x/y/z` into per-draw cached
     samplers (`GetOrCreateSampler`) — the smoke scrolls U past 1.0 and needs
     `kRepeat`; the old fixed clamp-to-edge sampler smeared the last texel
     column across the seam instead of looping.

Everything below this line is the historical investigation log, kept for
reference. Its two "root cause" claims (NaN `position.w`, blend equation) are
**superseded** by the tessellation finding above.

---

# (historical, superseded) Blend-equation theory (2026-07-14)

## Summary

The NaN-`w` vertex fix (see "ROOT CAUSE FOUND" section below) makes the smoke
quad's geometry valid — vertices project to on-screen NDC, the draw reaches
`vkCmdDrawIndexed`, and the texture is non-zero. **But the smoke remains
invisible.** The root cause is now identified as a **blend equation problem**:
the smoke's `(src + dst) * (1 - dst)` blend produces alpha=0 (and severely
attenuated RGB) when the destination already contains opaque content from
prior draws.

## What was done

1. **NaN-w fix**: always-on SSBO patch that overwrites `position.w = 0xFFFFFFFF`
   (NaN) with `1.0f` for every smoke draw (keyed on the 852×128 texture). This
   is confirmed working — vertices now project correctly to NDC.

2. **SDK NaN guard is redundant**: `spirv_translator.cpp:2418-2425` already
   catches NaN `w` in `gl_Position` and forces 1.0. The SSBO fix is technically
   unnecessary but harmless.

3. **Extensive diagnostic logging** added to `native_command_processor.cpp`:
   - `SMOKE_VERT`: per-vertex position/uv/pad for each smoke draw
   - `SMOKE_STATE`: full register dump (clip, vte, blend, depth, surface, etc.)
   - `SMOKE_FIX`: w-verification log (confirms SSBO patch is applied)
   - `SMOKE_TEX`: first 8 dwords of smoke texture from guest physical memory
   - `SMOKE_SYSCONST`: ndc_scale/ndc_offset/color_target dimensions
   - `SMOKE_DRAW_EXEC`: confirms draw reaches vkCmdDrawIndexed

## Current root cause: blend equation kills alpha

### The blend state

```
blend0 = 0x01090109
```

Decoded (per `RB_BLENDCONTROL` union in `registers.h:779`):
- Bits [4:0]   = color_srcblend = 9 = `kOneMinusDstColor`
- Bits [7:5]   = color_comb_fcn = 0 = `kAdd`
- Bits [12:8]  = color_destblend = 9 = `kOneMinusDstColor`
- Bits [20:16] = alpha_srcblend = 9 = `kOneMinusDstColor`
- Bits [23:21] = alpha_comb_fcn = 0 = `kAdd`
- Bits [28:24] = alpha_destblend = 9 = `kOneMinusDstColor`

All 6 factors are `kOneMinusDstColor`, all 3 ops are `kAdd`.

### Vulkan mapping

Per Vulkan spec, `VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR`:
- For RGB: factor = `(1-R_d, 1-G_d, 1-B_d)` — uses destination RGB
- For Alpha: factor = `1-A_d` — uses destination alpha

Blend equation (kAdd):
```
output = src * (1-dst) + dst * (1-dst)
       = (src + dst) * (1 - dst)
```

Per-component for both RGB and alpha.

### Why the smoke is invisible

The framebuffer starts each frame cleared to `(0.05, 0.05, 0.35, 1.0)` at
line 2892-2893 of `native_command_processor.cpp`. Then 5955 draws before the
first smoke draw (#5956) write opaque background content into the same
framebuffer. There are **zero EDRAM resolve copies** in the log — the entire
frame (background + smoke + HUD) renders into a single Vulkan render target.

When the smoke draws, the destination already contains opaque content
(alpha ≈ 1.0 from prior draws or the clear value). The blend computes:

```
output_a = (src_a + dst_a) * (1 - dst_a)
```

With `dst_a ≈ 1.0`:
```
output_a = (0.447 + 1.0) * (1 - 1.0) = 0.0
```

The alpha channel is zeroed. The A2B10G10R10_UNORM format stores this as
2-bit alpha = 0 (fully transparent).

For RGB, the output is also severely attenuated:
```
output_rgb = (src_rgb + dst_rgb) * (1 - dst_rgb)
```

If the background at the smoke's location (y=0–184) is bright (e.g. 0.8),
`output ≈ (0.45 + 0.8) * 0.2 = 0.25` — dim but technically non-zero.
Against a white background (dst=1.0), output = 0.

### Why it works on Xbox 360

On real hardware, the game would:
1. Draw the background to EDRAM → resolve to a texture in main memory
2. Clear/switch EDRAM tiles → fresh surface with alpha=0
3. Draw smoke on the cleared surface → `dst=0` → blend simplifies to `src * 1 = src`
4. Resolve smoke → composite background + smoke using standard alpha blend

The recompiler doesn't model EDRAM surface switching. Everything goes into
one framebuffer. The smoke blend is designed for `dst=0` but sees `dst=opaque
background`.

## Smoke draw details (from log 008)

### Vertex positions (all smoke draws identical)

```
v0: pos=(1.0, 0.0, 1.0, NaN) uv=(0.001087, 0.003906)
v1: pos=(1279.0, 0.0, 1.0, NaN) uv=(1.001087, 0.003906)
v2: pos=(1279.0, 184.0, 1.0, NaN) uv=(1.001087, 1.003906)
v3: pos=(1.0, 184.0, 1.0, NaN) uv=(0.001087, 1.003906)
```

Full screen width (1–1279), 184px tall at top of screen. UVs scroll
horizontally across frames (smoke animation).

### Render state

```
clip=00090000     clip_disable=1, dx_clip_space_def=1
vte=00000400      vtx_w0_fmt=1 (WNotReciprocal), all vport enables OFF
su_sc_mode=00010006  cull_back=1, face=0 (CCW front), vtx_window_offset_enable=1
blend0=01090109   ALL factors=kOneMinusDstColor(9), ALL ops=kAdd(0)
colorctrl=8700000C  alpha_test_enable=1, alpha_func=kGreater(4), ref=0.0
depthctl=00700734   depth test disabled (z_enable=0), z_write_enable=1
surface=14000500   surface_pitch=1280
scissor_tl=00000000  scissor_br=20002000  (no clipping)
vtx_cntl=00000004   pix_center=kD3DZero, quant_mode=1
sq_prog=10200E0E
color_exp_bias=0  → exp2(0) = 1.0 (no bias)
alpha_ref=00000000 → 0.0f (any alpha > 0 passes test)
```

### Sysconst values

```
ndc_scale=(0.001563, 0.002778, 1.000000)
ndc_offset=(-0.999219, -0.998611, 0.000000)
color_target_w=1280  color_target_h=720
```

Manual NDC projection confirms vertices are on-screen (NDC within [-1,1]).

### Texture data

```
phys=1EBB8000  size=0x6C000  format=BGRA8  dims=852×128×4
d0=93727272 d1=93717171 d2=93707070 d3=93707070
d4=95757575 d5=95747474 d6=95727272 d7=95727272
```

First pixel: B=0x93 G=0x72 R=0x72 A=0x72 → alpha ≈ 0.447 (non-zero).
Texture data is non-zero; alpha test (kGreater ref=0.0) passes.

### Draw execution

`SMOKE_DRAW_EXEC` fires for every smoke draw (#1 through #93+), confirming
the draw reaches `vkCmdDrawIndexed` — no silent `return` paths kill it.

### Frame timing

- 93+ smoke draws over ~1.8 seconds (23:16:49.548 to 23:16:51.324)
- Draws are interleaved with non-smoke draws (draw numbers ~23 apart:
  5956, 5979, 6002, 6025, ...)
- Non-smoke draws between smoke draws are HUD elements (169×41, 300×160,
  692×44, 256×512, 26×26 textures)

## What does NOT explain the invisibility

- **Vertex positions**: valid, on-screen, correct NDC projection
- **Face culling**: face=CCW front, both smoke triangles are CCW in clip
  space → front faces → NOT culled (Vulkan Y-flip handled by SDK viewport)
- **Alpha test**: ref=0.0, func=kGreater → any alpha > 0 passes → NOT a blocker
- **Depth test**: disabled → NOT blocking
- **Draw execution**: reaches vkCmdDrawIndexed → NOT silently skipped
- **Texture data**: non-zero, non-zero alpha → shader outputs non-zero color
- **SSBO w-fix**: confirmed working, vertices project correctly

## Next steps

### Approach A: Clear framebuffer before smoke pass (targeted)

Before the first smoke draw, issue `vkCmdClearAttachments` to clear the
color attachment to `(0,0,0,0)`. This simulates the game's fresh EDRAM
surface. The smoke blend would then see `dst=0` and produce `output=src`.

**Risk**: destroys background content below y=184. But proves the theory —
if smoke becomes visible, the blend is confirmed as root cause.

**Location**: In `TryDraw`, after `current_draw_is_smoke` is set to true
and before `draw_with_synthesized_indices` (around line 3744). Gate on
a `static bool` to clear only once.

### Approach B: Track EDRAM surface switching (correct fix)

When the game changes `RB_COLOR_INFO.color_base` (indicating a switch to a
new EDRAM tile/surface), clear `color_target_image_`. This models the real
Xbox 360 rendering pipeline correctly.

**Requires**: Adding RB_COLOR_INFO logging to SMOKE_STATE, monitoring
color_base changes between draws, implementing `vkCmdClearColorImage` or
equivalent at surface switches.

### Approach C: Modify blend for smoke draws (shader-level)

Replace the blend with `src*1 + dst*0` (opaque) for smoke draws, making
the smoke render at full intensity regardless of destination. This doesn't
match Xbox 360 behavior exactly but would make the smoke visible.

### Recommended order

1. Try **Approach A** first (quick diagnostic, proves theory)
2. If theory confirmed, implement **Approach B** (correct architectural fix)
3. **Approach C** as a fallback if A/B are too invasive

---

# ROOT CAUSE FOUND (2026-07-13): NaN vertex position.w, NOT vertex-fetch zeroing

**The original investigation below chased a red herring.** The "vertex fetch
comes back all-zero / degenerate" symptom only ever reproduces *under RenderDoc
capture or injected per-draw latency* -- it is a capture/timing artifact, not
the reason the smoke is missing in normal gameplay. Proven this session:

- In a **normal, uninstrumented run** the smoke draw (`fetch_constant=95`,
  `size=32`, a 4-corner quad at screen-space `(232,54)-(1048,666)`) is emitted
  **every frame with fully valid geometry** (`degenerate=false`), its SSBO
  mirror matches guest RAM exactly (`ssbo_mismatch=false`), and its render
  state is normal (blend `07060706` = standard src-alpha/inv-src-alpha; color
  mask `F`; alpha-test GREATER ref 0; depth test disabled). The smoke texture's
  alpha is ~0.75 (not zero). Nothing about geometry, sync, blend, or alpha
  explains the invisibility.
- The one anomaly: the vertex **position.w is `0xFFFFFFFF` (NaN)**
  (`guestV0=(43680000,42580000,3F800000,FFFFFFFF)` = x=232, y=54, z=1, w=NaN).
  The smoke vertex shader feeds `gl_Position.w` from this w and computes `1/w`
  in its epilogue, so `gl_Position` becomes NaN -> the primitive is clipped ->
  invisible.
- **Decisive experiment**: patching each of the 4 vertices' w to `1.0` in the
  SSBO mirror right before the draw (env `NOCTURNE_SMOKE_FIX_W=1`, see
  `native_command_processor.cpp`) makes the smoke **render** (confirmed
  visually: the dark scrolling mist appears over the title-screen scene).
- Why RenderDoc "sees zero": under capture the vertex data reads as all-zero,
  so w=0 -> the VS transform yields a *finite* w=1.0 -- accidentally masking
  the real NaN-w bug and sending every prior session down the vertex-fetch
  rabbit hole. `smoke.rdc`'s post-VS `gl_Position=(-0.997,-0.998,1,1)` is the
  transform of a zero input, not the real draw.

## Why it happens / where the proper fix goes

These are **pre-transformed, screen-space** vertices (positions are literal
pixel coords). The guest leaves `position.w = 0xFFFFFFFF` as a don't-care
because real Xenos, with the draw's `PA_CL_VTE_CNTL` state, neutralizes it
(Xenos ALU float rules: `0 * NaN = 0`, NaN-flushing; w treated as 1 for
window-space verts). The renderer already maps `PA_CL_VTE_CNTL.vtx_*_fmt` into
`kSysFlag_XY/Z_DividedByW` / `kSysFlag_WNotReciprocal`
(`native_command_processor.cpp` ~line 3308), but that flag only chooses `w` vs
`1/w` -- both NaN -- so it does not tame a garbage w. The SDK's
`SpirvShaderTranslator` `1/w` path does not reproduce Xenos NaN semantics.

## Fix implemented (2026-07-13)

The targeted SSBO patch is now **always-on** (no env gate) inside the
`samples_smoke` block in `TryDraw` (`src/native_command_processor.cpp`). For
every draw whose pixel shader samples the 852x128 texture, it scans each
vertex's `position.w` in the shared-memory SSBO; if the dword is NaN or Inf
(IEEE 754 exponent all 1s), it overwrites it with `1.0f` (`0x3F800000`) and
re-flushes the mapped range. This is the same operation the earlier
`NOCTURNE_SMOKE_FIX_W` env-gated probe validated, but unconditionally applied
to the correctly-targeted draw.

The unrelated full-screen red quad (which also has NaN w but renders correctly
via a different code path) is not affected because the fix is keyed on the
852x128 texture match, not on a NaN heuristic.

General upstream fix (still desired long-term):
1. In the SDK `SpirvShaderTranslator` position-export epilogue, apply Xenos
   float semantics to the w path (flush NaN/Inf, or force w=1 when the
   viewport-transform state marks the vertex as window-space/pre-transformed),
   matching real hardware. This is the correct, general fix (SDK change, see
   `../rexglue-sdk`).
2. Verify `kSysFlag_WNotReciprocal` polarity vs Xenia for this VTE_CNTL state
   -- if inverted, the shader may be taking the wrong branch.

---

# (historical) Open bug: smoke quad (and a sibling solid-color quad) render degenerate

Read `docs/native-rendering.md` first for architecture/status. This file is a
focused investigation log for one specific, still-open bug, kept separate
because the investigation was long and easy to re-tread by accident.

## Symptom

There should be a smoke texture scrolling horizontally in the top-right area
of the screen during normal gameplay (stage/menu scene with the health bar
HUD visible). It never appears. A RenderDoc capture of the broken frame shows
the draw call executing, sampling the right texture, but rasterizing nothing.

## Repro

Two independent RenderDoc captures (`smoke.rdc`, `smoke2.rdc`, both from the
`native` branch, the second taken after the cleanup described below) both
show the *same* symptom on the *same relative draw*, in an otherwise-normal
gameplay frame (stage background + HUD, ~5 UI draws in the main color pass).
Not yet reproduced outside a RenderDoc capture -- see "What's confirmed"
below.

## What's confirmed (don't re-derive these)

- The smoke texture is real and correctly uploaded/decoded. Identified by
  content (a soft grayscale cloud/mask image, confirmed visually via
  `save_texture`) at `dumps/textures/8814a07ceb7c7f5a_852x128_k_8_8_8_8.png`.
  In both captures this texture is sampled by exactly one draw call in the
  main color pass.
- That draw call (and, in both captures, exactly one other draw in the same
  pass -- a textureless solid-color quad, different shader) rasterizes
  **nothing**: all 4 vertices of its quad come back from RenderDoc's
  post-vertex-shader data as byte-for-byte identical (same clip position,
  same UV, same color) -- a zero-area primitive.
- This is a genuine vertex-fetch data problem, not a shader logic bug. Traced
  the SPIR-V (`SpirvShaderTranslator`-generated) by hand for the smoke
  draw's vertex shader: it correctly computes a per-invocation fetch address
  as `base + floor(vertex_index) * 8` (dwords), using `gl_VertexIndex`
  correctly, no shared/aliased addressing across the 4 invocations. This
  matches the shader used by *working* draws in the same frame (same
  generator, same structure, just different translated instances).
- The **fetch constant itself is correct**: decoded `fetch_constants[47]`
  (the slot this shader's vfetch instruction references) by hand from the
  raw uniform buffer bytes for every draw in the pass (118, 122, 126, 130,
  134 in `smoke2.rdc`). Every draw gets its own distinct, plausible physical
  address (`0x1FB01xxx`..`0x1FB11xxx` range, sequential/bump-allocated
  looking). Address decode is not the bug.
- The **raw guest memory at that address is genuinely all zero** for the
  broken draws -- verified directly via `mcp__renderdoc__get_buffer_data` on
  the `xe_shared_memory` SSBO (`ResourceId::200`) at the exact computed byte
  offset, with a wide (~1KB) window scan around it to rule out an
  off-by-a-small-amount addressing error, and with `set_event` pinned to the
  draw before reading. This is not a tool artifact (unlike `get_cbuffer_contents`,
  which *was* independently confirmed broken/unreliable for this shader and
  should not be trusted -- always read fetch constants via raw
  `get_buffer_data` + manual byte parsing instead).
- **Address locality doesn't explain it.** In `smoke2.rdc`, draw 130's
  (broken) vertex data sits at `0x1FB11450`; draw 134's (working, real
  distinct 4-corner data) sits at `0x1FB114D0` -- 128 bytes later, in the
  *same* bump-allocated scratch arena, immediately downstream. The broken
  draw isn't in some separate/uninitialized memory region; its neighbor in
  the same pool is fine.
- `NativeCommandProcessor::UpdateSharedMemory` (the guest-RAM -> SSBO mirror
  sync, called per-draw from `TryDraw` right before pipeline/descriptor
  setup) is correct: units match Xenia's own convention (`fetch.size` is in
  dwords despite the misleading "size in words" comment inherited from
  Xenia's header), the byte range copied covers the full declared vertex
  count, and `FlushMappedMemoryRange`'s parameters match its documented
  signature. Not a sync-plumbing bug.
- **Not caused by the now-removed debug-override code.** `native_command_processor.cpp`
  used to unconditionally overwrite whatever draw was the app's literal
  2nd/6th draw *ever* (since process start, `draws_logged_` never resets)
  with hardcoded debug geometry. This was real, hazardous dead code and has
  been removed (along with extending the `vfetch_logged` diagnostic to dump
  the *entire* declared vertex range instead of just vertex 0 -- the
  original version could not have caught this bug, since it only ever
  sampled dword 0). But the bug reproduces identically in `smoke2.rdc`,
  captured *after* that removal, on a freshly rebuilt binary. Rule this out
  before re-investigating it.
- **Not an indirect-buffer dedup bug.** `KernelState`'s ring/IB decode dedups
  *debug logging* of repeated `(ptr, len)` indirect-buffer targets, but
  explicitly always re-decodes content for any registered
  `native_gpu_command_callback_` (see `kernel_state.cpp` around
  `decode_ib_content`) -- real consumers never get skipped content.
- **A live (non-RenderDoc) run never reproduces this.** Rebuilt the binary
  with the extended vfetch logging, ran it standalone (no capture), and the
  equivalent draw (`fetch_constant=95`, same texture) showed fully valid,
  correctly-varying 4-corner vertex data every time across several runs.
  The bug is either capture-timing-sensitive, or specific to whatever game
  state/frame RenderDoc happened to trigger on both times (see below).
- **A mutex around `NativeCommandProcessor::OnPacket` does NOT fix it, and
  is actively harmful.** Tried this as the most concrete hypothesis
  available (see "Ruled out" below for the reasoning). It compiles and
  runs, but the game drops into slow motion (almost certainly the lock
  serializing `PresentFrame`'s synchronous fence wait against the headless
  vblank thread, which can legitimately need to call back into
  `OnPacket`-adjacent state), and the smoke is *still* invisible. **Reverted.**
  Don't re-attempt this exact fix without first proving OnPacket is actually
  entered concurrently (see "Next steps").

## Hypotheses considered and ruled out

In addition to the ones woven into "What's confirmed" above:

- Wrong units for `fetch_constants[47].z`'s base address (dword vs byte
  shift) -- re-derived by hand multiple times, cross-checked against
  Xenia's own vfetch struct comments and consumers (`draw_util.cc`,
  `trace_viewer.cc`). Not the bug.
- `PM4_SET_CONSTANT`/`PM4_LOAD_ALU_CONSTANT` decode gap analogous to the
  already-fixed bug #20 (dynamic pixel-shader vfetch constant) -- checked
  `packet_disassembler.cpp`'s `PM4_SET_CONSTANT` case by hand; it's generic
  register-write forwarding, not type-specific, so there's no equivalent gap
  for vertex-stage fetch constants specifically.
- Point-sprite/host-side quad expansion mismatch (i.e. hardware expecting 1
  real vertex expanded to 4 corners, vs. our quad-list code expecting 4 real
  vertices) -- ruled out by checking `xenia-canary`'s own
  `GetQuadListTriangleListIndexCount` (`(n/4)*6`) and `PrimitiveType::kQuadList`
  handling: real Xenos quad lists require 4 real vertices per quad, same as
  what our code assumes.
- A real guest index buffer being ignored in favor of host-synthesized
  sequential indices -- checked the actual `PM4_DRAW_INDX` decode log
  (`source_select=2` i.e. auto-index, `index_buffer_size_words=0`) for every
  draw in the pass, including the broken ones. All auto-indexed; no real
  guest index buffer is being discarded.

## Working theory (unconfirmed — historical)

The bug is real, reproducible on demand via RenderDoc capture, isolated to
2 of 5 draws per frame (always the same *relative* two, not tied to a
specific physical address range), and does not reproduce in an
uninstrumented run. The `mcp__renderdoc__debug_shader_at_pixel` tool
reported "DebugPixel returned no trace data (may not be supported for this
API/GPU)" when asked to step this shader, so RenderDoc's own shader debugger
isn't available as a cross-check here.

Live lldb investigation (RelWithDebInfo build, see `docs/native-rendering.md`'s
"Quick reference" for the exact build steps -- the previously-documented
RelWithDebInfo startup segfault did **not** reproduce this session, so that
known-issue note may be stale) established one structural fact worth
building on: `KernelState::StartHeadlessVblankThreadIfNeeded` spins up a
"Headless VBlank" host thread whose loop calls
`DispatchGraphicsInterruptCallback`, which runs the guest's registered
vblank ISR **directly on that thread** via
`FunctionDispatcher::ExecuteInterrupt` -- not marshaled onto the main
thread. A breakpoint on `KernelState::SetGraphicsInterruptCallback` (the
guest registering that ISR) fired once early in boot, confirming the game
does register a vblank callback. A breakpoint on
`DispatchGraphicsInterruptCallback` itself never fired in a 90-second
session despite the game clearly running many frames (5MB+ of rotated
logs) -- inconclusive; likely an inlining/breakpoint-targeting problem in
the RelWithDebInfo build rather than proof the dispatch never happens.

The mutex experiment (see above) was an attempt to defensively serialize
`NativeCommandProcessor::OnPacket` against exactly this possible
main-thread/vblank-ISR-thread concurrency, on the theory that if the guest's
ISR itself kicks a ring-buffer submission (writes `CP_RB_WPTR`, which is a
generic MMIO callback with no thread affinity -- see
`KernelState::HeadlessWriteRegister`), two threads could both enter
`OnPacket` and race on `registers_`, `shared_memory_mapped_`,
`frame_transient_buffers_`, `draws_logged_`, and in-flight Vulkan command
buffer recording, all completely unsynchronized. **This did not fix the bug**,
which means either:

1. The vblank ISR does *not* actually kick ring submissions in this game
   (plausible -- SOTN may only use vblank for pacing/timing, not rendering),
   and the real producer of this scratch vertex data is something else
   entirely on the *same* thread as the draw, in which case this is not a
   cross-thread race at all; or
2. It is a race, but not one `OnPacket`'s critical section covers -- e.g. the
   actual write happens via a completely different path (a guest memory
   store not routed through any callback we intercept), and slowing down
   `OnPacket` just changed timing without addressing the real unsynchronized
   access.

## Vblank ISR thread question: answered, negative (step 2 done)

Added temporary `REXGPU_INFO` thread-id logging (SDK side, since reverted --
not a repo diff) at the top of `DispatchGraphicsInterruptCallback` and inside
`HeadlessWriteRegister`'s `CP_RB_WPTR` case, then ran a full standalone
session from cold boot through the main menu (no RenderDoc, no lldb -- just
`scripts/run.py` with `logs/*.log` captured, ~5MB across several rotated
files). Result, checked by grepping every log line's `[tNNNNN]` prefix for
each of the two call sites plus `vfetch draw#`:

- `DispatchGraphicsInterruptCallback` (the vblank ISR dispatch) **always**
  ran on one host thread (`t22444` this run -- the dedicated "Headless
  VBlank" thread, as expected from `StartHeadlessVblankThreadIfNeeded`).
- `HeadlessWriteRegister`'s `CP_RB_WPTR` handling, and every single
  `vfetch draw#N` log line (i.e. every draw, from boot through gameplay),
  **always** ran on a single other host thread (`t11148`).
- These two thread ids never mixed for either log site, across the entire
  session -- confirming the vblank ISR and ring-buffer
  submission/draw-processing are two genuinely distinct, non-interleaving
  (from a *submission* standpoint) threads, but critically: **the vblank ISR
  thread never once called `HeadlessWriteRegister`** (zero `CP_RB_WPTR`
  log lines with the ISR's thread id anywhere in the session). So while the
  two threads *do* run concurrently in wall-clock time (interleaved
  timestamps down to the millisecond were observed), the ISR thread never
  touches ring submission, and therefore never drives `OnPacket`/`TryDraw`/
  `UpdateSharedMemory` at all.
- This confirms **hypothesis 1** from the previous session: the vblank ISR
  does not kick ring submissions in this game (SOTN's vblank callback is
  pacing/timing only). All draw processing for this title is single-threaded
  on `t11148`. The earlier `OnPacket` mutex experiment was therefore
  guarding against a race that structurally cannot happen -- consistent with
  it having no effect on the bug.

## Breakthrough: reproduces live with pure added latency, no RenderDoc needed

Tested the "single-threaded timing, not a race" theory directly: added a
temporary env-var-gated artificial delay
(`NOCTURNE_SIMULATE_CAPTURE_DELAY=1` -> 3ms `std::this_thread::sleep_for` at
the top of every `TryDraw`, see `src/native_command_processor.cpp`) to
simulate RenderDoc's per-draw CPU overhead without RenderDoc itself, plus a
permanent-but-capped diagnostic in `UpdateSharedMemory` that logs any vertex
fetch range that copies back entirely zero (independent of the old
`vfetch_logged < 10` cap, which only covered the first few draws ever).

Ran a real interactive session (`NOCTURNE_SIMULATE_CAPTURE_DELAY=1 python
scripts/run.py`, no lldb, no RenderDoc) from boot through the main menu into
actual gameplay (confirmed visually: **the smoke overlay was missing in the
live gameplay view**, same as the RenderDoc-only symptom). The log confirms
why:

```
NativeCommandProcessor: UpdateSharedMemory range is ALL ZERO -- address_dwords=07EB02A4 size_dwords=6
```

This is the same failure signature as the RenderDoc captures (fetch address
resolves fine, guest memory at that address is genuinely zero at read time).
**This settles the open question from the previous session**: the bug is
*not* RenderDoc-specific. It's triggered by generic added latency in the
draw-processing path -- RenderDoc capture overhead was just one way to
introduce that latency; a plain `sleep_for(3ms)` per draw does the same
thing. This means:

- The remaining "vblank ISR" and "cross-thread race" theories are now fully
  closed, not just for the ISR thread specifically -- there is no need for
  *any* second thread to explain this. A single delayed thread reproduces it.
- The real mechanism is almost certainly guest-side and frame-time-driven:
  something in the game's UI/effects update logic (likely a per-frame
  "should I refresh this scratch vertex data" decision, possibly gated on a
  frame counter, a dirty flag, or a real-time delta) skips writing new smoke
  geometry for a frame when that frame's wall-clock pacing is perturbed
  (slower `TryDraw`/`PresentFrame` turnaround -> guest's own timing-dependent
  logic observes a different delta than it would natively), leaving the
  scratch slot at whatever it last held -- likely zero-initialized backing
  memory that a *previous* real write never touched, if the ring/bump
  allocator handed out a fresh slot this frame that the skipped update never
  filled.
- **This is now reproducible on demand without RenderDoc**, which is a much
  cheaper repro loop for the next phase: attach lldb directly to a
  `NOCTURNE_SIMULATE_CAPTURE_DELAY=1` run (no capture-injection complexity,
  no RelWithDebInfo-specific RenderDoc interaction to fight), break in
  `UpdateSharedMemory` when the zero-range diagnostic fires, and walk
  backward/forward from there.

The `NOCTURNE_SIMULATE_CAPTURE_DELAY=1` sleep-injection knob described above
has since been **removed** from `src/native_command_processor.cpp` (it was a
diagnostic probe to test a hypothesis, not a fix, and was never meant to be
permanent) -- the finding it produced is still valid and documented above,
it's just no longer live code. Re-add an equivalent probe (a plain
`std::this_thread::sleep_for` at the top of `TryDraw`, gated behind
something that defaults off) if you need to re-run this experiment; nothing
about the finding depends on the exact removed diff. The zero-range
diagnostic in `UpdateSharedMemory` (logs any vertex-fetch range that copies
back entirely zero, capped at 50 lines) is still in the tree and is
independent of the delay knob -- it fires on a real RenderDoc-triggered
repro too.

## Next steps for whoever picks this up

1. **Don't re-try the `OnPacket` mutex, and don't chase vblank-ISR or any
   other cross-thread-race theory.** Fully closed -- see "Breakthrough"
   above; a single-threaded delayed run reproduced the bug, so there is no
   race to guard against anywhere.
2. Re-add a delay probe (see above) as the primary repro for the next
   debugging session -- no RenderDoc, no lldb overhead fighting a capture.
   The bug wasn't deterministic-every-frame under the removed 3ms knob (one
   `ALL ZERO` hit was observed in the session tested, not a hit every frame
   the smoke should be visible) -- worth first checking whether a larger
   delay (10-20ms?) or a delay placed elsewhere (e.g. only around
   `PresentFrame`, or only for specific fetch constants) makes it fire more
   reliably before moving to live breakpoint work, to keep the debugging
   loop fast.
3. With a reliable delayed repro, treat this as single-threaded timing: add
   a log line at the *exact* guest instruction/function that writes this
   scratch vertex arena (find it by watching guest writes to the physical
   address range `0x1FB0xxxx`-`0x1FB2xxxx` via a memory watchpoint in lldb,
   or a `[[midasm_hook]]` per `docs/native-rendering.md`'s hook recipe) and
   compare its execution order, within a single frame's guest instruction
   stream, against when `TryDraw`/`UpdateSharedMemory` reads that same
   range for the broken draws vs. the working ones. The goal is to find
   *what's different* about frames where RenderDoc's capture is active that
   causes this producer step to be skipped or reordered relative to the
   two specific consumer draws -- RenderDoc capture overhead does perturb
   guest-visible timing in this headless architecture (everything is
   processed synchronously off Vulkan-call timing), so "only reproduces
   under capture" is a real, explainable symptom, not necessarily a
   RenderDoc artifact -- and neither is "only reproduces under added
   latency"; see "Breakthrough" above for why RenderDoc was never actually
   necessary.
4. Once a live repro under lldb (using the delay knob, not RenderDoc) is
   achieved, a
   `bt`/register dump at the moment `UpdateSharedMemory` reads all-zero
   data for one of these two draws, compared against the moment (if any)
   the real data gets written, will settle this quickly. Everything short
   of that has been tried and has converged on "the address/decode/sync
   code is correct, the guest memory itself is empty at read time" as the
   irreducible fact -- the remaining question is purely *why*, guest-side.
