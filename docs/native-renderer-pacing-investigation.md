# Native renderer: frame-pacing / "skips ahead" investigation

Status as of this writing: **unresolved**. The game periodically appears to
"skip ahead" 2-3 seconds of gameplay, as if it computed several seconds'
worth of simulation almost instantly. With the xenos GPU plugin this never
happened (aside from a since-fixed, unrelated `timeBeginPeriod(1)` timer-
resolution issue). All experimental code from this investigation has been
reverted (`git checkout` on the files below) so the tree is back to how it
was before this investigation started -- this doc exists so the next attempt
doesn't have to re-derive the same findings from scratch.

Files touched during the investigation (all reverted):
`src/native_command_processor.cpp`, `src/native_command_processor.h`,
`src/nocturnerecomp_hooks.cpp`.

## Symptom, precisely

- Host and guest FPS were both pinned around ~40 (xenos got 120 host / 60
  guest).
- Roughly every 5-10 real seconds, the game visibly "skips ahead" as if
  several seconds of gameplay were simulated near-instantly.
- Subjectively: "runs slow for a while, then skips all of the frames at once
  as if it's trying to stay at pace."

## Architecture recap (see docs/native-rendering.md for the full picture)

`KernelState::HeadlessWriteRegister` (rexglue-sdk `src/system/kernel_state.cpp`)
is the MMIO handler for `CP_RB_WPTR` writes. It decodes newly-submitted PM4
packets from the ring (following `PM4_INDIRECT_BUFFER` targets) and forwards
each decoded packet synchronously to `KernelState::native_gpu_command_callback_`,
which this project binds to `NativeCommandProcessor::OnPacket`
(`src/native_command_processor.cpp`). That processes register writes,
`OnShaderLoad`, `OnDraw`, and calls `PresentFrame()` on swap packets.

Critically, **all of this used to run synchronously, inline, on the guest
CPU thread** -- `HeadlessWriteRegister` is itself the MMIO write handler, so
whatever `OnPacket` does happens before the guest instruction that triggered
the write can continue.

`HeadlessRingWaitBypass` (`src/nocturnerecomp_hooks.cpp`) is a pair of
`[[midasm_hook]]` bypasses (`nocturnerecomp_config.toml`, addresses
`0x825252C8` and `0x82524330`) that make the guest's "wait for the GPU to
catch up" spin-loop primitive always exit instantly when headless, since
there's no real GPU to advance the counter it polls. This means **there is
no real backpressure at all** between the guest and the renderer -- a
deliberate, documented tradeoff from an earlier milestone (see
docs/native-renderer-headless-boot.md bug #2), but it's the root enabler of
everything below.

## What was tried, in order, and what each one found

### 1. `timeBeginPeriod(1)` -- ruled out immediately

Already called unconditionally in `OnPreSetup` (`nocturnerecomp_app.h`)
regardless of renderer, well before the native/headless path is even chosen.
Not the cause.

### 2. Headless vblank thread catch-up cap (rexglue-sdk, kept)

`KernelState::StartHeadlessVblankThreadIfNeeded`'s vblank-dispatch loop had
an unbounded catch-up `while`: if it ever fell behind real time, it would
fire every backlogged vblank interrupt in one tight burst instead of
resyncing. Capped to firing at most a few backlogged ticks before resyncing
`last_frame_time` to `current_time` (mirrors the standard game-loop
"spiral of death" fix). **This is a real, harmless improvement and is still
in `rexglue-sdk`** (it wasn't reverted -- it lives in the SDK repo, not this
one). Did not fix the main symptom by itself.

### 3. Diagnostic timing instrumentation (reverted)

Added `steady_clock` timing around `PresentFrame`'s submit/fence-wait, and a
per-swap packet-type histogram in `OnPacket`, logged whenever a frame
exceeded a stall threshold (not just every Nth frame -- a modulo-N sample
missed the actual stall entirely the first time, since it only sampled 1
frame in 60). This is what actually localized the problem. If re-attempting,
recreate something like this early -- it's cheap and was decisive. Key
technique: log unconditionally on threshold-crossing, not on a fixed
interval, or you will sample right past the event you're looking for.

### 4. Root cause A: per-draw `vkAllocateMemory` (fixed, reverted -- reapply this one)

`TryDraw`'s `upload_constant_buffer` lambda called
`rex::ui::vulkan::util::CreateDedicatedAllocationBuffer` (a real
`vkCreateBuffer` + dedicated `vkAllocateMemory`) once per constant buffer
per draw -- up to 5 per draw, ~250-600 draws/frame, so up to ~1300 real
memory allocations per frame. `vkAllocateMemory` is one of the most
expensive Vulkan calls per-invocation; this alone cost ~15-20ms/frame.

**Fix** (worth reapplying on its own, independent of everything else in this
doc): replaced with one large (64MB) persistently-mapped "constants arena"
buffer, created once, that `upload_constant_buffer` suballocates from via
offset (respecting `minUniformBufferOffsetAlignment`) instead of allocating
fresh. Reset (`constants_arena_offset_ = 0`) once per frame in
`EnsureFrameBegun`, right after the fence wait that guarantees the GPU is
done reading the previous frame's region. Flushed once per frame (in
`PresentFrame`, before submit) instead of once per buffer.

Measured effect: draw cost dropped from ~15-20ms/frame to ~5-10ms/frame;
host FPS went from ~40 to ~50. Real, uncontroversial improvement -- no
observed downside. **Recommend reapplying this fix specifically**, since
it's unrelated to the pacing investigation and was purely a performance bug.

### 5. Root cause B: indirect-buffer spin-resubmission storm (partially fixed, reverted)

