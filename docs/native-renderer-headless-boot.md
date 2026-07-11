# Native renderer: getting a headless boot working

Working notes from the first steps toward replacing the general-purpose
xenos GPU plugin with a game-specific Vulkan renderer. Xenos translates
Xbox 360 GPU calls to Vulkan/D3D12 but supports every 360 title, so it's
heavier than a renderer written only for this game needs to be.

The plan is incremental: first get the game running with `--gpu_plugin`
removed entirely — black screen, but audio playing and menus navigable —
before writing a single line of the real renderer. This document is the
log of that first step.

**Status: headless boot works, and phases 1-2 of the follow-on "basic
textures" plan are done.** The game boots fully headless (audio plays,
guest frame loop runs indefinitely), the real intro-sequence PM4 draw
stream can be decoded (see "Phase 1 revisited" below), and a real Vulkan
swapchain is attached to the game window (see "Phase 2" below). Nothing
renders on screen yet — that's Phase 3.

## Where the renderer will hook in

Per guidance from an experienced ReXGlue-SDK-based recomp contact: don't
try to decouple the ring buffer, and don't fight the SDK's plugin
loading — there's already a documented "bring your own renderer" path:

- Set `config.graphics = nullptr` in `OnPreSetup` (see
  `src/nocturnerecomp_app.h`) — this is the actual, intentional way to go
  headless, rather than just omitting `--gpu_plugin` from the CLI (both
  currently produce the same `graphics_system_ == nullptr` state, but the
  explicit form is the documented entry point and doesn't depend on the
  CLI flag being absent).
- Override `OnCreateImmediateDrawer()` to return a custom
  `ui::ImmediateDrawer` subclass once there's a real renderer to back it
  ("detached overlay mode" — see the doc comment on that hook in
  `rex/rex_app.h`). `CreateTexture` must tolerate the GPU device not
  existing yet (the SDK uploads the ImGui font atlas lazily on first
  `Draw`), so per-renderer GPU init has to be lazy, not tied to
  `OnEnterPresenter` (never called in this mode).
- Use `OnPreLaunchModule()` as the last hook before the guest thread
  starts, and `window()->GetNativeWindowHandle()` to get a native handle
  to build a swapchain against.
- The real GPU device struct (created by the game's statically-linked
  D3D9-on-360 layer) is the load-bearing piece: "it will probably not
  match [other recomps'] offsets, but the fields and their function won't
  change." Getting that struct's layout right is expected to be most of
  the work, followed by shader const/upload tracking, texture/surface
  mirrors, and eventually the full resolve/swap/present chain.
- The ring buffer itself doesn't need decoupling — it's xenos-only
  emulation. The task is making sure nothing *depends* on it once xenos
  isn't loaded.

## Investigation: why headless boot doesn't just work

Removing `--gpu_plugin=xenos` (or setting `config.graphics = nullptr`)
doesn't crash, but produces total silence: no audio, no menu input, no
log activity — not even a hang exactly, but a live, CPU-spinning guest
thread doing nothing observable.

### First hypothesis (partially right, not the main blocker)

The guest's vblank interrupt callback (registered via
`VdSetGraphicsInterruptCallback`) is normally pumped by a "GPU VSync"
worker thread that only exists inside `GraphicsSystem`
(`rexglue-sdk/src/graphics/graphics_system.cpp:153-180`). With no
`GraphicsSystem`, nothing ever calls `DispatchInterruptCallback`, so any
guest logic gated on that interrupt firing periodically would stall.

Fixed in the SDK (`rexglue-sdk`, not this repo) by adding a
plugin-independent vblank pump to `KernelState`:

- `KernelState::SetGraphicsInterruptCallback` /
  `DispatchGraphicsInterruptCallback` (`include/rex/system/kernel_state.h`,
  `src/system/kernel_state.cpp`) — stores the callback/user-data and
  dispatches it via `FunctionDispatcher::ExecuteInterrupt`, mirroring what
  `GraphicsSystem::DispatchInterruptCallback` does.
- `KernelState::StartHeadlessVblankThreadIfNeeded` — spins up an
  `XHostThread` ("Headless VBlank") that mimics `GraphicsSystem`'s "GPU
  VSync" thread loop (same `VdQueryVideoMode`-based refresh-rate timing),
  active only when there's no real `GraphicsSystem`.
- `VdSetGraphicsInterruptCallback_entry`
  (`src/kernel/xboxkrnl/xboxkrnl_video.cpp`) now forwards to
  `KernelState` instead of silently discarding the registration when
  headless.

This works (confirmed via logs — the "Headless VBlank" thread starts,
and the callback registration no longer produces the
"no GPU emulation loaded" warning) but **did not fix the silence**. The
real blocker is downstream of this.

### Second hypothesis (confirmed via debugger, root cause)

Attaching lldb to the spinning process
(`lldb -p <pid> -o "thread backtrace all"`) shows the actual guest main
thread ("Main XThread") burning ~100% of a core in a tight loop with no
further host-visible calls — i.e. pure guest computation, not blocked on
a kernel wait. CPU-time sampling confirmed this: ~5s of CPU burned in
~6-9s of wall time on one core.

Getting a symbolized backtrace required a build with debug info (`python
scripts/build.py --debug`, i.e. the RelWithDebInfo CMake preset) — the
release build strips all `sub_XXXXXXXX` guest function symbols. Note:
**the SDK's current per-config DLL packaging has a bug** — see "Known SDK
issue" below — that currently makes a straight `--debug` build not run at
all without manually renaming DLLs. Workaround used for this
investigation:

```
python scripts/deploy-sdk.py --config RelWithDebInfo --project NocturneRecomp
python scripts/build.py --debug
cp out/build/win-amd64-relwithdebinfo/nocturnerecomp.exe nocturnerecomp.exe
cp sdk/bin/rexruntimerd.dll rexruntime.dll
cp sdk/bin/TracyClientrd.dll TracyClient.dll
```

With that, the spin resolves to:

```
sub_8252F840   (nocturnerecomp_recomp.23.cpp:17091, called from ...)
sub_825252C8   (nocturnerecomp_recomp.22.cpp:65348)
sub_82525630   (nocturnerecomp_recomp.22.cpp:65993)
sub_82534378   (nocturnerecomp_recomp.23.cpp:29134)
sub_82534910   (nocturnerecomp_recomp.23.cpp:29387)
sub_82582360   (nocturnerecomp_recomp.26.cpp:53319)
sub_82576C58   (nocturnerecomp_recomp.26.cpp:26116)
sub_82578A30   (nocturnerecomp_recomp.26.cpp:30469)
xstart         (nocturnerecomp_recomp.29.cpp:23134)
```

`sub_825252C8` (`nocturnerecomp_recomp.22.cpp:65320-65358`) contains a
classic GPU-fence wait:

```
loc_82525350:
    sub_8252F840(...)              // returns 1 for ~5000 ticks, then 0 ("give up")
    if (r3 == 0) goto loc_8252537C  // only exits early on timeout
loc_82525360:
    r10 = [r31+10768]              // pointer to a live counter
    r11 = [r31+10780]              // a value (not directly the exit target — see below)
    r9  = r11 - r30                // r30 = some target set earlier by the caller
    r10 = [r10]                    // dereference: the actual counter value
    r11 = r11 - r10
    cmplw cr6, r9, r11
    if r9 >= r11 (unsigned) goto loc_82525384   // exit
    // else loop back to loc_82525350
```

Net exit condition (as far as traced): `counter >= r30`, where `counter`
is whatever's mirrored at the address the guest registered via
`VdEnableRingBufferRPtrWriteBack`, and `counter`'s address is read
indirectly through `[r31+10768]` — `r31` being (per the "D3D device
struct" tip above) almost certainly the D3D device pointer itself.

This is the guest waiting for the GPU's ring-buffer read pointer to catch
up to a write pointer it just submitted (`CP_RB_WPTR`, MMIO register
`0x01C5` in the `0x7FC80000-0x7FCFFFFF` range) — a real GPU thread would
process commands and periodically mirror its read position back to guest
memory via the address from `VdEnableRingBufferRPtrWriteBack`. With no
GPU plugin, nothing ever writes that memory, so the counter never
advances and the wait spins until timing out (the 5000-tick timeout
inside `sub_8252F840` runs some fallback, but doesn't break the caller's
retry loop).

### Fix attempted so far (partial: crash fixed, spin not yet resolved)

Added to the SDK (`rexglue-sdk`), independent of `GraphicsSystem`:

- `KernelState::EnableHeadlessRingBufferWriteBack(ptr, block_size_log2)` —
  stores the write-back pointer when `VdEnableRingBufferRPtrWriteBack` is
  called headless (`src/kernel/xboxkrnl/xboxkrnl_video.cpp`).
- `KernelState::InstallHeadlessGpuMmioIfNeeded()` — registers an MMIO
  handler over the same `0x7FC80000` GPU register range that
  `GraphicsSystem::SetupGuestGpu` would normally own
  (`memory_->AddVirtualMappedRange`), so ring-buffer-related MMIO writes
  land somewhere instead of faulting.
- `KernelState::HeadlessWriteRegister` — on a write to `CP_RB_WPTR`
  (register index `0x01C5`), immediately mirrors the written value back
  into the read-pointer write-back address (`memory_->TranslatePhysical`
  — the pointer from `VdEnableRingBufferRPtrWriteBack`/
  `MmGetPhysicalAddress` is a *physical*, not virtual, guest address; this
  was the source of an access-violation crash in an earlier iteration of
  this fix, since `TranslatePhysical` and `TranslateVirtual` are
  different address spaces here).
- `KernelState::HeadlessReadRegister` — same fallback register reads
  `GraphicsSystem::ReadRegister` provides generically (`RB_EDRAM_TIMING`,
  `RB_BC_CONTROL`, vblank/viewport queries via `VdQueryVideoMode`),
  everything else returns 0.

This resolved the crash (confirmed via lldb: the access violation in
`KernelState::HeadlessWriteRegisterThunk` writing to an invalid address
is gone), but the guest thread lands right back in the **same** wait
loop afterward, at the same call chain. So mirroring the write pointer
into the read-pointer write-back location isn't actually satisfying the
loop's exit condition — either:

- `r30` isn't simply "the write pointer just submitted" (could be a ring
  buffer *space available* check instead, which would need proper
  wraparound-aware math, not just an instant-equal mirror), or
