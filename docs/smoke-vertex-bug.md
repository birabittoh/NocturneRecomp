# Open bug: smoke quad (and a sibling solid-color quad) render degenerate

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

## Working theory (unconfirmed)

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

The delay knob and the zero-range diagnostic are left in the tree
(`src/native_command_processor.cpp`), both off by default (env var unset /
capped at 50 log lines) -- intentionally, as a debugging tool for whoever
continues this, not dead code to clean up prematurely.

## Next steps for whoever picks this up

1. **Don't re-try the `OnPacket` mutex, and don't chase vblank-ISR or any
   other cross-thread-race theory.** Fully closed -- see "Breakthrough"
   above; a single-threaded delayed run reproduces the bug, so there is no
   race to guard against anywhere.
2. Use `NOCTURNE_SIMULATE_CAPTURE_DELAY=1` (no RenderDoc, no lldb overhead
   fighting a capture) as the primary repro for the next debugging session.
   The bug isn't yet deterministic-every-frame under this knob (one
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