With the arena fix in place, per-swap logging caught the real stall event: a
single `CP_RB_WPTR` write's decode window occasionally contained **19000+
packets** (vs. a normal ~250-600), overwhelmingly `PM4_TYPE0` (raw register
writes) reached via ~1000 `PM4_INDIRECT_BUFFER` packets (vs. a normal ~10).
Root cause, found by reading `KernelState::HeadlessWriteRegister`
(rexglue-sdk): the guest resubmits the same handful of indirect-buffer jumps
in a tight spin while waiting on something, since nothing ever drains the
ring headlessly (see `HeadlessRingWaitBypass` above) -- and
`decode_ib_content`'s dedup (`already_seen_ib`) only gated the **debug log**,
not actual decode/forward: `bool decode_ib_content = is_indirect_buffer &&
ib_ptr && ib_len && (!already_seen_ib || native_gpu_command_callback_);` --
with a real callback always registered, this was unconditionally `true`.
Every spin iteration fully re-decoded and re-forwarded the same content to
the renderer.

**Fix applied in rexglue-sdk** (`src/system/kernel_state.cpp`,
`HeadlessWriteRegister`) -- **this one is still live, not reverted**, since
it's in the SDK repo:
- Added a per-call (per `CP_RB_WPTR` write, i.e. per decode window) local
  `std::set<std::pair<ptr,len>> decoded_ibs_this_call` dedup, distinct from
  the persistent `logged_indirect_buffers` set (which must stay
  forever-lived -- the guest legitimately reuses the same physical address
  for new content across real frames, so that one can only gate the debug
  log, not real decode).
- Additionally added content-hash dedup (`XXH3_64bits` over the IB's actual
  bytes, also per-call-scoped) because address-only dedup wasn't sufficient:
  a real repro showed ~1000 IBs with mostly *distinct* addresses (a small
  ring-allocated pool the guest cycles through while spinning) but
  near-identical content.

Measured effect: worst observed stall dropped from **~7.3 real seconds to
~50-450ms** for the same burst pattern -- an order of magnitude, but **not
fully eliminated**. Some bursts still process ~10000+ packets in ~50-100ms
(cheap now thanks to the arena fix, but still real wasted redundant work),
and the content hash doesn't catch every case (presumably some resubmitted
content genuinely differs slightly frame-to-frame within the spin, e.g. a
counter or timestamp embedded in the command stream, defeating an exact
hash match).

**If re-attempting: this SDK fix is still worth having independent of the
rest.** It was not reverted and should still be in `rexglue-sdk`. Verify
with `git -C ../rexglue-sdk log` / `diff` before assuming it's still there.

### 6. A second, distinct freeze: ~400-450ms with zero packet traffic

Separately from the storm above, per-swap logging also caught recurring
~400-450ms gaps in `PresentFrame`'s pre-sleep elapsed time with **no**
accompanying packet-count spike at all -- meaning nothing was happening in
`OnPacket` during that window. This is *not* a rendering/decode issue; it's
something else blocking the same thread (at the time, still the single
guest/render thread) for ~400ms on a roughly 5s cadence, unrelated to GPU
command traffic. **This was never actually diagnosed** -- see "Next steps"
below.

### 7. Threading refactor: decouple guest from render (reverted -- do not just reapply blindly)

Compared against xenos's real `CommandProcessor`: `UpdateWritePointer`
(xenos) just sets a value and signals an event, returning immediately --
all real decode/draw work happens on `CommandProcessor::WorkerThreadMain`, a
dedicated thread. This project's `OnPacket` ran everything inline on the
guest thread instead, which is *why* guest and host FPS were pinned
together, and why *anything* that stalled the render/decode chain also froze
guest simulation (explaining both the storm-driven stalls above and
plausibly the mystery ~400ms freeze in #6, though that was never confirmed).

Implemented: `OnPacket` (the actual `NativeGpuCommandCallback`) now just
copies the packet's bytes (`packet_base` is only valid for the call's
duration) into a `QueuedPacket` and pushes it onto a mutex+condvar-guarded
queue; a new `worker_thread_` (started at the end of the constructor, after
all Vulkan resources exist; joined at the start of the destructor, before
any Vulkan teardown) drains the queue and runs the old `OnPacket` body,
renamed `ProcessPacket`, unchanged otherwise.