- the `CP_RB_WPTR` MMIO write this loop is waiting on hasn't actually
  happened by the time the loop runs (e.g. it's submitted by a sibling
  code path not yet reached), or
- offsets `+10768`/`+10780` on the D3D device struct (`r31`) mean
  something other than assumed here.

### Resolution: bypass the wait primitives instead of feeding them

Runtime instrumentation (via SDK [[midasm_hook]] config, not by editing
`generated/` directly — those files get overwritten by codegen) settled
the open questions above empirically instead of by further disassembly
guessing:

- **`r31` is *not* simply "the" D3D device pointer, and there is no single
  ring-buffer counter to feed.** `sub_825252C8` is a generic
  "wait until some GPU-driven counter reaches a target" primitive, called
  from many sites with different device/pool pointers and different
  target semantics (absolute ring position in one call site, a small
  block-count delta in another). Logging actual entry args at the hang
  showed `device=0x40039C00 target=7 mode=3` — nowhere near ring-buffer
  scale, and dereferencing `device+10768` gave `0xFFC9B000`, a guest
  *virtual* address completely unrelated to the *physical* write-back
  pointer (`0x1FC9C03C`) registered via `VdEnableRingBufferRPtrWriteBack`.
  So the mirror-on-`CP_RB_WPTR`-write fix above was correctly mirroring
  the *real* ring buffer's read pointer, but this particular wait was
  parked on an entirely different counter that nothing was ever going to
  advance.
- There's also at least one **structurally-duplicated wait loop that
  doesn't call `sub_825252C8` at all** — `sub_82524330` inlines the same
  "poll a GPU counter, retry via `sub_8252F840`" pattern independently
  (found by re-attaching lldb after the first bypass and getting a fresh
  hang backtrace one level further into boot).

Given that, "make the GPU look infinitely fast" (mirroring specific
counters) doesn't scale — there's no way to know how many of these
counters exist or where they live without tracing every call site by
hand. The fix that actually worked is "make ring buffer space always
look available" **at the primitive itself**: both `sub_825252C8` and
`sub_82524330` are guest code whose entire purpose is waiting on GPU
progress. Headless means there is no GPU, so the wait is unconditionally
meaningless — bypass it outright rather than trying to satisfy it.

Implemented as two SDK mid-asm hooks (see [[Mid-ASM
Hooks|../../rexglue-sdk-wiki/Mid-ASM-Hooks.md]] in the wiki) at each
function's entry point, both wired to the same bool hook function:

```toml
# nocturnerecomp_config.toml
[[midasm_hook]]
address = 0x825252C8
name = "HeadlessRingWaitBypass"
return_on_true = true

[[midasm_hook]]
address = 0x82524330
name = "HeadlessRingWaitBypass"
return_on_true = true
```

```cpp
// src/nocturnerecomp_hooks.cpp
bool HeadlessRingWaitBypass() {
  return REX_KERNEL_STATE()->emulator()->graphics_system() == nullptr;
}
```

When a real `GraphicsSystem` (xenos) is loaded, the hook returns `false`
and the original wait logic runs untouched — this only changes behavior
in headless mode.

**Verified this fixes it**, not just "compiles": before the fix, CPU
sampling showed one core pinned near 100% with zero further log output
(a true spin). After, the guest ran its normal frame loop indefinitely —
`HeadlessWriteRegister` (CP_RB_WPTR mirroring) kept firing continuously
frame after frame (log grew from ~170 lines to 16,000+ over a few
seconds), and repeated lldb backtraces of the "Main XThread" showed it
actively moving through real game logic (recursive scene/actor update
calls), not stuck at a fixed PC. `XMP: started BGM playlist` — the same
milestone the working xenos boot reaches — now shows up in headless logs
too, and the game's audio is audible when run.

The debug mid-asm hooks used to get the `device`/`target`/`W`/`R` values
above (`DebugRingWaitEntry`, `DebugRingWaitIter`, `DebugRingWaitPtr`) were
temporary and have been removed; `REXSYS_INFO` logging temporarily added
to `KernelState::EnableHeadlessRingBufferWriteBack` /
`HeadlessWriteRegister` for the same purpose was also reverted. Only the
two `HeadlessRingWaitBypass` hooks and the original ring-buffer-mirroring
fix (`EnableHeadlessRingBufferWriteBack` et al., described above) remain.

One thing not yet confirmed: whether there are *further* GPU-wait
primitives of this shape deeper in the game (e.g. gated behind menu
input or a specific game state not reached during the ~10s boot window
tested). If a future headless run hangs again, the same technique
applies: re-attach lldb to the spinning `Main XThread`, get a fresh
backtrace, and if it's a new function shaped like "poll a counter, retry
via `sub_8252F840`", add another `HeadlessRingWaitBypass` mid-asm hook at
its entry rather than trying to feed it real data.

## Known SDK issue: per-config DLL packaging

Independent of the above: `scripts/build.py`'s `copy_runtime_libs` copies
each shared-lib variant under its suffixed name (`rexruntime.dll` /
`rexruntimed.dll` / `rexruntimerd.dll` for release/debug/relwithdebinfo),
but the built `nocturnerecomp.exe`'s import table references the
**unsuffixed** name (`rexruntime.dll`) regardless of which config it was
built in (confirmed via `llvm-objdump -p` on a RelWithDebInfo build).
So a plain `python scripts/build.py --debug` currently fails at launch
with `rexruntime.dll: cannot open shared object file`. Worked around
during this investigation by manually copying/renaming the `rd`-suffixed
SDK DLLs to their unsuffixed names; a real fix belongs in the SDK's CMake
packaging (making the per-config output name consistent with what
consumers actually link against) and hasn't been made yet.

## Files touched

- `rexglue-sdk` (sibling repo, `development` branch as of this work, work
  committed on a `native` branch):
  - `include/rex/system/kernel_state.h`
  - `src/system/kernel_state.cpp`
  - `src/kernel/xboxkrnl/xboxkrnl_video.cpp`
- This repo (work committed on a `native` branch):
  - `src/nocturnerecomp_app.h` — explicit `config.graphics = nullptr` in
    `OnPreSetup`.
  - `nocturnerecomp_config.toml` — two `[[midasm_hook]]` entries
    (`HeadlessRingWaitBypass` at `0x825252C8` and `0x82524330`).
  - `src/nocturnerecomp_hooks.cpp` — the hook implementation.

Both `native` branches need to stay in sync going forward — the
SDK-side fix without the `config.graphics = nullptr` app-side change (or
vice versa) doesn't get you headless boot; they were developed and
tested together.

## Reproducing this / tools for the next session

Everything below assumes both repos are checked out side by side (as
required by `CLAUDE.md`) and both are on their `native` branch
(`git checkout native` in each). This repo lives at
`c:\Users\<user>\dev\NocturneRecomp`, the SDK at
`c:\Users\<user>\dev\rexglue-sdk` (adjust for your machine).

### Build and run headless (release, no debugging)

```
python scripts/build.py
./nocturnerecomp.exe --game_data_root assets --license_mask=1
```

No `--gpu_plugin` flag. Compare against a normal boot
(`--gpu_plugin=xenos` added back) by diffing `logs/nocturnerecomp_*.log`
— both now reach `XMP: started BGM playlist` shortly after
`KernelState: Preparing module launch...`. `logs/` is gitignored and each
run appends a new numbered file, so `rm -f logs/*.log` before a run keeps
things readable.

### Instrumenting guest code without editing `generated/`

**Do not edit files under `generated/`** to add temporary logging/asserts
— they're `build.py`'s codegen output and get silently overwritten (or,
if codegen decides its inputs are unchanged, silently *kept* on the next
build, which is worse: you'll debug against stale edits without
realizing it). The supported way to inject native code at a specific PPC
instruction is a **mid-asm hook**, documented in the SDK wiki at
`../rexglue-sdk-wiki/Mid-ASM-Hooks.md`. Two-step recipe used throughout
this investigation:

1. Add a `[[midasm_hook]]` entry to `nocturnerecomp_config.toml` (config
   input, survives codegen) naming the guest address and the registers
   you want to inspect/patch, e.g.:
   ```toml
   [[midasm_hook]]
   address = 0x825252C8   # function/instruction address, from lldb backtraces
   name = "MyDebugHook"
   registers = ["r3", "r4", "r5"]
   ```
2. Implement `MyDebugHook` in `src/nocturnerecomp_hooks.cpp` (a real
   source file, not generated) — either a `void` hook for pure logging via
   `REXSYS_INFO(...)`, or a `bool` hook combined with `return_on_true` /
   `jump_address_on_true` etc. to actually redirect control flow (this is
   how the `HeadlessRingWaitBypass` fix above works).
3. `python scripts/build.py --debug` (or without `--debug` for a release
   test) — codegen detects the TOML change and regenerates, wiring the
   hook call into the matching `generated/*.cpp` line automatically.

This is also how the exact hang state was captured empirically instead of
guessed from disassembly: temporary hooks logged `device`/`target`/`W`/`R`
at the wait loop (see git history around this doc's commit for the exact
temporary hooks used, since they've since been removed) rather than
trying to read live PPC-context register values through lldb, which is
*not* reliable here — `PPCContext` is one mutable struct shared across
the whole call stack (all guest functions operate on the same `ctx`), so
`frame select N` in lldb changes which *source line* is displayed but
**not** which register values you read; `ctx.r31` always reflects
whatever the innermost currently-executing guest function is doing with
r31 right now, not what the frame you selected saw at its own call site.
Reading a specific frame's original register values reliably means
either breaking at that function's own entry (before it or its callees
clobber anything) or reading the stack-spilled copies `__savegprlr_*`
wrote — a mid-asm hook at the exact instruction of interest sidesteps the
whole problem.

### Checking if the guest is hung vs. spinning

A hung/blocked thread shows near-zero CPU; a spinning one doesn't. From
PowerShell, while the game is running:

```
powershell -NoProfile -Command "Get-Process nocturnerecomp | Select-Object Id,CPU"
```

Sample twice a few seconds apart — if CPU time climbs at roughly
wall-clock rate (e.g. ~5s of CPU across ~6s of wall time), one thread is
spinning hot, not blocked. `tasklist //FI "IMAGENAME eq nocturnerecomp.exe"`
gets you the PID for the next step. (Bash tool note: this project's Bash
tool runs Git Bash/MSYS — `tasklist`, `taskkill`, and `powershell` all
work directly; just remember MSYS mangles a bare single `/` in flags, so
use `//FI` not `/FI`. Also: `$TMPDIR` is unset in this environment's Bash
tool, so redirecting to `"$TMPDIR/foo.log"` silently writes to
`/foo.log` at the drive root — always use an explicit scratch path.)

### Getting a symbolized backtrace of the spinning/crashing thread

Release builds strip all `sub_XXXXXXXX` guest symbols, so you need a
build with debug info. Because of the DLL packaging bug (below), a
plain `--debug` build won't launch — do this instead:

```
# One-time (or whenever the SDK-side native branch changes):
cd ../rexglue-sdk
python scripts/deploy-sdk.py --config RelWithDebInfo --project NocturneRecomp

# Build NocturneRecomp with debug info, then work around the DLL naming bug:
cd ../NocturneRecomp
python scripts/build.py --debug
cp out/build/win-amd64-relwithdebinfo/nocturnerecomp.exe nocturnerecomp.exe
cp sdk/bin/rexruntimerd.dll rexruntime.dll
cp sdk/bin/TracyClientrd.dll TracyClient.dll
# (rexgpu-xenosrd.dll only matters if you pass --gpu_plugin=xenos; not needed headless)
```

Then run it in the background and attach lldb by PID (lldb lives at
`C:\Users\<user>\scoop\apps\llvm\current\bin\lldb.exe` on this machine —
find yours with `where.exe lldb.exe`):

```
./nocturnerecomp.exe --game_data_root assets --license_mask=1 &
tasklist //FI "IMAGENAME eq nocturnerecomp.exe"     # get the PID

lldb.exe -p <PID> -o "thread backtrace all" -o "detach" -o "quit"
```

Look for the thread named `Main XThread (...)` — that's the guest's
main thread. Its frames will show as `nocturnerecomp.exe\`__imp__sub_XXXXXXXX(...)
at nocturnerecomp_recomp.N.cpp:LINE` once symbols resolve correctly.

To catch a crash live instead of a spin, launch directly under lldb so
it stops automatically on the fault (note the `--` separator before the
game's own args — `settings set target.run-args` chokes on args that
look like lldb options, so use `process launch --` instead):

```
lldb.exe -o "process launch -- --game_data_root assets --license_mask=1" \
         -o "thread backtrace all" -o "quit" ./nocturnerecomp.exe
```

Once done debugging, restore the normal release build (the debug/rd
DLLs left renamed in the project root will otherwise linger and confuse
future runs):

```
rm -f rexruntimerd.dll rexgpu-xenosrd.dll TracyClientrd.dll
python scripts/deploy-sdk.py --project NocturneRecomp   # back in ../rexglue-sdk, Release config
python scripts/build.py                                  # back in NocturneRecomp
```

### Key addresses/constants from this investigation

- Guest code range: `code=82230000-828799CC` (from the
  "Function table initialized" log line).
- The spin: `sub_8252F840` → `sub_825252C8` (wait loop, see disassembly
  above) → `sub_82525630` → `sub_82534378` → `sub_82534910` →
  `sub_82582360` → `sub_82576C58` → `sub_82578A30` → `xstart`.
- D3D device pointer candidate: `r31` in `sub_825252C8`, offsets
  `+10768` (pointer to a live counter) and `+10780` (a value used
  against `r30`, meaning not yet nailed down).
- `CP_RB_WPTR` MMIO register index `0x01C5` (i.e. address
  `0x7FC80000 + 0x01C5*4` within the GPU register range
  `0x7FC80000-0x7FCFFFFF`); see `GraphicsSystem::WriteRegister` in
  `rexglue-sdk/src/graphics/graphics_system.cpp` for the real (xenos)
  handling this headless path is standing in for.
- `VdEnableRingBufferRPtrWriteBack`'s pointer argument is a *physical*
  guest address (from `MmGetPhysicalAddress`), not virtual — use
  `Memory::TranslatePhysical`, not `TranslateVirtual`.

## Phase 1: decoding the headless ring's PM4 command stream

Per the native-renderer phase-2 plan (get intro-logo textures on screen),
Phase 1 was to decode what the intro sequence actually submits to the ring
buffer, to scope how much of a game-specific Vulkan renderer is really
needed. `KernelState::HeadlessWriteRegister`
(`rexglue-sdk/src/system/kernel_state.cpp`) previously just hex-dumped raw
ring dwords on every `CP_RB_WPTR` write; it now decodes them with the SDK's
existing `rex::graphics::PacketDisassembler` (opcode/register names via
`RegisterFile::GetRegisterInfo`), logged as `REXGPU_INFO("headless pm4: ...")`
lines.

**Reuse note — `PacketDisassembler`/`RegisterFile` had to be duplicated into
the always-linked runtime.** They live in `src/graphics/` alongside the rest
of the Xenos GPU format layer, which is built into the `rexgpu-xenos`
plugin — runtime-loaded only when a GPU plugin is active, never linked into
the base `rexruntime` that `kernel_state.cpp` is part of. Linking failed
(`undefined symbol: PacketDisassembler::DisasmPacket` /
`RegisterFile::GetRegisterInfo`) until `register_file.cpp` and
`packet_disassembler.cpp` were added directly to `REXSYSTEM_SOURCES` in
`src/system/CMakeLists.txt`. Both are small, self-contained (no dependency
on `command_processor.cpp` or anything else plugin-only), so duplicating
just these two translation units into the core runtime is safe and narrow —
it does *not* pull in the general xenos plugin machinery the overall plan is
trying to avoid.

**Bugs found and fixed while wiring this up** (both in
`KernelState::HeadlessWriteRegister`):

1. **Decode cursor must be independent of the per-call wptr delta.** The
   guest advances `CP_RB_WPTR` in small increments — often fewer dwords than
   one packet — so restarting the packet walk at each call's "new bytes
   since last call" misaligns mid-packet and cascades garbage for every
   packet after. Fixed by adding a persistent
   `headless_ring_buffer_decode_pos_` cursor (separate from the wptr itself)
   that only advances past *fully* decoded packets, leaving any trailing
   partial packet for a later call once more of it has been submitted.
2. **Out-of-bounds read / crash.** `PacketDisassembler::DisasmPacket` reads a
   type-3 packet's whole body speculatively, based on the header's count
   field, before the caller can check whether that many dwords were even
   copied yet. Copying only the "newly available" span into a local
   `std::vector` therefore let a packet header sitting near the end of that
   span read past the vector — an intermittent segfault (crashed most runs
   within ~15-30s). Fixed by always copying a full-ring-sized window (plus
   `1 + 0x3FFF+1` dwords of padding for the theoretical max packet size,
   wrapping back onto real ring content) — always valid, already-mapped
   guest physical memory, so the speculative read is safe even for a
   not-yet-"available" or malformed body; `available` still gates what's
   actually logged as complete.
3. **Runaway logging.** With no real GPU draining the ring, the guest can
   resubmit a tiny unchanging command in a tight spin (see finding below),
   generating many thousands of log lines per millisecond. Capped total
   logged/decoded packets per process (`kHeadlessPm4LogLimit = 20000`) since
   this is throwaway diagnostic instrumentation, not something that needs to
   run unbounded.

### Finding (superseded): headless boot never reaches the real intro command stream

An earlier capture appeared to show **only** repeating, static
`PM4_INDIRECT_BUFFER`/keepalive traffic for the entire ~20,000-packet
window, with no draws, no shader loads, and no viewport/render-target
register writes — leading to a (wrong) conclusion that GPU submission never
happens headlessly at all. **This was a decoder limitation, not a fact
about the guest.** The decoder only walked the *top-level* ring buffer,
which is dominated by the guest's tight `PM4_INDIRECT_BUFFER` resubmit spin
(nothing drains the ring headlessly, so the guest keeps resubmitting the
same jump); the real per-frame command stream lives *inside* those
indirect buffers and was never actually decoded. See "Phase 1 revisited"
below for the fix and the real finding.

### Phase 1 revisited: the real command stream was inside the indirect buffers all along

`KernelState::HeadlessWriteRegister` now follows each `PM4_INDIRECT_BUFFER`
packet's target and decodes its contents too, logging each distinct target
(deduped by pointer+length, since the same handful of buffers get
resubmitted every spin iteration) only once. Two bugs had to be fixed to
get this working:

1. **Pointer space.** The raw dword in the packet body is a GPU-space
   pointer (see `xenos::CpuToGpu`/`GpuToCpu` and
   `CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER` in
   `rexglue-sdk/src/graphics/command_processor.cpp`) and must be masked to
   the 29-bit physical range (`& 0x1FFFFFFF`) before being treated as a
   `TranslatePhysical` argument — using it raw caused a runaway decode of
   garbage packets.
2. **Unbounded recursion guard.** Added a defensive cap
   (`kMaxIbPacketsLogged = 512`) and a zero-length-packet check on the
   inner decode loop, since this walks guest-controlled data and must not
   be able to spin forever on a corrupt/stale buffer.