**Consequence, not fixed before reverting:** decoupling removed the
*accidental* pacing the synchronous design used to provide (the guest
thread blocking inline on ~10-20ms of real render cost every frame was,
completely by accident, functioning as the guest's frame-rate cap). Once
decoupled, with a generous queue cap (8192), the guest ran fully unthrottled
-- observed **~400% real-time speed**, visibly. This is because
`HeadlessRingWaitBypass` (see architecture recap) provides no real
backpressure at all; the accidental synchronous-thread stall was the only
thing that had been pacing the guest.

**First attempted fix (partially effective):** shrunk the queue cap
(`kMaxQueuedPackets`) from 8192 down to 512 (~1 typical frame's worth), so
`OnPacket` blocks the guest thread almost every real frame waiting for the
worker to drain, approximating the old accidental pacing through genuine
backpressure. This reduced the speed-up but **the original skip-ahead
symptom was still present** ("still happening" -- user report) -- the
per-swap logs after this change still showed both the storm-style
10000+-packet bursts (up to ~1.2s) *and* the ~400ms silent gaps, now
confirmed via the worker thread's own idle-wait to be genuinely idle (no
packets at all during those gaps) -- ruling out the render/decode path as
their cause and pointing at *something else on whatever thread produces
those gaps*.

**Second attempted fix (broke boot -- do not repeat this specific approach):**
tried gating `HeadlessRingWaitBypass` itself on the new queue's real depth
(via a new `NativeCommandProcessorHasRoomForMorePackets()`, wired into both
hook addresses) instead of the flat queue-size cap, reasoning that this
would make the guest's *own* spin-wait respect real backlog like it would on
real hardware. **This hung the game at boot with 100% CPU / black screen.**
Root cause: `HeadlessRingWaitBypass`'s own comment states the hook fires at
multiple call sites for different logical waits ("polls a GPU-driven counter
... not always the CP ring buffer's read pointer"). Gating *all* of them on
render-queue depth broke whichever call site(s) aren't actually about
rendering backlog, since nothing else ever satisfies their real wait
condition headlessly -- they spun forever. **If ever revisiting this
approach: first identify, via lldb/disassembly at `0x825252C8` and
`0x82524330`, which (if either) of the two hook addresses is actually the
ring-capacity wait, and only gate that one.**

Given the user reported the skip-ahead was still present even with the
sub-512 queue cap (before the above hook attempt), **the threading refactor
alone does not fix the core symptom** -- it changes *how* the imbalance
manifests (unthrottled 400% speed instead of periodic skips, depending on
queue size) but doesn't address why the guest and renderer fall out of sync
in the first place. All of it was reverted via `git checkout` on the three
files listed at the top.

## What's actually still unexplained

1. **The ~400ms silent freeze** (#6): confirmed not to involve any PM4/GPU
   traffic, confirmed (via the worker thread experiment) to be genuinely
   idle on whatever thread was measured, and never actually diagnosed via a
   live backtrace. An attempted "poor man's profiler" (repeated lldb
   attach/interrupt/backtrace/detach in a bash loop) failed for boring
   reasons (lldb's `process continue` blocks synchronously in batch/script
   mode -- don't use that pattern; repeated attach/detach cycles also left
   the target process in a bad state once, requiring a fresh launch) before
   a real sample was ever captured. **Next attempt should either:**
   - Use lldb's Python API directly (`import lldb`, `SBProcess.Continue()` is
     non-blocking, unlike the CLI `continue`) to build a real sampling
     profiler loop within one attached session, or
   - Add a self-triggering watchdog: a background thread that tracks a
     heartbeat updated every packet/frame, and calls `__debugbreak()` the
     moment the heartbeat goes stale beyond a threshold -- if lldb is
     already attached and sitting on `continue`, it traps exactly at the
     freeze with zero timing-race risk. (This was designed but never
     implemented before the investigation pivoted to the architecture
     comparison in step 7.)
   - Whatever it turns out to be, it's most likely audio, a periodic
     kernel/XAM callback, or some other guest-side blocking call entirely
     unrelated to graphics -- do not assume it's GPU/PM4-related again
     without evidence; that assumption already cost one investigation cycle.

2. **Why the guest falls behind at all**, root-root-cause: `HeadlessRingWaitBypass`
   provides zero real backpressure, by design, from an earlier milestone.
   Any real fix likely needs *some* form of genuine pacing restored between
   guest submission rate and render throughput -- but not via a blanket gate
   on that shared hook function (see the boot-hang above). Options not yet
   tried: gate only the correctly-identified ring-capacity hook site (after
   disassembly investigation); or pace via the *vblank* mechanism instead
   (the guest's real per-frame timing should, on real hardware, come from
   waiting on vblank/GPU-signaled events specifically, not from ring
   capacity) -- worth checking whether the guest's actual frame-loop wait is
   even the ring-wait hook at all, versus some other primitive entirely.

## Recommended next steps, in order

1. Reapply the constants-arena fix (#4) on its own -- pure win, no downside,
   unrelated to the rest of this investigation.
2. Confirm the rexglue-sdk indirect-buffer dedup fix (#5) is still present
   (it was never reverted, but verify) -- real improvement, though not a
   complete fix for the storm.
3. Diagnose the ~400ms freeze (#1 above) with a real live backtrace before
   doing anything else architectural -- everything downstream depends on
   knowing what that actually is.
4. Only after (3), reconsider whether a threading split and/or real
   backpressure restoration is the right fix, informed by what's actually
   causing the silent freeze (it may turn out to be unrelated to rendering
   entirely, in which case the threading refactor may not be necessary at
   all for the core symptom).

## UPDATE (2026-07-12): the symptom is a per-present update-burst, not a stall

A long session re-investigated the "skip ahead" with fresh in-process
instrumentation. The symptom is now measured precisely and several earlier
theories are **disproven**. The core cause is still unfixed, but the search
space is much smaller. Two facts from the user that reframed everything:

- **It is GPU-related / native-only.** The skip does **not** happen with the
  xenia-based `xenos` GPU plugin -- only with the native Vulkan renderer.
- **A prior threading split made the burst frames *visible*.** With GPU work on
  a second thread the "skipped" frames were actually *seen* being rendered very
  fast, rather than skipped. So the guest genuinely *produces* those frames as
  fast as it can; single-threaded they mostly never reach the screen.

### What the symptom actually is (measured, reliable)

The guest frame/update loop is `sub_82578A30` (Application::Run) ->
`sub_8258B8A0` (mode loop, `do { ... } while(!*(a1+184))`) -> `sub_8258B3B8`
(per-frame fixed-timestep tick) -> `sub_825AAE90` -> `sub_825D1618` (the
"for each active screen: Update()" dispatcher).

Counting entries to `sub_8258B3B8` per 500ms of host time:
- **Baseline: ~30 ticks/500ms (exactly 60fps), steady.**
- **Bursts: 300-724 ticks/500ms (~10-24x) every ~6-6.5s, lasting ~1s.**
- **`PresentFrame` stays ~30/500ms (paced by its 16ms sleep) throughout**, and
  its pace-sleep engages ~27-30 times/window even during a burst.

So during a burst the guest runs ~10-24 *update ticks per presented frame* --
that is the visible "runs very fast for a few frames." It is genuine game-logic
advancement (not just re-rendering), consistent with the threading observation.

### Disproven / ruled out this session

- **Not the vblank interrupt over-delivering.** A probe in the headless vblank
  thread (`kernel_state.cpp`) shows it *under*-delivers during bursts (~11-16
  dispatched/500ms vs ~30 normal). The drop is a *symptom* (the presented-frames
  backpressure gate below throttles it because present stalls), not the driver.
  The guest runs far more update ticks than vblanks delivered, so the catch-up
  target is **not** the vblank count.
- **Not the save-data / XamContent waits.** The freeze watchdog (below) caught
  recurring ~105-120ms `NtWaitForSingleObjectEx` stalls on the main thread with
  the same RIP each time, in a save-data read chain: `sub_824F86C8` (reads
  `save:\%s` via `XamContentCreate`/`XamContentClose` + an `_XOVERLAPPED`) ->
  `sub_825D7CF0` (overlapped-IO wait, checks status `997` = ERROR_IO_PENDING) ->
  `sub_825DA6B0` -> `sub_825DC320` (the `NtWaitForSingleObjectEx` wrapper). These
  are real and roughly ~6s-cadenced, **but their timestamps do not line up with
  the bursts** -- most bursts have no preceding stall at all. Not the cause.
- **Not the SDK clock.** `rex::chrono` (`src/core/clock.cpp`) advances smoothly
  from host ticks; no periodic resync/jump. (Note a latent detail: its
  `UpdateGuestClock` `try_lock` path returns the last value without advancing
  when contended -- but that returns the freshest value another thread wrote,
  not a stale/jumping one.)
- **Not the framework object's frame counters.** `sub_8258B3B8` compares
  `game_time` at `a1+2236` vs `target_time` at `a1+2232` (guest big-endian; `a1`
  == `r3` at entry, confirmed from generated code). A probe reading them shows
  sane, climbing values but **`target - game` stays <= 0 through every burst**
  (game a few units *ahead* of target). So the catch-up-loop continue condition
  (`game + step <= target`) is *never* met -- `sub_8258B3B8`'s own inner
  do-while is not what iterates. Clamping these did nothing (the clamp never
  engaged).
- **Not any stored object field.** Dumping the *screen* object
  (`dword_82E4F80C`, the current-screen singleton) offsets 0..96 during a
  714-tick burst vs a 30-tick window: **byte-for-byte identical.** Same for the
  framework object. **No stored field reflects the burst** -> the frame-skip /
  catch-up count is a *live per-frame computation* (a real-time or GPU-progress
  read each frame), not a counter sitting in either object.

### Refined root-cause hypothesis (unconfirmed)

Missing **GPU backpressure**, native-only. On xenos the guest fills the ring and
blocks on the ring-full wait until the real GPU (own thread, fixed consume rate)
drains it, pacing frame production to ~60fps. Native strips this out two ways:
the `HeadlessRingWaitBypass` mid-asm hooks make the ring waits return instantly,
and `HeadlessWriteRegister` mirrors WPTR->RPTR *instantly* (so the ring always
looks fully drained). With no consumption-rate pacing, the guest is free to
produce frames faster than they present; the game's per-frame catch-up logic --
reading some GPU-progress/real-time signal that native reports wrong -- then runs
a burst of update frames. The ~6s period and why it is intermittent (rather than
constant 400% as in the earlier decouple experiment) is still unexplained.

### Things tried this session and their result

- **Constants-arena fix (#4) reapplied.** Real win: the worst inline-GPU stall
  dropped from ~3797ms to gone in the logs. Does **not** affect the burst.
  **Kept in the tree** (`src/native_command_processor.cpp`: `constants_arena_*`
  members, per-draw suballocation, per-frame reset in `EnsureFrameBegun`, flush
  in `PresentFrame`). Recommend keeping regardless.
- **Headless vblank backpressure gate (SDK, kept in tree).**
  `KernelState::NotifyHeadlessFramePresented()` / `headless_presented_frames_`;
  the vblank thread only dispatches while `vblank_count < presented + 3`, else
  resyncs. `NativeCommandProcessor::PresentFrame` bumps the counter. **No effect
  on the symptom** (guest doesn't pace on the vblank ISR count). Harmless.
- **Paced RPTR write-back (SDK, kept in tree).** Changed the instant WPTR->RPTR
  mirror in `HeadlessWriteRegister` to advance the read-pointer write-back at
  most once per ~16ms, so a guest reading it for GPU progress would see truthful
  backpressure. **No effect** -- so the guest does not pace off the RPTR
  write-back either (or not only). Safe (nothing host-side reads RPTR; the
  native decode walk uses its own cursor).
- **Catch-up cap via forcing `a1+184` = 1** (frame-loop exit flag). This
  **booted the game to the main menu** instead of capping -- `a1+184` is an
  "exit current front-end mode -> main menu" flag, **not** a per-frame-done
  flag. Reverted. Documented separately as a potential `skip_intros` lever in
  `docs/skip-intros-finding.md`.

### Diagnostic tooling left in the tree (temporary -- remove when done)

All in-process, no lldb. Reusable pattern: resolve a live host RIP / stack
return address back to a guest function via the in-memory `PPCFuncMappings`
table (reject overshoots using the gap to the next tabulated entry -- host
functions are ~4x their guest size, so large *host* offsets are still valid).

- `src/nocturnerecomp_hooks.cpp`: `FreezeWatchdogHeartbeat` (hook at
  `sub_825D1618`) -- watchdog thread that `SuspendThread`s the main thread and
  logs its stuck guest function on a >100ms stall (`ReadProcessMemory` for the
  stack scan so an OOB page fails safe); `FrameTickProbe` (hook at
  `sub_8258B3B8`) -- currently a read-only screen-object field dumper +
  ticks/500ms counter; `FrameCounterProbe` (hook at `sub_825AAE90`).
- `nocturnerecomp_config.toml`: the three `[[midasm_hook]]` entries above.
- `src/native_command_processor.cpp`: `[PRESENT]` per-500ms rate log in
  `PresentFrame`.
- rexglue-sdk `src/system/kernel_state.cpp`: `[VBLANK]` per-500ms dispatch-rate
  log in the headless vblank thread.

### Best remaining lead for next time

No stored field drives the burst, so find the **live per-frame read** the
frame-skip logic uses. Prime suspect (GPU-related, native-only, untried):
`ExecutePacketType3_EVENT_WRITE_SHD` (`rexglue-sdk`
`src/graphics/command_processor.cpp:1345`) writes `counter_` (the GPU vblank
counter) into a guest memory address as the GPU-completion fence. The native
path (`NativeCommandProcessor` / `HeadlessWriteRegister`) does **not** handle
`EVENT_WRITE`/`EVENT_WRITE_SHD` at all, and nothing increments a `counter_`
equivalent, so whatever fence the guest polls for "GPU finished frame N" is
never written correctly in native. On xenos it advances at vblank rate. If the
frame-skip decision reads that fence, this is the wrong signal. Next attempt:
have the native path service `EVENT_WRITE_SHD` -- write a present-driven /
vblank-driven counter to its target address -- and see if the bursts stop.
Alternatively, hook the guest's actual per-frame time/GPU-counter read directly
(rather than probing object fields, which is now exhausted).

### Unrelated blocker encountered

The native shader translator hard-fails on some pixel shaders with
`Shader translation fatal error: Unknown ALU vector operation`
(`rexglue-sdk` `src/graphics/pipeline/shader/spirv_translator_alu.cpp:800`, end
of the `switch(instr.vector_opcode)`), which then hangs the game after the
logos. Content-dependent, pre-existing, separate from pacing -- needs the
specific unimplemented Xenos ALU vector opcode identified and added.

## UPDATE (2026-07-13): full frame-pacing chain reverse-engineered (addresses)

This session statically + dynamically resolved the entire game-side pacing
machinery. **Reference for future work -- all addresses verified against
`assets/default.xex` (imagebase 0x82000000, IDA db `assets/default.xex.i64`)
and, where noted, against a live run.**

### The frame counter the game paces on

- `dword_82E4F808` -- the Application singleton (framework object; the `a1`
  passed to `sub_8258B8A0`/`sub_8258B3B8` and stored by `sub_82578A30` at
  `0x82578A68`). Runtime vtable `0x820099C8`.
- `sub_825AAE90` (per-frame) calls **vtable+68** on that singleton, which at
  runtime is **`sub_82581430`** (verified live via probe: `app=4002F7A0
  vtable=820099C8 slot68 -> sub_82581430`; note the *static* slot in the idb
  is a `blr` stub -- the runtime value differs, don't trust the static read).
- `sub_82581430` just returns the global **`dword_82E4FC7C`** -- the game's
  master frame/vblank counter.
- `dword_82E4FC7C`'s only writer is **`sub_82581420`** (`0x82581420`): it
  stores `*(r3)` into the global. It is registered as the **D3D vblank
  callback** during graphics init in `sub_82582360` (via
  `sub_82534A70(device, cb)`, which stores the callback at **device+16168 /
  0x3F28**).
- The callback is invoked from **`sub_82533B48`** -- the game-side D3D
  **vblank ISR handler** (runs off the graphics interrupt = our headless
  vblank thread's `DispatchGraphicsInterruptCallback(0, 2)`). What it does,
  in order (all offsets are dwords into the D3D device struct):
  - `++device[4043]` -- the **vblank counter** (offset 0x3F2C).
  - `mftb -> device[4044]` -- timestamp of the vblank.
  - Drains the **pending-swap ring** `device[4081]` (consume idx) /
    `device[4082]` (produce idx): each pending swap whose scheduled vblank
    `<= device[4043]` is completed -- increments `device[4048]` (the
    **swaps-completed counter**, offset 0x3F40) and writes the new
    front-buffer address to **MMIO 0x7FC86110** (or zeroes
    `*(device[2693]+4)` when the entry has no front-buffer pointer).
  - Calls the registered callback (`device[4042]`, offset 0x3F28) with a
    3-dword struct `{vblank_count, swaps_completed, 0}` -- so the game's
    global counter `dword_82E4FC7C` == **vblank ISR count**, NOT the swap
    count (`sub_82581420` reads `*(r3)` = word 0).

### The catch-up / fast-forward logic itself

- `sub_8258B8A0` (fwmain mode loop; `.\source\fwmain.cpp` per its assert
  string). Per outer frame it runs an inner do-while:
  `while (sub_8258B3B8(a1) returns true) { tick }`.
- `sub_8258B3B8` (the fixed-timestep tick), fields on the Application object:
  - `a1+2236` = **game_time**, `a1+2232` = **target_time** (units: 300ths of
    a second -- one 60Hz frame = 5).
  - `a1+2228` = TV refresh rate (60 for NTSC), `screen+12` (screen obj from
    `dword_82E4F80C`) = the screen's sim rate (observed 60).
  - Logic: if `game > target`, bump `target += 300/refresh` and return
    false. Else run one screen Update (`screen vt+4`), `game +=
    300/(screen+12)`, and **return true (keep catching up) iff game is
    still <= target**. There's also a "more than one frame behind" notify
    (`screen vt+92`) when `game + quantum < target` -- likely the "skip
    rendering this frame" signal.
- `sub_825AAE90` (per-frame): reads the counter global via the virtual call,
  stores it at `framework+168`, computes `delta = new - old` and clamps the
  per-frame advance factor to **1 or 2** (r24). So the vblank-counter path
  can only add at most 2 quanta of target advance per presented frame -- a
  10-900x tick burst CANNOT come from that path alone (confirmed live:
  bursts of 60-956 ticks/500ms happened while `dword_82E4FC7C` jumped by
  more than 3 exactly once in a 3-minute run).

### Guest-side GPU wait primitives (the bypassed hooks), decoded

Both `HeadlessRingWaitBypass` hook sites poll words in a **GPU write-back
block** whose pointer lives at `device+10768` (0x2A10):
- `sub_825252C8` (hooked): ring free-space wait -- polls `*(*(device+10768)+0)`
  = the **ring RPTR write-back** (what `VdEnableRingBufferRPtrWriteBack`
  registers; the SDK's `HeadlessWriteRegister` mirrors WPTR into it).
- `sub_82524330` (hooked): "GPU consumed past address X" wait -- polls
  `*(*(device+10768)+4)`, a second GPU-written progress word (on real HW
  written by the GPU via fence packets; also zeroed by the vblank ISR's
  no-frontbuffer path via `device[2693]` -- note `device[2693]` == the same
  write-back block, i.e. `device+10772` region).
- `sub_825243C8` reads `*(*(device+10768)+60)` -- a third progress word.
- The wait loops' blocking primitive is `sub_8252F608` (arm) /
  `sub_8252F840` (block) / `sub_8252F638` (disarm) -- an event-wait, so
  waits wake on (graphics) interrupts, they don't hard-spin.

### EVENT_WRITE_SHD fence servicing (implemented this session -- kept)

`NativeCommandProcessor::OnPacket` now services `PM4_EVENT_WRITE_SHD`
(mirroring xenos `ExecutePacketType3_EVENT_WRITE_SHD`): writes the packet's
value -- or, when initiator bit 31 is set, `swap_counter_` (incremented once
per swap packet, mirroring xenos `counter_`) -- to the packet's guest
physical address with GpuSwap endianness. Correct to have regardless, **but
it did not fix the burst** (user-verified still happening with it in).

### Discarded this session (measured no effect; do not re-try blindly)

- SDK paced-RPTR write-back (reverted): instant WPTR->RPTR mirroring is
  *truthful* for the native path since decode/forward happens synchronously
  inside the same MMIO write.
- SDK vblank presented-frames backpressure gate + [VBLANK] probe (reverted):
  the guest does not pace on the vblank ISR count (clamped to 2, see above).

### (superseded) burst-onset probing

The `[BURSTTICK]` probe answered the question: during bursts `target-game`
stays **-5** (game one quantum *ahead*), i.e. each `sub_8258B3B8` call runs
exactly one update and exits -- the burst is the **outer mode loop iterating
at 700+Hz without present pacing**, not the inner catch-up do-while. See the
next section for the full causal chain.

## UPDATE (2026-07-13, later): root cause found -- D3D swap-completion
## machinery never serviced headless. Partially fixed; still not resolved.

### The complete causal chain (all confirmed by live counters)

1. The game's mode loop calls the D3D swap wrapper (`sub_82534910` ->
   `sub_82534378`) **every iteration**. Probes: `[FWPRESENT]` / `[D3DSWAP]`
   call rates == `[TICK]` rate always.
2. `sub_82534378` (D3D Swap) does, in order:
   - `++device[4046]` (**swaps submitted**, byte offset 16184);
   - emits the frame's swap packet via `VdSwap(...)` (the packet lands
     *inside the frame's indirect buffer*, not at ring top level --
     `[SWAPDIAG]` proves swaps are only ever found in-IB);
   - **swap throttle**: `if (device+21912 & 4) { arm event; while
     (event_wait() && (device[4046] - device[4048]) >= 0xF); }` where
     `device[4048]` (byte offset 16192) = **swaps completed**. There is also
     a skip path: `if (device[5289] /*+21156*/) skip VdSwap entirely` -- this
     is what makes most loop iterations emit no swap while wedged.
3. `device[4048]` (swaps completed) is only advanced by
   **`sub_82533D40`** (swap-completion routine: `++completed`, writes the
   frontbuffer flip to MMIO `0x7FC86110`, or defers into the pending-swap
   ring `device[4081]/[4082]` drained by the vblank ISR `sub_82533B48`).
4. `sub_82533D40` is invoked by the guest graphics ISR **`sub_82524608`**
   (registered via `VdSetGraphicsInterruptCallback` in `sub_82535FB8`) when
   a **source-1 interrupt** arrives: the ISR reads a callback fn pointer
   from `*(block+16)` and its arg from `*(block+20)`, where `block` =
   `device[2693]` = the **scratch write-back block**. If the slot holds
   `0x0BADF00D` the ISR **traps** ("Unanticipated CPU_INTERRUPT"); if 0 it
   silently ignores.
5. The slot is armed **through the command stream**: `sub_82525390` embeds
   type-0 writes of `{fn=sub_82533D40, arg}` to **SCRATCH_REG4/5** (GPU regs
   `0x057C/0x057D`), which the real CP mirrors into memory at
   `SCRATCH_ADDR + reg*4` (i.e. block+16/+20) per **SCRATCH_UMSK**
   (`0x01DC`) / **SCRATCH_ADDR** (`0x01DD`); then a `WAIT_REG_MEM` handshake
   and a **`PM4_INTERRUPT`** (type-3 opcode `0x54`, payload = cpu mask)
   raise the source-1 interrupt; then a disarm write sets SCRATCH_REG4 =
   `0x0BADF00D`.
6. Headless, none of 3-5 existed: no scratch write-back, no PM4_INTERRUPT
   dispatch -> swaps-completed stuck at 0 -> throttle wedged -> most loop
   iterations skip VdSwap -> no swap packet -> the native renderer's
   16ms/present pacing never engages -> **mode loop free-runs = the burst**.
   The bistable behavior (locks to a clean 60Hz sometimes) happens when
   every iteration's swap goes through and PresentFrame's sleep paces the
   loop directly.

### Fixes implemented this session (all in the working trees, unverified-EOD)

rexglue-sdk (`src/system/kernel_state.cpp`, `include/rex/system/kernel_state.h`,
`src/graphics/packet_disassembler.cpp`):
- **Scratch register write-back** in `HeadlessWriteRegister`: mirrors
  SCRATCH_REG0-7 writes (both direct MMIO and packet-decoded register-write
  actions, top-level and in-IB) to `SCRATCH_ADDR + reg*4` per SCRATCH_UMSK,
  matching `CommandProcessor::WriteRegister`. State in
  `headless_scratch_umsk_/addr_`.
- **PM4_INTERRUPT dispatch** (source 1) from the decode walk, with an
  **armed-slot gate** (only dispatch if `*(scratch block+16)` is a plausible
  guest fn pointer: not 0, not 0x0BADF00D, 4-aligned, `>>28 == 8`) and a
  **deferred queue** (`headless_pending_cpu_interrupts_`) delivered by the
  headless vblank thread once the slot arms (hardware timing is async; our
  synchronous decode reaches the packet "too early" or while disarmed).
- **Disassembler**: `packet == 0` is now a 1-dword pad skip (was misparsed
  as 2-dword type0); unknown type-3 opcodes now *skip by their header count*
  (was: abort the whole walk) -- matching
  `CommandProcessor::ExecutePacketType3`'s default case. Aborting was
  silently dropping the rest of every frame IB containing any unknown
  opcode, including its trailing swap.
- **IB walk hardening**: a packet whose count crosses the IB end aborts the
  walk (misaligned/terminator data -- observed FFFFFFFF fillers and garbage
  headers whose wild counts previously caused misparses that *stomped
  SCRATCH_UMSK/ADDR with garbage* and crashed the guest via a junk callback
  pointer). Also: per-call IB dedup now *walks* duplicate-content IBs and
  forwards their swap packets (dropping them starved present pacing);
  decode cap raised 512 -> 1M packets (it bounded decode, not logging, and
  truncated real frames before their swap).

NocturneRecomp:
- `PM4_EVENT_WRITE_SHD` serviced in `NativeCommandProcessor::OnPacket`
  (writes value or `swap_counter_` to the fence address; `swap_counter_`
  ++ per swap packet, mirroring xenos `counter_`). Correct but did not fix
  the burst by itself.
- The old SDK experiments (paced RPTR, vblank presented-frames gate) were
  reverted as measured-ineffective.

### Where it stands (still broken) and exactly what to look at next

With everything above (log `logs/nocturnerecomp_033.log`):
- No more crashes, and **swaps complete in runs now** (`dev_swaps_done`
  progresses 390 -> 732 -> 1072 -> 1415...) -- but in **large batches**, not
  continuously: the `[INTQ]` probe shows `scratch_addr=0` for long stretches
  with **thousands of deferred interrupts** accumulating (~57/s), then a
  re-arm flushes them all at once (completed jumps by ~340).
- Bursts therefore still happen (user-confirmed in gameplay, and unattended
  ~365-tick windows every ~6s persist).

Open questions, in order:
1. **Why is `scratch_addr` 0 during those stretches?** Log `032` caught
   UMSK/ADDR being overwritten with garbage (`02D00500`, `C80F8000`,
   `C4000000`...) right at the logo transition -- those were misparse
   artifacts (partially fixed by the IB bounds check), but `ADDR=0` /
   `UMSK=0` writes also appear and may be *legitimate* (D3D disabling
   write-back in some phase, e.g. loading/attract) -- in which case the
   game may be arming the slot by **writing block+16 directly with the
   CPU** (no scratch mirror needed) and our armed-slot check on
   `scratch_addr` is wrong to depend on the *register* being nonzero.
   Cache the last nonzero scratch block address instead, or find the block
   address from `device[2693]` directly (device ptr global =
   `dword_82E4FD00`).
2. **The deferred queue must not batch-flush thousands of stale
   interrupts** -- that itself distorts the completed count. Cap pending at
   ~2-3 (drop older; hardware would coalesce level-triggered interrupts).
3. If 1-2 still don't hold: instrument `device[5289]` (byte 21156, the
   VdSwap-skip flag) and the throttle inputs (`device[4046]`, `[4048]`)
   per-window to see which gate is wedging the swap emission.

### Diagnostic inventory currently in the tree (all TEMP -- strip when done)

- NocturneRecomp `src/nocturnerecomp_hooks.cpp` + `nocturnerecomp_config.toml`:
  `FreezeWatchdogHeartbeat` (sub_825D1618), `FrameTickProbe` (sub_8258B3B8:
  `[TICK]` rate + `[BURSTTICK]` burst tracer incl. D3D device counters),
  `FrameCounterProbe` (sub_825AAE90: `[COUNTER]` + one-time vtable resolve),
  `FrameworkPresentProbe` (sub_82582AC0: `[FWPRESENT]`), `D3DSwapProbe`
  (sub_82534910: `[D3DSWAP]`).
- NocturneRecomp `src/native_command_processor.cpp`: `[PRESENT]` per-500ms
  rate log in PresentFrame.
- rexglue-sdk `src/system/kernel_state.cpp`: `[SWAPDIAG]` (swap location +
  IB dedup counters), `[IBABORT]` (walk aborts + hex dump), `[SCRATCH]`
  (UMSK/ADDR/REG4 writes), `[INTQ]` (wedged deferred-interrupt queue).
- Logs go to `logs/nocturnerecomp_NNN.log` (spdlog auto-sequential).

### Key device-struct offsets (D3D device ptr = `*(dword_82E4FD00)`)

| offset (bytes) | dword idx | meaning |
|---|---|---|
| 10768 | 2692 | ptr to GPU write-back block (ring RPTR at +0, progress word at +4, +60) |
| 10772 | 2693 | ptr to scratch/interrupt block (callback fn at +16, arg at +20) |
| 16168 | 4042 | registered vblank callback (game sets `sub_82581420`) |
| 16172 | 4043 | vblank counter (ISR `++`) |
| 16184 | 4046 | swaps submitted (`++` at top of D3D Swap) |
| 16188 | 4047 | swaps processed by `sub_82533D40` |
| 16192 | 4048 | swaps completed (throttle compares vs 4046) |
| 16324/16328 | 4081/4082 | pending-swap ring consume/produce indices |
| 21156 | 5289 | nonzero => D3D Swap skips VdSwap entirely |
| 21912 | 5478 | flag bit 2 enables the swap-throttle wait loop |

## RESOLVED (2026-07-13): root cause was a halved ring-buffer wrap mask

The burst is fixed. Verified: 5+ unattended minutes plus attended gameplay
with zero bursts, every 500ms window at exactly ~30 ticks (60fps), and the
D3D swap counters (submitted / ISR-processed / completed) advancing in
lockstep.

### The actual root cause

`VdInitializeRingBuffer`'s `size_log2` parameter is log2 of the ring size in
**bytes minus 3** -- the real ring is `1 << (size_log2 + 3)` bytes, i.e.
`1 << (size_log2 + 1)` dwords (ground truth: xenia
`CommandProcessor::InitializeRingBuffer`). `KernelState::HeadlessWriteRegister`
treated it as log2 of *dwords* (`1 << size_log2`), so the headless decode
walk saw a ring **half its real size**. Every lap of the real ring, the
walk's wrap mask wrapped early and re-read stale first-half content as fresh
stream. Downstream symptoms of that one bug:

- Stale/shifted `PM4_INDIRECT_BUFFER` jump packets decoded with garbage
  pointers/lengths (observed: "IBs" claiming 150K-930K dwords whose tails
  were inline float vertex data misparsed as packets).
- Those misparses swept garbage over SCRATCH_UMSK/ADDR, breaking the
  swap-completion interrupt machinery (see previous section) -> swaps
  stopped completing -> the D3D swap throttle wedged -> VdSwap skipped ->
  no swap packets -> present pacing disengaged -> **the mode loop free-ran =
  the burst, once per ring lap (~6s)**.
- The intermittent crash at the logo-skip transition (native renderer
  consuming garbage draw/texture packets from the desynced walk).

Fix: `size_dwords = 1u << (size_log2 + 1)` in `HeadlessWriteRegister`
(rexglue-sdk `src/system/kernel_state.cpp`).

### Contributing bug fixed along the way

`PacketDisassembler::DisasmPacket` treated a raw `0x0BADF00D` dword as a
type-0 header (claiming a ~3000-dword count). The real CP
(xenia `ExecutePacket`) skips it as a single dword, like zero padding -- it
is the guest D3D "disarmed" filler and appears in real streams. Fixed in
rexglue-sdk `src/graphics/packet_disassembler.cpp` (`PM4_BADF00D_PAD`).

### Kept hardening (SDK)

- Misparse detector: a decoded packet whose register writes land outside
  the Xenos register file (>= 0x5003) aborts the walk (top-level: skips to
  wptr so the cursor can't wedge; in-IB: stops that IB).
- Scratch-slot armed check uses the last *nonzero* SCRATCH_ADDR
  (`headless_scratch_block_addr_`), since the guest legitimately zeroes the
  register in some phases.
- Deferred PM4_INTERRUPT queue capped at 3 (hardware coalesces
  level-triggered interrupts; batch-flushing a stale backlog distorts the
  swaps-completed count).

### Facts learned (for future decode work)

- The `02D00500`-looking dword is scissor data (`720<<16|1280`), the value
  of `PA_SC_WINDOW_SCISSOR_BR` in a 3-dword type-0 write at base 0x2080 --
  not a packet header.
- The guest performs a legitimate ~721-dword type-0 register-restore sweep
  of 0x500-0x7D0 that covers SCRATCH_REG0-7; scratch writes must NOT be
  filtered by packet size (an earlier attempt did, and broke re-arming).
- Type-3 packet total size is `2 + ((header >> 16) & 0x3FFF)` dwords
  (header + count body dwords, count = field + 1) -- matches both this
  SDK's disassembler and xenia's CP (`assert` at the end of
  `ExecutePacketType3`).

All TEMP diagnostics from this investigation (probes in
`src/nocturnerecomp_hooks.cpp` + their `[[midasm_hook]]` config entries, the
`[PRESENT]` rate log, and the SDK's `[SCRATCH]/[INTQ]/[SWAPDIAG]/[IBABORT]/
[IBDESYNC]/[IBBIGLEN]` logs) were removed after verification.