**With that fix, the real finding reverses the earlier conclusion: headless
boot *does* reach genuine GPU submission.** The decoded indirect-buffer
content for the intro sequence shows real vertex/pixel shader loads
(`PM4_IM_LOAD`/`PM4_IM_LOAD_IMMEDIATE`), `SQ_PROGRAM_CNTL` and
`SHADER_CONSTANT_FETCH_*` (texture/vertex fetch constants) register writes,
`PM4_LOAD_ALU_CONSTANT`, active blend/depth/color state
(`RB_BLENDCONTROL0-3`, `RB_DEPTHCONTROL`, `RB_COLORCONTROL`), and 10
`PM4_DRAW_INDX` calls. This confirms the intro is not a simple passthrough
quad — Phase 3's native command processor will need the real
`SpirvShaderTranslator` path (translating the actual Xenos microcode found
at the shader-constant addresses), not a hardcoded shader.

## Phase 2: a real Vulkan swapchain via detached `ImmediateDrawer`

Implements `NocturnerecompApp::OnCreateImmediateDrawer()` and
`OnPreLaunchModule()` (`src/nocturnerecomp_app.h`), per the detached-mode
contract documented on `rex::ReXApp::OnCreateImmediateDrawer` in
`rex/rex_app.h`:

- `src/native_immediate_drawer.h` — `nocturne::NativeImmediateDrawer`, an
  `ImmediateDrawer` subclass constructed with no Vulkan state at all
  (`OnCreateImmediateDrawer` is called from `SetupPresentation` right after
  the window opens, well before any GPU device exists). Every virtual
  (`CreateTexture`, `Begin`, `BeginDrawBatch`, `Draw`, `EndDrawBatch`,
  `End`) forwards to an internal `rex::ui::vulkan::VulkanImmediateDrawer`
  that starts out null; `CreateTexture` returns `nullptr` (never crashes)
  until `InitializeVulkan()` has run, satisfying the "font atlas may upload
  before the device exists" contract.
- `OnPreLaunchModule()` builds a `rex::ui::vulkan::VulkanProvider` with
  `with_gpu_emulation=false` (no `CommandProcessor`/xenos guest-GPU
  emulation — just instance/device/samplers) and `with_presentation=true`,
  creates a `Presenter` from it, and attaches that presenter to the game
  window directly via `window()->SetPresenter(...)` — the same mechanism
  `GraphicsSystem::SetupPresentation` uses for the xenos backend, just
  wired manually instead of through `GraphicsSystem`. This gets swapchain
  creation, resize handling, and per-frame presentation for free (the
  `Window`'s own paint loop already calls
  `presenter->PaintFromUIThread()`).
- `VulkanProvider::CreateImmediateDrawer()` returns a fully-working
  `VulkanImmediateDrawer` (the SDK's own texture/pipeline machinery,
  reused rather than reimplemented); `NativeImmediateDrawer::
  InitializeVulkan()` wraps it and calls `SetPresenter()` on it. Since the
  detached-mode `SetupOverlays(nullptr, drawer)` call (in
  `rexglue-sdk/src/ui/rex_app.cpp`) constructs `imgui_drawer()` with a null
  presenter (no device existed yet at that point), it's never auto-
  registered as a UI drawer anywhere; `OnPreLaunchModule` does that
  manually once a real presenter exists
  (`presenter->AddUIDrawerFromUIThread(imgui_drawer(), 0)`), so the
  debug/console/settings overlays still paint.
- `VulkanProvider::CreatePresenter()` must run on the UI thread (mirrors
  `GraphicsSystem::SetupPresentation`'s use of
  `CallInUIThreadSynchronous`); `OnPreLaunchModule` isn't guaranteed to be
  called on the UI thread by the SDK's general contract, so the whole body
  is wrapped in `window()->app_context().CallInUIThreadSynchronous(...)`
  (harmless if already on the UI thread — it just calls through directly).

**SDK-side fix required:** `rexglue-sdk/src/kernel/CMakeLists.txt` linked
`rexui` (which declares `renderdoc` as a `PUBLIC` dependency, for
`rex/ui/renderdoc_api.h`'s `<renderdoc_app.h>` include) as `PRIVATE` into
`rexruntime`, so the `renderdoc` include directory never propagated through
`rexruntime`'s CMake export. Invisible as long as only code built alongside
`rexui` in-tree included `rex/ui/vulkan/*` headers; NocturneRecomp is the
first *external* consumer to include them directly (for
`OnCreateImmediateDrawer`), which surfaced it as a `renderdoc_app.h` file
not found` error. Fixed by re-exporting `renderdoc` `PUBLIC` from
`rexruntime` explicitly.

**Verified working** (checked the log — note the SDK's rotating file sink
means recent short runs can land their early messages in an
`nocturnerecomp_NNN.<n>.log` backup file, not the live
`nocturnerecomp_NNN.log`): `OnPreLaunchModule` runs, the Vulkan
provider/presenter/swapchain all construct with no errors, and the
window's paint loop drives presentation automatically. No visible
on-screen content yet — that's Phase 3, which needs to actually feed the
decoded PM4 draw stream from Phase 1 into this swapchain.

## Phase 3: a minimal native command processor

Per the phase-2 plan (`C:\Users\<user>\.claude\plans\deep-foraging-lamport.md`)
and its phase-3 follow-up (`C:\Users\<user>\.claude\plans\giggly-munching-cook.md`),
this phase is a game-specific PM4 interpreter — new files
`src/native_command_processor.{h,cpp}`, class `nocturne::NativeCommandProcessor`
— that consumes the ring-buffer/indirect-buffer packets Phase 1 decodes,
translates the real shaders found there via `SpirvShaderTranslator`, and
presents through the Phase 2 swapchain on swap. Built in verifiable steps,
each run and checked before moving to the next.

### Milestone 3a: prove the callback -> interpreter -> present loop

`NocturnerecompApp::OnPreLaunchModule` (`src/nocturnerecomp_app.h`)
constructs a `NativeCommandProcessor` after the Phase 2 Vulkan swapchain is
ready, then calls `KernelState::SetNativeGpuCommandCallback` (new SDK hook,
`kernel_state.h:272-274`) to register `NativeCommandProcessor::OnPacket` as
the consumer of every decoded PM4 packet `HeadlessWriteRegister` produces —
including indirect-buffer content, unconditionally, independent of the
debug-log dedup/cap. `OnPacket` folds register-write actions into a local
`rex::graphics::RegisterFile`, and on a `kSwap`-category packet calls
`PresentFrame()`, which clears the Phase 2 swapchain's guest-output image to
a distinctive color via `Presenter::RefreshGuestOutput` (acquire/clear/release
barrier sequence matching `VulkanPresenter::kGuestOutputInternal*`).

Two real bugs surfaced and were fixed here, not hypothetical:
- `submit_fence_` was created unsignaled, but `PresentFrame`'s first step
  waits on it as "the previous frame's fence" — on the very first call there
  is no previous submission, so it hung forever. Fixed by creating it
  `VK_FENCE_CREATE_SIGNALED_BIT`.
- With `HeadlessRingWaitBypass` removing all real GPU backpressure, the
  guest free-ran the frame loop at thousands of iterations/second once
  presenting actually started succeeding, flooding logs and burning CPU.
  Fixed by pacing `PresentFrame` to ~60fps (`std::this_thread::sleep_for`).

**Verified working:** window shows the clear color (not black) and flips
per guest swap; audio still audible.

### Milestone 3b step 1: decode draws and shader loads

`PacketDisassembler` only turns plain register writes into `PacketAction`
entries — `PM4_IM_LOAD`/`_IMMEDIATE` (shader loads) and
`PM4_DRAW_INDX`/`_2` (draws) need their payload dwords re-read directly from
`packet_base`, mirroring the layouts in
`CommandProcessor::ExecutePacketType3_IM_LOAD[_IMMEDIATE]`/
`ExecutePacketType3Draw` (`rexglue-sdk/src/graphics/command_processor.cpp`).
`OnShaderLoad`/`OnDraw` do this and log the first few draws' decoded state.

Confirmed from the log: all of the intro's draws use `source_select=2`
(`xenos::SourceSelect::kAutoIndex` — no real index buffer, 3-4 vertices per
draw), a mix of embedded-immediate and guest-memory shader microcode, and
fetch constant slot 0 is *not* reliably the vertex/texture source for every
draw (some draws' slot-0 decode is garbage) — confirming the real shader
needs to be analyzed to know which fetch-constant slots it actually reads,
rather than assuming slot 0.

### Milestone 3b step 2: translate the real shaders to SPIR-V

Captures the actual microcode bytes (guest/big-endian, from either
`PM4_IM_LOAD`'s guest-memory address or `PM4_IM_LOAD_IMMEDIATE`'s embedded
payload) into `ShaderState::ucode`, then builds `rex::graphics::Shader`
instances, calls `AnalyzeUcode`, and runs them through
`SpirvShaderTranslator::TranslateAnalyzedShader` with the default
modification per stage. Confirmed working end to end: both the intro's
vertex and pixel shader translate to nonempty, error-free SPIR-V
(9328 and 8792 bytes respectively, logged at
`NativeCommandProcessor: shader translation vs_ok=... ps_ok=...`).

**SDK fix required:** `SpirvShaderTranslator`'s public header
(`include/rex/graphics/pipeline/shader/spirv_builder.h`) does
`#include <SPIRV/SpvBuilder.h>` (glslang), but glslang's own install rules
are disabled (`SKIP_GLSLANG_INSTALL`, set when its subdirectory is added —
`thirdparty/CMakeLists.txt:328`) and nothing installed its headers in their
place, even though the glslang/SPIRV *library targets* were already
exported. Any downstream project consuming the installed SDK failed to
compile the moment it included `spirv_translator.h`. Fixed by adding an
`install(DIRECTORY thirdparty/glslang/SPIRV/ ...)` block to
`cmake/rexglue_install.cmake`, matching the existing SPIRV-Tools headers
install right above it (only `SPIRV/` is actually needed — confirmed by
grepping the translator/builder `.h`/`.cpp` files for glslang includes).

### Milestone 3b step 3: descriptor sets, pipeline, real draw plumbing

Builds the fixed Vulkan resources `SpirvShaderTranslator`'s SPIR-V output
expects, replicating (not reusing — no `CommandProcessor`/`PipelineCache`
dependency) the layout the SDK's own Vulkan backend builds in
`src/graphics/vulkan/command_processor.cpp`:

- **Set 0** (`kDescriptorSetSharedMemoryAndEdram`): one `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`
  binding over a 512 MB host-visible buffer (`shared_memory_buffer_`)
  mirroring guest physical memory 1:1 by byte offset — translated shaders
  read vertex data via raw SSBO loads computed from the guest vertex fetch
  constants, not `VkVertexInputAttributeDescription` bindings (confirmed:
  `src/graphics/vulkan/pipeline_cache.cpp` uses an all-zero
  `VkPipelineVertexInputStateCreateInfo` for every guest draw). Only the
  byte ranges a draw's shader actually references get copied in
  (`UpdateSharedMemory`, driven by `Shader::vertex_bindings()`'s
  `fetch_constant` indices), not a blanket copy.
- **Set 1** (`kConstantBufferSystem/FloatVertex/FloatPixel/BoolLoop/Fetch`):
  5 uniform buffer bindings, binding index == `SpirvShaderTranslator::
  ConstantBuffer` enum value. `SystemConstants` is mostly zeroed (just
  `kSysFlag_XYDividedByW` and identity `ndc_scale`) rather than the real
  backend's full `UpdateSystemConstantValues` population — a known
  simplification, not yet verified correct for this game's actual draws.
  Float constants are tightly packed in ascending register order per
  `Shader::ConstantRegisterMap::float_bitmap` (only registers the shader
  actually reads); bool/loop and fetch constants are copied in full,
  unconditionally (small, fixed size), matching `UpdateBindings` in the
  real backend exactly.
- **Sets 2/3** (vertex/pixel textures): a single shared empty
  (0-binding) `VkDescriptorSetLayout`/`VkDescriptorSet` for both slots —
  texture upload/sampling isn't implemented yet, so every shader is treated
  as sampling nothing. **This will produce wrong output once a shader that
  does sample a texture actually draws** (the intro's shaders may well be
  among them); it's a known gap, not an oversight.
- Pipeline: no vertex input state (see set 0 above), dynamic
  viewport/scissor, no depth/stencil, no blending, no culling (winding
  convention unverified). One `VkPipeline` cached per (shader pair,
  primitive topology).
- A frame's draws accumulate into an offscreen `color_target_image_`
  (`VK_FORMAT_A2B10G10R10_UNORM_PACK32`, matching
  `VulkanPresenter::kGuestOutputFormat` exactly so the final copy needs no
  conversion) via one render pass begun lazily on the first draw
  (`EnsureFrameBegun`) and ended in `PresentFrame`, which then copies it
  into the swapchain's guest-output image and presents. Two Vulkan
  functions assumed available turned out not to be in this SDK's exposed
  function table (`vkCmdBlitImage`, then `vkCmdCopyImage`) — the final copy
  is relayed through a staging buffer via `vkCmdCopyImageToBuffer` +
  `vkCmdCopyBufferToImage` instead.
- Each draw gets fresh, dedicated buffer objects for its 5 constant buffers
  (`TransientBuffer`) rather than reusing one buffer across draws: every
  draw in a frame is *recorded* before any of them execute (the whole frame
  submits once, on swap), so reusing+overwriting one buffer between draws
  would make every draw read whichever draw's constants were written last.
  Freed once `EnsureFrameBegun`'s fence wait (at the start of the *next*
  frame) confirms the GPU is actually done with them.

**Known, intentional gap (at the time of this step):** `rex::graphics::xenos::
PrimitiveType::kRectangleList` (prim_type 8) and `kQuadList` (prim_type 13)
aren't native Vulkan primitives. Every one of the intro's 10 draws uses one
or the other, so `TryDraw` skipped all of them (logged, rate-limited) rather
than guessing at an incorrect topology mapping — **nothing draws yet**, the
window still shows milestone 3a's flat clear color. The SDK's real backend
handles rectangle lists via `Shader::HostVertexShaderType::
kRectangleListAsTriangleStrip` (changes the shader modification and host
vertex/topology convention — not yet researched here) and presumably quad
lists via a host-synthesized triangle-list index buffer (standard
technique, no translator changes needed, but new code). Confirmed no crash,
no hang, clean present with all draws skipped. **Both `kQuadList` and
`kRectangleList` are handled as of the steps below.**

### Milestone 3b step 4: quad-list support

`TryDraw` now handles `PrimitiveType::kQuadList` (prim_type 13) instead of
unconditionally skipping it: for a quad-list draw with `num_indices` quad
vertices, it host-synthesizes a `VK_INDEX_TYPE_UINT32` index buffer (two
triangles per quad, `0,1,2,0,2,3`) into a fresh `TransientBuffer` and issues
`vkCmdDrawIndexed` with `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`, instead of the
non-indexed `vkCmdDraw` standard-topology draws use. No translator or
`HostVertexShaderType` changes needed — the vertex shader still reads vertex
`0..num_indices-1` via the shared-memory SSBO exactly as a non-indexed draw
of the same quad list would.

Enabling these draws (previously always skipped) surfaced two real bugs, not
hypothetical:
- The transient descriptor pool was sized for 64 draws/frame, sufficient
  when quad-list draws were unconditionally skipped. With them now actually
  drawing, a single frame exceeded that and most draws silently failed with
  "transient descriptor pool exhausted". Fixed by sizing it for 4096
  draws/frame.
- One specific draw in the intro's stream has a garbage-decoded
  `num_indices` (same root cause as the known fetch-constant/register decode
  unreliability noted in step 1) that, left unchecked, made the
  host-synthesized index buffer balloon to hundreds of MB. Fixed with a
  65536-index sanity cap, matching the existing pattern of skipping
  (rate-limited log) rather than guessing at corrupt data.

**Known gap surfaced, not yet fixed:** that same garbage-decoded draw's
shader ucode, once actually reached (rather than skipped as an unsupported
primitive), makes `SpirvShaderTranslator`/`vkCreateGraphicsPipelines` take
roughly 20 seconds for that one shader — stalling the frame loop, though not
hanging or crashing it. A `size_dwords > 4096` sanity cap on shader loads
(real intro shaders are ~2300 dwords) was added defensively but did not
catch this particular case, meaning the pathological input is a
plausible-sized but structurally-corrupt ucode, not an oversized one. Not
chased further here — same underlying decode-reliability issue as step 1,
and out of scope for quad-list support specifically.

**Verified:** quad-list draws now reach `vkCreateGraphicsPipelines`
successfully (`topology=3`/`VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` pipelines
created, logged) and no longer hit "transient descriptor pool exhausted".

### Milestone 3b step 5: rectangle-list support

`TryDraw` now handles `PrimitiveType::kRectangleList` (prim_type 8) too,
replicating what the SDK's real Vulkan backend does
(`PrimitiveProcessor::InitializeCache`/`Process` in
`rexglue-sdk/src/graphics/primitive_processor.cpp`) rather than guessing:
unlike `kQuadList`, this genuinely needs a different vertex shader
translation, not just a host-side index remap, because the guest only
provides 3 vertices per rectangle — the 4th corner has to be reconstructed
as `v0 + v2 - v1` by the shader itself.

- `GetOrTranslateShader` gained a `Shader::HostVertexShaderType` parameter
  (default `kVertex`), threaded into
  `SpirvShaderTranslator::GetDefaultVertexShaderModification` and folded
  into the shader cache key (a shader used both as a plain draw and a
  rectangle-list draw needs two distinct translations, not one aliased
  onto the other). For rectangle-list draws, `TryDraw` requests
  `HostVertexShaderType::kRectangleListAsTriangleStrip` — the SPIR-V that
  comes back already contains the synthetic-index decode and parallelogram
  reconstruction, so this step only has to feed it the right index stream,
  not implement any vertex math itself.
- `GetOrCreatePipeline` gained a `primitive_restart_enable` parameter
  (folded into its cache key too): rectangle lists draw as
  `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP` with primitive restart enabled,
  not a plain list.
- The index buffer follows `PrimitiveProcessor`'s exact pattern: for guest
  rectangle count `num_indices / 3`, emit a `0xFFFFFFFF` restart index
  before every rectangle but the first, then 4 synthetic indices
  `(i<<2)+0..3` per rectangle (host vertex position within the pair in the
  low 2 bits, rectangle index in the rest) — decoded back into
  `(primitive_index, vertex_in_primitive)` inside the translated shader,
  exactly matching `spirv_translator.cpp`'s rectangle-list vertex-index
  math.
- `draw_with_synthesized_indices`, a small lambda, now backs both the
  quad-list and rectangle-list draw paths (upload a `uint32_t` index vector
  to a fresh transient buffer, bind, `vkCmdDrawIndexed`) instead of
  duplicating that logic per primitive type.

**Verified:** rectangle-list draws reach `vkCreateGraphicsPipelines`
successfully (`topology=4`/`VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP` pipelines
created, logged) with no crash. Full end-to-end pixel correctness (a
presented frame with real rectangle-list content) wasn't confirmed in this
step — see the pathological shader translation time gap below, which the
same test run hit before reaching a clean present.

### Milestone 3b step 6: bound the pathological shader-translation stall

Mitigated (not root-caused) item 1 below: `GetOrTranslateShader`
(`src/native_command_processor.cpp`) now refuses to translate a *new*
distinct shader once `shader_cache_` already holds `kMaxShaderCacheEntries`
(64) entries — logged once via `REXGPU_ERROR("...shader cache exceeded...")`
and the draw is dropped (same code path as any other unsupported-draw case),
rather than paying the ~20s `SpirvShaderTranslator`/`vkCreateGraphicsPipelines`
cost unboundedly. Real intro content only ever needs a handful of distinct
shaders, so 64 is generous headroom before treating further "new" shaders as
the known-garbage case. Root cause (why that one draw's ucode/address decode
is unstable across resubmits) is still open — see item 1 below, now
re-scoped as "fix the root cause" rather than "stop the stall", since the
stall itself is now bounded.

Verified via a fresh headless run after redeploying the SDK (the local `sdk/`
checkout was stale relative to `rexglue-sdk`'s `native` branch and needed
`deploy-sdk.py` re-run to pick up the glslang/renderdoc install fixes from
Phase 2/3b step 2): build succeeds, and the log now shows real draws
executing end-to-end — `NativeCommandProcessor: ready`, multiple
`created a graphics pipeline` lines (topology=3 and topology=4, i.e. both
plain and rectangle-list draws), `first real draw issued`, and
`first frame presented (had_draws=true)` — all within the first second of
boot, with `XMP: started BGM playlist` still reached normally afterward. The
shader-cache cap wasn't exercised in this run (didn't hit the pathological
draw in the ~30s window tested), so it's confirmed as a safe no-op in the
common case but not yet confirmed to trigger correctly against the
pathological input itself.

### Milestone 3b step 7: texture upload (narrow scope)

Adds real texture sampling for a deliberately small subset of formats/layouts
instead of the previous hardcoded-empty texture descriptor sets.

- **Descriptor layout correction, not just population.** Investigated how
  `SpirvShaderTranslator` actually expects textures bound (it doesn't use
  `COMBINED_IMAGE_SAMPLER`): sets 2/3 (`kDescriptorSetTexturesVertex`/
  `kDescriptorSetTexturesPixel`) get separate `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`
  and `VK_DESCRIPTOR_TYPE_SAMPLER` bindings, with the *binding index* being a
  compacted, dedup'd position in the shader's own `texture_bindings()`/
  `sampler_bindings()` (keyed by `(fetch_constant, dimension, is_signed)`),
  not the guest fetch-constant slot directly. This renderer only supports one
  texture per shader stage for now, so `texture_layout_`
  (`native_command_processor.h`) is a single fixed 2-binding layout (0=image,
  1=sampler) reused for both "samples a texture" and "doesn't sample
  anything" shaders — a shader with zero `texture_bindings()` simply never
  statically references the binding, so binding *something* valid (a 1x1
  default white texture, `default_texture_set_`) there is harmless and avoids
  needing multiple pipeline-layout variants.
- **`GetOrUploadTexture`** (`native_command_processor.cpp`) decodes
  `RegisterFile::GetTextureFetch(fetch_constant_index)` and only proceeds for
  a narrow allow-list: `DataDimension::k2DOrStacked` and not stacked, not
  tiled, no packed mips, `mip_min_level == mip_max_level == 0`, format
  `TextureFormat::k_8_8_8_8`. Anything else falls back to the default texture
  with a rate-limited log, matching the "skip with a log" pattern used
  elsewhere in this file for unsupported primitives/formats — deliberately
  not guessing at tiling (`texture_util::GetTiledOffset2D`/`GetGuestTextureLayout`,
  investigated but not wired up yet) or compressed-format decode.
  For the supported case: reads guest memory via `Memory::TranslatePhysical`,
  using the fetch constant's `pitch` (texels>>5) for row stride, byte-swapping
  each texel dword when `Endian::k8in32` (the common case for RGBA8 textures),
  and uploads via a new `UploadTexelsAndTransition` one-shot command
  buffer/staging-buffer helper into a `VK_FORMAT_R8G8B8A8_UNORM` `VkImage`.
  Cached by a hash of the fetch constant's raw dwords, capped at
  `kMaxTextureCacheEntries = 64` (same rationale/pattern as step 6's shader
  cache cap) — a known limitation of this cache key: a guest that overwrites
  texture *content* in place without changing the fetch constant itself would
  incorrectly keep serving the stale upload; not hit by the intro's content.
- `TryDraw` resolves each stage's first `texture_bindings()` entry (if any)
  to an uploaded texture's descriptor set, or the default set otherwise, and
  binds sets 2/3 accordingly instead of always binding the same empty set.

**No SDK-side changes needed** — `rex::graphics::texture_util` (format/tiling
helper functions) turned out to already be linked into the always-loaded
`rexruntime` via `rexcore` (an `OBJECT` library `rexruntime` already pulls
in), discovered by an initial attempt to duplicate `pipeline/texture/util.cpp`
into `REXSYSTEM_SOURCES` (the established pattern for `register_file.cpp` et
al.) failing with duplicate-symbol linker errors — reverted once that showed
the functions were already available via the installed headers.

**Verified:** a real headless run still reaches `XMP: started BGM playlist`
with no crash/hang, and the texture path is genuinely exercised — the log
shows real `skipping texture upload for unsupported fetch constant` entries
with concrete decoded field values (e.g. `tiled=1 packed_mips=1 format=20`,
`format=26`, `format=3`), confirming the intro's actual fetch constants are
being read correctly but don't happen to use the one format
(`k_8_8_8_8`/format 6) this step supports yet — so nothing renders
differently on screen from step 6 in this run.

### Milestone 3b step 7 follow-up: k_1_5_5_5/k_4_4_4_4 support

`dumps/textures/` (real texture dumps from a working xenos run, filenames
encode the decoded dimensions/format, e.g.
`165f453a5f35c459_2048x1024_k_1_5_5_5.png`) settled what step 7 left open:
the intro's non-`k_DXT4_5` content uses `k_1_5_5_5` (format 3) and
`k_4_4_4_4` (format 15), not `k_8_8_8_8`. `format=20` in the earlier log is
`k_DXT4_5` (block-compressed, and tiled/packed-mips in that log line —
correctly rejected, still unsupported) and `format=26` is `k_16_16_16_16`
(also still unsupported, low priority per the dumps).

`GetOrUploadTexture` now unpacks both 16-bit packed formats to RGBA8 on the
CPU (bit-replication expansion, e.g. 5-bit `x` → `(x<<3)|(x>>2)`) using the
D3DFMT_A1R5G5B5/A4R4G4B4 bit layouts, byteswapping 16-bit texels for
`Endian::k8in16` the same way the existing `k_8_8_8_8` path byteswaps 32-bit
texels for `Endian::k8in32` — both formats then reuse the existing
`R8G8B8A8_UNORM` upload path unchanged, no new Vulkan image format needed.

**Verified:** the log now shows `NativeCommandProcessor: first real texture
uploaded (2048x1024)`, matching
`dumps/textures/165f453a5f35c459_2048x1024_k_1_5_5_5.png`'s dimensions
exactly — real guest texture content (not just fetch-constant metadata) is
reaching the GPU for the first time. Still no crash/hang, BGM still starts.
Pixel-level correctness of the sampled output wasn't visually confirmed in
this pass (no interactive screenshot taken), and most of the dumped textures
are still `k_DXT4_5` and therefore still rejected — see "Next".

### Milestone 3b step 8 (in progress): the "blue screen" investigation — real draws still rasterize nothing

After step 7, the user reported the game window still shows only the flat
clear color, never any of the intro's actual geometry, despite the log
showing real draws/pipelines/textures all succeeding. This step is a deep
dive into why, and ends with real fixes plus a still-open root cause.

**Two real bugs found and fixed:**

1. **Missing explicit memory flushes.** Every host-visible buffer this
   renderer writes via `memcpy` (the persistent shared-memory SSBO in
   `UpdateSharedMemory`, per-draw constant buffers, synthesized index
   buffers, texture staging buffers) was never explicitly flushed.
   `rex::ui::vulkan::util::MemoryPurpose::kUpload` only guarantees
   host-*visible* memory, not host-*coherent* — `ChooseHostMemoryType`
   (`sdk/include/rex/ui/vulkan/util.h`) picks by cached/uncached preference
   and never checks the coherent bit. Added `FlushMapped` (a small
   `vkFlushMappedMemoryRanges` helper, whole-range for the short-lived
   per-draw buffers) and switched the persistent shared-memory buffer to the
   SDK's own `rex::ui::vulkan::util::FlushMappedMemoryRange` (which handles
   `nonCoherentAtomSize` alignment correctly for a sub-range flush, needed
   since that buffer is 512 MB and can't be whole-range-flushed every draw).
   The debug-readback path added below also needed the opposite direction
   (`vkInvalidateMappedMemoryRanges` before a CPU read of GPU-written
   memory), also missing before. **On this specific NVIDIA GPU this turned
   out to be a no-op** (confirmed via `debugClamp`-style testing — see
   below) — host-visible memory types on this device are actually coherent
   — but it's a real, previously-silent correctness bug worth having fixed
   regardless, and may matter on other GPUs/drivers.
2. **`SystemConstants` were a hardcoded, wrong approximation.** The
   original milestone 3b step 3 code set `ndc_scale = (1,1,1)`,
   `flags = kSysFlag_XYDividedByW` unconditionally, regardless of what the
   guest's actual `PA_CL_VTE_CNTL`/`PA_CL_CLIP_CNTL`/viewport registers said.
   Replaced with real register-driven values in `TryDraw`
   (`native_command_processor.cpp`), replicating (at reduced generality — no
   resolution scaling, no MSAA) the two branches of
   `draw_util::GetHostViewportInfo` (`rexglue-sdk/src/graphics/util/draw.cpp`)
   — that function itself isn't callable directly since `draw.cpp` also
   pulls in the plugin-only `TextureCache`/`TraceWriter` headers this
   renderer deliberately doesn't link. Also fixed
   `kSysFlag_ZDividedByW`/`kSysFlag_WNotReciprocal`/`kSysFlag_PrimitivePolygonal`
   to be register/topology-driven instead of omitted. Confirmed via logging
   (`sysconst draw#N ...`) that the intro's actual draws decode as
   `clip_disable=1` (the "screen-space UI quad" case) with a computed
   pixel→NDC transform that, checked by hand against the guest's actual
   vertex data (also logged), is mathematically correct — e.g. a `draw#2`
   full-screen rect-list quad's three vertices `(-0.5,-0.5)`,
   `(1279.5,-0.5)`, `(1279.5,719.5)` map to almost exactly `(-1,-1)`,
   `(1,-1)`, `(1,1)` NDC.

**New tooling: a real screenshot-equivalent debug capture**
(`NativeCommandProcessor::DebugDumpColorTarget`,
`native_command_processor.{h,cpp}`) — reads back the offscreen color target
to a raw RGBA (`A2B10G10R10_UNORM_PACK32`) file under `logs/` for the first 3
presented frames with real draws, via a one-shot readback buffer + explicit
`vkInvalidateMappedMemoryRanges`. Converted to PNG for inspection with a
small numpy/PIL script (not checked in — ad hoc per session). **This is how
the investigation below was actually driven**, instead of guessing from
source review alone: it gave a ground-truth answer to "did anything actually
change on screen" independent of what the log claims. Gated to 3 dumps,
harmless to leave in, but should eventually be removed or made
`REXCVAR`-gated once the root cause is fixed and this stops being needed
every session.

**Decisive finding: even hand-injected, provably-correct vertex data
produces zero rasterized pixels.** After the two fixes above didn't change
the outcome, temporary debug code was added directly in `TryDraw` (still in
the tree, guarded by `draws_logged_ == 2` / `== 6`, clearly marked
`// TEMPORARY diagnostic` — **should be removed once the real bug is
found**) that overwrites the guest's actual vertex data in shared memory
with a hand-constructed, full-screen-covering triangle (for a rect-list
draw) and quad (for a quad-list draw) just before the draw executes,
bypassing any question of whether the *guest's* data or *this renderer's
interpretation* of it is correct. **Neither test produced any visible
pixels** in the debug dump, across two different shader-side code paths
(rect-list's `IsSpirvRectangleListVertexLoopEnabled()` GLSL-loop
reconstruction vs. quad-list's plain host-synthesized-index path). This
rules out: vertex/NDC math, guest data decode correctness, and anything
primitive-topology-specific. The failure is generic, downstream of "a
correctly-created pipeline was fed indisputably-correct vertex data."

**Structural comparison against the real Vulkan backend, audited by hand,
found no smoking gun:** descriptor set/binding indices for the 5 constant
buffers (`kConstantBufferSystem`=0 through `kConstantBufferFetch`=4, matches
`command_processor.cpp`'s `constants_binding.binding = i` exactly); shared
storage-buffer binding count (`SpirvShaderTranslator::
GetSharedMemoryStorageBufferCountLog2` — logged as
`max_storage_buffer_range=4294967295 shared_memory_binding_count=1`,
confirming the single-buffer setup in `InitializePipelineResources` is
correct for this device, not an array-of-N mismatch); `vkCmdBindDescriptorSets`
happens after (not before) the corresponding `vkUpdateDescriptorSets` calls
in code order (irrelevant to correctness per spec — GPU reads descriptor
contents at submission, not recording — but checked anyway); rectangle-list
vertex index decode (`primitive_index = gl_VertexIndex>>2`,
`vertex_in_primitive = gl_VertexIndex&3`) matches the synthesized index
buffer scheme in `TryDraw`'s `draw_with_synthesized_indices`. Also
investigated and ruled out: `depthClampEnable` (this device's
`VkPhysicalDeviceFeatures.depthClamp` is `false`, so the fix attempted here
is a no-op — moot, not a real candidate on this GPU).

**Not yet available: Vulkan validation layers.** Passing
`--vulkan_validation_enabled=true` (a real cvar,
`rexglue-sdk/src/ui/vulkan/vulkan_provider.cpp`) produced no validation
output at all — `VK_LAYER_KHRONOS_validation` isn't installed on this
machine, only vendor/overlay layers (NV Optimus, OBS hook) are present. This
is the natural tool for "pipeline looks right on paper but silently does the
wrong thing" bugs and hasn't been tried yet.

**New tooling for next session: a working `renderdoc-mcp` server.**
Installed [Linkingooo/renderdoc-mcp](https://github.com/Linkingooo/renderdoc-mcp)
(cloned to `../.tools/renderdoc-mcp` — sibling of this repo, not inside it) —
but the scoop-installed RenderDoc (and even the official renderdoc.org
Windows zip/installer) **does not ship `renderdoc.pyd`**, confirmed against
RenderDoc's own docs: it's only generated by source builds, "not included in
distributed builds," and explicitly called out as "not supported" as a
standalone module. Built it from source instead:

- Cloned `baldurk/renderdoc` tag `v1.45` (matching the installed version) to
  `../.tools/renderdoc`.
- The repo bundles everything needed for the *default* Python 3.6 build
  (`qrenderdoc/3rdparty/python`, `qrenderdoc/3rdparty/swig`) — no external
  SWIG/Python install needed for that path.
- Built `qrenderdoc/Code/pyrenderdoc/pyrenderdoc_module.vcxproj` directly via
  MSBuild (not the full `renderdoc.sln`, which pulls in every IHV/driver
  project and Qt) with `-p:PlatformToolset=v143` (repo targets VS2015's
  v140, only VS2022 Build Tools are installed here) — this produced a
  Python-3.6-ABI `renderdoc.pyd`, confirmed working
  (`import renderdoc; renderdoc.GetVersionString()` → `1.45`) against a
  freshly-installed Python 3.6.8 (`C:\Python36`, installed standalone,
  `PrependPath=0` so it doesn't affect the system default).
- **But the `mcp` Python SDK package requires Python ≥3.10 at every released
  version back to 0.9.1** — no version supports 3.6, so `renderdoc-mcp`
  cannot run under the 3.6 interpreter its own `renderdoc.pyd` needs.
  Resolved by rebuilding the module a second time against the *system*
  Python 3.12 instead: `pyrenderdoc_module.vcxproj`'s `python.props` supports
  a custom Python via the `RENDERDOC_PYTHON_PREFIX64` environment variable
  (checks `include\Python.h` + `pythonMAJMIN.zip` + `libs\pythonMAJMIN.lib`
  existence; the `.zip` only needs to *exist* for this build-time check, an
  empty placeholder is fine — it's not what's loaded at runtime once a real
  `python.exe` is doing the importing). Set up
  `../.tools/py312_override/{include/,libs/python312.lib,python312.zip}`
  copied/touched from the system `C:\Python312` install, rebuilt with
  `RENDERDOC_PYTHON_PREFIX64` set — MSBuild confirmed
  `Built against python from ...py312_override`, and the resulting
  `renderdoc.pyd` imports cleanly under plain `python` (3.12).
- Runtime files (`renderdoc.pyd` + `renderdoc.dll`, the pyd's own
  dependency) live in `../.tools/renderdoc_runtime/` — a small directory
  built just for this, not the full build output tree.
- Registered via `claude mcp add renderdoc -e RENDERDOC_MODULE_PATH="...\renderdoc_runtime" -- python -m renderdoc_mcp`
  (project-local scope) — confirmed `claude mcp list` shows it Connected.
  MCP tools only load at the start of a session, so it wasn't usable within
  the session that set it up; the next session should have `renderdoc-mcp`
  tools available directly.

## Next

In rough order of what unblocks visible output soonest:

1. **Use `renderdoc-mcp` to find the "nothing rasterizes" root cause.**
   Capture a frame from `nocturnerecomp.exe` (RenderDoc's inject/capture
   wrapper — `renderdoccmd.exe capture` or launching under `qrenderdoc.exe`,
   both available at `C:\Users\<user>\scoop\apps\renderdoc\current`), open
   the `.rdc` via the new MCP tools, and inspect the actual pipeline state
   and post-vertex-shader output for one of the early draws (`draw#1`/`#2`
   are simplest — small shaders, already-understood vertex layout). This is
   the highest-leverage next step: step 8's investigation exhausted what
   paper-auditing against the real backend's source could find, and proved
   (via hand-injected known-good vertex data) that the bug is generic
   Vulkan-plumbing-level, not shader/data-specific — exactly the class of
   bug RenderDoc's mesh viewer and pipeline state inspector are built to
   surface directly instead of by inference. Once found, **remove the
   `TEMPORARY diagnostic` vertex-data-override blocks and the verbose
   per-draw disasm/vfetch/sysconst logging** added during this
   investigation (all clearly marked in `native_command_processor.cpp`) —
   they were debugging aids, not permanent features.
2. **`k_DXT4_5` support (block decompression + tiling).** Confirmed via
   `dumps/textures/` to be the dominant format in the intro's actual content
   (majority of the 22 dumped textures), and it's tiled with packed mips in
   the observed fetch constant, so this needs both a DXT5/BC3 block decode
   (CPU-side unpack to RGBA8, same pattern as the 16-bit formats, or upload
   natively as `VK_FORMAT_BC3_UNORM_BLOCK` if the device supports it) and
   real tiled-address math via `texture_util::GetTiledOffset2D`/
   `GetGuestTextureLayout` (already linked, not yet called) instead of the
   linear-row assumption the current three supported formats use. Blocked
   behind item 1 — no point getting texture data right while nothing
   rasterizes at all.
3. **Pathological shader translation time — stall now bounded, root cause
   still open.** One specific garbage-decoded draw's shader ucode makes
   `SpirvShaderTranslator`/`vkCreateGraphicsPipelines` take roughly 20
   seconds *per occurrence*, and it recurs repeatedly (the guest appears to
   reissue the same corrupt draw in a loop). Step 6 caps total translations
   so this can no longer stall a run for minutes, but the underlying
   fetch-constant/register decode unreliability (step 1's root cause: the
   same draw's "guest_address"/size decode is itself garbage/unstable across
   resubmits) is unfixed — worth chasing directly since it also affects
   draw/texture-fetch reliability elsewhere.
4. **Remaining `SystemConstants` fields** (point sprite sizing, user clip
   planes, alpha test, gamma conversion, EDRAM/poly-offset fields) are still
   zeroed/default — fine for now since step 8 confirmed the fields that
   matter for basic 2D UI positioning are correct, but will need real values
   once more complex draws (3D gameplay, not just intro UI) are reached.

## Step 9 (2026-07-11): `renderdoc-mcp` finally used against a real capture — found and fixed two root causes of "nothing rasterizes"

Picking up item 1 above. The `renderdoc-mcp` server set up at the end of
step 8 connected fine this session, but automating capture creation itself
(`ExecuteAndInject`/`CreateTargetControl` via the self-built `renderdoc.pyd`,
`scripts/capture_frame.py`/`scripts/trigger_capture.py`, both still in the
tree as reusable scaffolding) never got RenderDoc's hooks to attach to this
process — no ImGui capture overlay, no target-control API connection, even
after trying `renderdoccmd.exe capture` directly and enabling "hook child
processes." Root cause not found (the working theory, disproven: an
instance-creation-timing race between our early `OnPreLaunchModule` Vulkan
init and RenderDoc's inject window — ruled out because the user's own
manual launch also needed **Vulkan hooking enabled explicitly** in
qrenderdoc's settings, which isn't on by default; once that was flipped,
capture worked immediately with no timing changes on our side). **Practical
answer: have the user create captures manually via qrenderdoc** (Vulkan
hooking enabled in Settings, launch `nocturnerecomp.exe` with
`--fullscreen=false` so it doesn't take over the screen, F12 once a few
seconds in) rather than fighting automated injection further.

Two captures analyzed (`cap1.rdc`, `cap2.rdc` — not committed, regenerate
via the above if needed):

**Bug 1 — `vertex_index_min`/`vertex_index_max` never populated.**
`native_command_processor.cpp`'s `SystemConstants system_constants{};` is
value-initialized (all zero) and nothing ever wrote these two fields. The
translated vertex shader's fetch prologue does
`UClamp(computed_index, vertex_index_min, vertex_index_max)` before using
the result to address shared memory — with both bounds zero, **every
vertex's index clamped to 0**, so every vertex in every draw fetched from
the same address. Found via `get_post_vs_data`: all 4 `gl_Position` outputs
of `cap1.rdc`'s first draw were bit-identical. Real Vulkan backend reference
(`command_processor.cpp`) reads these straight from the
`VGT_MIN_VTX_INDX`/`VGT_MAX_VTX_INDX` registers, confirmed non-zero in this
game's actual PM4 stream (`0x00FFFFFF`, `0x0000FFFF` observed via the
register-dump logging). Fix: read both registers into
`system_constants.vertex_index_min`/`vertex_index_max` alongside the other
per-draw system constants. Confirmed fixed live (`vidx_max` now logs real
values, and `cap2.rdc`'s post-VS positions are 4 genuinely distinct
vertices forming a real quad) — but not sufficient by itself, see bug 2.

**Bug 2 — alpha-test flags never populated, so every fragment was
`Kill()`ed.** With bug 1 fixed, `cap2.rdc` still showed a fully uniform
clear-colored render target. `pixel_history` at a pixel inside the new
quad's screen-space footprint showed the draw touched it but failed with
`shader_discarded`. Disassembling the pixel shader: it always runs an
alpha-test block gated on `BitFieldUExtract(flags, 16, 3) != 7` — i.e. it
assumes an *unset* alpha-test field means "run the test," and the test's
own comparison-function decode treats `0` as "never passes," not
"always passes" (`kAlways` is encoded as `7`). Since
`native_command_processor.cpp` never set these bits, every fragment on
every draw failed this unintended alpha test and got discarded. Real
backend reference: `command_processor.cpp` sets
`flags |= uint32_t(rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func : xenos::CompareFunction::kAlways) << kSysFlag_AlphaPassIfLess_Shift`,
plus `system_constants_.alpha_test_reference` from `RB_ALPHA_REF`. Fix:
same read, added right after the existing flag-setting block. Confirmed
fixed live: DebugDumpColorTarget's first 3 dumped frames went from 100%
uniform clear-color blue to genuine rasterized content (currently solid
black quads, not yet the right color — see below).

**Net visible result:** the game now renders actual geometry (previously:
nothing, ever, since Phase 3 began) — user-observed as black rectangles
appearing over the blue clear color, flickering, while gameplay/audio
continues normally underneath (confirms the CPU/guest side was never the
problem, only this rasterization gap). This is real forward progress but
not yet correct output.

**Follow-up same session, confirmed with a fresh capture (see "Getting a
RenderDoc capture" below): the black-quads mystery above is already-known,
not new.** `get_draw_call_state` on one of the textured draws
(`event_id=437`, 78 indices) shows its bound texture (`xe_texture0_2d_u`) is
a **1x1 white placeholder** (`read_texture_pixels` confirms `[1,1,1,1]`),
not real intro artwork. That's `GetOrUploadTexture`'s documented, deliberate
fallback (`native_command_processor.cpp`'s `format_supported` check) for any
fetch constant it doesn't yet handle — and this game's intro content is
overwhelmingly `k_DXT4_5`/tiled, i.e. exactly step 8's item 2 below, not a
new bug. So: bugs 1-2 above were genuinely blocking *all* rasterization;
now that they're fixed, the game is correctly falling through to the
already-documented "next" item (DXT4/5 + tiling) rather than hitting
anything new. **Item 2 below is now the actual next step, unblocked.**

**Next (in order):**
1. Remove the temporary diagnostics (per-pixel `TEMPORARY diagnostic`
   vertex overrides, verbose disasm/vfetch/sysconst logging,
   `DebugDumpColorTarget` call) added across steps 8-9 — all clearly marked
   in `native_command_processor.cpp` — once a couple of real textures are
   confirmed rendering, so they don't have to be re-diagnosed from scratch
   if something regresses in the meantime.
2. **`k_DXT4_5` support (block decompression + tiling)** — see step 8's
   item 2 for the details (unchanged, still accurate). This is the actual
   next piece of work: `GetOrUploadTexture`'s `format_supported` check needs
   a DXT5/BC3 path plus real tiled-address math
   (`texture_util::GetTiledOffset2D`/`GetGuestTextureLayout`, already linked,
   not yet called).
3. Items 3-4 from step 8's "Next" list (pathological-shader-stall root
   cause, remaining zeroed `SystemConstants` fields) still apply.

## Getting a RenderDoc capture (recipe that actually works)

Took two sessions to land on a reliable recipe — recorded here so the next
one doesn't have to re-discover it:

- **`renderdoc-mcp`'s own automated injection never worked** (`ExecuteAndInject`/
  `CreateTargetControl` against a separately-self-built `renderdoc.pyd` —
  see step 8's build notes) — no ImGui capture overlay ever appeared, no
  matter what launch options were tried. Root cause: **RenderDoc's Vulkan
  hooking must be explicitly enabled once, in qrenderdoc's own Settings**
  (a persistent machine-wide setting, off by default) — the officially
  *installed* RenderDoc's Vulkan implicit layer registration is what
  actually matters, not anything about our launch code or the self-built
  module. Once the user enabled it manually in qrenderdoc's Settings once,
  capturing worked immediately, both via manual qrenderdoc launch and via
  the scripted flow below, with no further code changes on our side. If a
  future session hits "no overlay / no capture" again, **check this
  setting first** before re-investigating injection timing or Vulkan
  instance creation order (both were dead ends this time).
- **Scripted, unattended capture**: `scripts/capture_frame.py`. Launches
  `nocturnerecomp.exe` via the *officially installed* `renderdoccmd.exe`
  (`capture -d <repo root> -c logs/capture <exe> --game_data_root assets
  --license_mask=1 --fullscreen=false` — important: use the installed one
  at `C:\Users\<user>\scoop\apps\renderdoc\current\renderdoccmd.exe`, not
  anything self-built, since only the installed copy is registered as the
  Vulkan layer), waits ~45s for the game to actually reach the intro draws
  (it takes a while to boot — don't shorten this), then uses the
  self-built `renderdoc.pyd` (`../.tools/renderdoc_runtime`, from step 8)
  purely for `CreateTargetControl`/`TriggerCapture` against the ident
  `renderdoccmd` printed at launch. This mixed approach — installed
  renderdoccmd for the inject/launch, self-built module for target control
  — is what actually works; using the self-built module for the launch
  step too was the reason automated injection failed.
- `scripts/trigger_capture.py` is a smaller standalone utility: connects to
  an *already-running*, already-injected `nocturnerecomp.exe` (via
  `rd.EnumerateRemoteTargets`) and triggers a capture, for when the game is
  already up (e.g. launched manually) and you just need one more frame.
- Always pass `--fullscreen=false` — the default (`fullscreen` cvar,
  `rexglue-sdk/src/ui/window.cpp`) takes over the whole screen, which is
  disruptive when driving this from an agent session.
- Analyze the resulting `.rdc` with the `renderdoc-mcp` server's tools
  (`open_capture`, `find_draws`, `get_draw_call_state`, `get_post_vs_data`,
  `pixel_history`, `disassemble_shader`, `read_texture_pixels`, etc.) — no
  need to touch the raw `renderdoc.pyd` API directly for analysis, only for
  the capture-triggering step above.
- Don't commit `.rdc` files (multi-MB binary, and stale ones actively
  mislead — this doc had two committed-adjacent captures removed mid-session
  for exactly that reason). Regenerate via the recipe above instead.
