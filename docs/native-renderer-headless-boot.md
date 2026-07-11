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
2. ~~`k_DXT4_5` support (block decompression + tiling)~~ — **done, see step
   10 below.**
3. Items 3-4 from step 8's "Next" list (pathological-shader-stall root
   cause, remaining zeroed `SystemConstants` fields) still apply.

## Step 10 (2026-07-11): `k_DXT4_5` (BC3) decode + tiled addressing for all supported texture formats

Implements item 2 above. `GetOrUploadTexture` (`native_command_processor.cpp`)
no longer rejects `tiled`/`packed_mips` fetch constants outright, and adds a
`k_DXT4_5` branch alongside the existing uncompressed formats.

- **No CPU BC1/2/3 decoder existed anywhere in the SDK** (confirmed by
  research: `rexglue-sdk` only decodes DXT on the GPU via compute shaders,
  `src/graphics/shaders/vulkan_spirv/texture_load_dxt5_rgba8_cs.h` et al. —
  no CPU-side equivalent). Added `DecompressDXT5Block` (anonymous namespace,
  `native_command_processor.cpp`), a standard from-spec BC3 block decoder
  (8-byte interpolated-alpha block + 8-byte BC1-style always-4-color-mode
  RGB block) — written from the format spec, not ported from anywhere.
- **Tiled addressing reuses the SDK's existing (already-linked, previously
  uncalled) `texture_util::GetTiledOffset2D`**
  (`rexglue-sdk/include/rex/graphics/pipeline/texture/util.h`). It operates
  in *block* units generically (a block is 1 texel for uncompressed formats,
  4x4 texels for DXT), taking `(x, y)` block coordinates, `pitch` in blocks,
  and `bytes_per_block_log2`, returning a byte offset — so the same function
  now backs both the new DXT path (`bytes_per_block_log2=4`, i.e. 16
  bytes/4x4 block) and the existing uncompressed paths (`bytes_per_texel_log2`
  of 1 or 2), replacing the old linear-only `(y * pitch + x) * bytes_per_texel`
  math with a `fetch.tiled ? GetTiledOffset2D(...) : linear` branch for every
  format, not just DXT.
- `packed_mips` is now allowed (previously an unconditional reject) because
  with `mip_min_level == mip_max_level == 0` (still required) there's no mip
  level 1+ for the packed tail to actually affect — packed_mips only changes
  where mip levels *above* the base live, per `util.h`'s addressing doc
  comment; the base level's own pitch-based addressing is unaffected. Real
  intro `k_DXT4_5` fetch constants observed with `tiled=1 packed_mips=1`
  (from step 7's log) are exactly the case this unblocks.
- DXT's own "pitch" reuses the same `fetch.pitch * 32` field as uncompressed
  formats, just interpreted in blocks instead of texels for a compressed
  format (32-block alignment for both, per D3D9's alignment behavior —
  documented in `util.h`'s big comment block on base-vs-mip addressing).
- Byteswapping: DXT blocks are treated as 8 packed 16-bit fields (matching
  the existing `Endian::k8in16` handling other 16-bit-field formats already
  use) — swapped in place, 2 bytes at a time, not reordered, before handing
  the block to the decoder.

**Verified:** headless run with no crash/hang, `XMP: started BGM playlist`
still reached. `format=20` (`k_DXT4_5`) fetch constants — previously 100% of
"skipping texture upload for unsupported fetch constant" log lines during
this run were `format=26` (`k_16_16_16_16`, still unsupported, low priority)
or `format=20` — **no longer appear in the skip log at all**, confirming
every `k_DXT4_5` fetch constant hit during boot is now taken through the new
decode path instead of falling back to the 1x1 default texture.

**Byteswap follow-up, same session:** the initial DXT implementation
byteswapped every block unconditionally as if `Endian::k8in16` (2-byte-pair
swap), regardless of the fetch constant's actual `endianness` field.
Generalized to a proper `GpuSwapBytes` helper matching `xenos::GpuSwap`'s
real per-`Endian`-value semantics exactly (`k8in16` = swap each 2-byte pair
independently; `k8in32` = full 4-byte dword reverse; `k16in32` = swap the
two 16-bit halves without touching in-half byte order; `kNone` = copy) —
confirmed against `rexglue-sdk`'s `GpuSwap`/`CopySwapBlock` (research, not
guessed). Applied uniformly to the whole 16-byte DXT block, matching how the
real Vulkan backend's `texture_load_dxt5_rgba8_cs` compute shader treats it
(no BC3-sub-field-aware swapping).

**Verification via a real RenderDoc capture (`scripts/capture_frame.py`),
same session:** a temporary per-texture diagnostic (average/max RGBA of the
CPU-decoded texel buffer, logged once per distinct texture, since removed)
confirmed the DXT decode itself produces real, non-degenerate color data —
e.g. a 512x512 `k_DXT4_5` texture decoded to `avg_rgba=(125.7,100.3,75.3,162.8)
max_rgb=(255,255,255)`, and RenderDoc's `list_actions`/`get_draw_call_state`
on the captured frame confirmed this exact texture (the Digital Eclipse
splash logo, matching `dumps/textures/0fdf4d1cb6e8369d_512x512_k_DXT4_5.png`
pixel-for-pixel when the dump is viewed directly) is genuinely bound at the
pixel shader's texture slot for a real draw. **So the DXT5 decode/tiling
work in this step is confirmed correct and is not the cause of the
still-visible "black squares" symptom** the user reported after this work
landed.

**However, that symptom is real and still unexplained** — investigated
directly in the same session, not yet resolved:
- The same diagnostic caught a **separate, pre-existing** issue: a *linear*
  (`tiled=0`) `k_1_5_5_5` fetch constant (`2048x1024`, `base_address=0x1ED80000`,
  `pitch=64`) decodes to **exactly all-zero RGBA** across every texel. Its
  addressing math is byte-for-byte the same linear-path formula this file
  already used before this session (unchanged by the DXT/tiling work), so
  this isn't a regression from step 10 — it was already reachable before
  (nothing in the `tiled`/`packed_mips` relaxation touches the linear,
  non-DXT path's addressing), just not diagnosed until now. Two live
  hypotheses, neither confirmed: (a) this fetch constant's `base_address`
  points at a render-target/resolve destination this renderer doesn't
  implement writing to yet (the real 360 GPU would populate it via a
  resolve/copy PM4 packet this native command processor doesn't consume),
  so the memory is genuinely never written; or (b) `base_address`/`pitch`
  decode is wrong for this specific fetch constant (same class of
  decode-reliability issue flagged since Phase 3's "Next" item 3).
- **A second, more surprising finding from the same capture**: despite that
  texture's texel data being all-zero (fully transparent black), the actual
  rendered pixels in RenderDoc's saved output for the draw using it are
  **solid opaque white**, not transparent/black — i.e. the GPU's sampled
  result doesn't match the CPU-side decoded content at all. `get_shader_bindings`
  on that draw's pixel stage shows the `xe_sampler0_fff`/`xe_sampler1_fff`
  separate-sampler bindings reporting empty/`ResourceId::0`, even though the
  matching `SAMPLED_IMAGE` binding correctly resolves to the real uploaded
  image resource. Not yet determined whether this is a genuine descriptor
  binding bug (the code writes both `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` and
  `VK_DESCRIPTOR_TYPE_SAMPLER` in `GetOrUploadTexture`/`InitializePipelineResources`
  and both look structurally correct on inspection) or a RenderDoc reflection
  quirk with non-combined image/sampler pairs — needs a `pixel_history` +
  `disassemble_shader` pass on this exact draw to settle, not done yet this
  session.
- **Net:** the "black squares" the user sees are not caused by the DXT5 work
  in this step, but by one or both of the above — a pre-existing, deeper
  rendering-pipeline issue (possibly missing resolve-target support, possibly
  a real sampler-binding bug, possibly both) that predates this session (the
  same symptom was already described in step 9's "Net visible result").

## Step 11 (2026-07-11, same day): found and fixed a real "no blending at all" bug; the very first black quad is a separate, still-open issue

Followed up on step 10's `pixel_history` lead immediately. At the render
target pixel under one of the reported black squares, `pixel_history` showed
`event_id=112` (the frame's first draw, no texture bound at all —
`texture_count=0`) writing `(0,0,0,0)` directly over the blue clear color,
with later draws at the same pixel either not touching it or getting
`shader_discarded`.

**Real bug #1, fixed: blending was unconditionally disabled.**
`GetOrCreatePipeline`'s `VkPipelineColorBlendAttachmentState` was a bare
zero-initialized struct (`blendEnable` defaults false) — nothing ever read
the guest's `RB_BLENDCONTROL0`/`RB_COLOR_MASK` registers, so *every* draw
wrote its raw shader output opaquely regardless of the guest's actual blend
intent. Fixed by translating those registers for real: `ToVkBlendFactor`/
`ToVkBlendOp` (new, `native_command_processor.cpp`) map `xenos::BlendFactor`/
`xenos::BlendOp`'s guest encoding to `VkBlendFactor`/`VkBlendOp` (the
numeric values don't line up 1:1 with Vulkan's own enum — a real table,
matching `rexglue-sdk`'s `VulkanPipelineCache::WritePipelineRenderTargetDescription`/
`kBlendFactorMap`, reimplemented rather than linked for the same reason
`SystemConstants` was). `GetOrCreatePipeline` gained `blend_control`/
`color_write_mask` parameters (raw `RB_BLENDCONTROL0`/RT0 write-mask bits),
folded into its cache key too (a shader pair can legitimately be reused
across draws with different blend state, e.g. a fade animating blend
factors frame to frame over static geometry) — read fresh per-draw in
`TryDraw` and passed through.

**Confirmed via a real capture, not just code review:** draw#6-10 in the
intro (the logo's glow/particle overlay, per the new `blend_control`/
`color_mask` fields added to the existing per-draw `sysconst draw#N` debug
log) carry `blend_control=07060706` — decodes to `color_srcblend=kSrcAlpha,
color_destblend=kOneMinusSrcAlpha` (standard "SrcAlpha, InvSrcAlpha" alpha
blending) — genuinely non-identity blend state that was previously forced
to opaque overwrite. This is a real, confirmed-live fix, independent of the
"black squares" symptom below.

**Real bug #2, still open: draw#1 itself outputs literal `(0,0,0,0)`, and
it's not a blending issue.** Decoded `draw#1`'s own `blend_control=00010001`
— that's `color_srcblend=kOne, color_destblend=kZero,
alpha_srcblend=kOne, alpha_destblend=kZero`, i.e. **identity** blend (the
same "no-op, treat as opaque" case this renderer already special-cases to
`blendEnable=false`). So draw#1's black pixel isn't a blending bug at all —
the pixel shader itself is producing `(0,0,0,0)` as its literal fragment
output, correctly written opaquely. Since this draw has no texture bound
(`texture_count=0` from `get_draw_call_state`), its color must come from a
vertex-color attribute or a float/bool constant register — and per the
now-familiar pattern from Phase 3's "Next" item 3, is a strong candidate for
the same fetch-constant/register decode-reliability issue already flagged
there (a specific draw whose decoded address/constants are corrupt across
resubmits). **Not yet root-caused.** Also can't yet rule out that this
specific draw is a legitimate black fade-in element (SOTN's intro does fade
from black) rather than a bug — needs either a longer observation window
(does a persistent black square from a *fully booted, past-intro* frame
show the same signature?) or vertex-data/constant inspection via
`get_post_vs_data`/`debug_shader_at_pixel` on event 112 specifically, which
wasn't done this session.

**Next:**
1. `get_post_vs_data`/`debug_shader_at_pixel` on event 112 (or an equivalent
   black-quad draw captured later in gameplay, not just the intro) to
   determine whether its vertex color / referenced constants are genuinely
   zero (decode bug) or intentionally black (legitimate content).
2. Re-check whether "black squares are the only thing visible... throughout
   gameplay" (the user's report) still holds now that real blending works —
   capture a frame well past the intro, not just the first ~1s of boot this
   session's captures used.
3. Items 3-4 from step 8's "Next" list (pathological-shader-stall root
   cause, remaining zeroed `SystemConstants` fields) still apply.

## Step 12 (2026-07-11, same day): the real bug — vertex-to-pixel interpolated data was never wired up at all

Followed item 1 above immediately with `disassemble_shader` instead of
`debug_shader_at_pixel` (the latter reported `DebugPixel returned no trace
data` -- shader debugging isn't supported on this API/GPU combo here).
Disassembling event 112's pixel shader's SPIR-V was decisive: its interface
block (`EntryPoint(Fragment, main, "main", {..})`) had **no Input variable
at all** for interpolated data -- just the uniform buffers, shared memory,
and `gl_FragCoord`. The shader body read `xe_var_registers[0]` (meant to
hold the vertex shader's interpolated color output) straight from a
zero-initialized local, with nothing ever loading it from an actual pixel
shader input. `oC0 = max(r0, r0) = r0 = 0` -- guaranteed black, unconditionally,
for *any* shader depending on interpolated vertex-stage output.

**Root cause:** `GetOrTranslateShader` used `SpirvShaderTranslator::
GetDefaultVertexShaderModification`/`GetDefaultPixelShaderModification`
as-is. Checked the translator source (`spirv_translator.cpp`): both
"default" functions leave `Modification::vertex.interpolator_mask`/
`pixel.interpolator_mask` at `0` -- they only set
`dynamic_addressable_register_count` (+ `host_vertex_shader_type` for
vertex). Nothing in this renderer ever set `interpolator_mask` to anything
else, so it was always `0`: zero interpolators declared as needed by the
pixel shader, zero declared as written by the vertex shader, for every
single shader pair translated since Phase 3 began. This is a far larger bug
than anything fixed in steps 10-11 -- it silently zeroed vertex color, UV
coordinates, and any other per-vertex data for *every* draw in the entire
game that relies on it (which is most non-trivial shaders), not just one
intro quad.

**The real formula** (confirmed in `rexglue-sdk/src/graphics/vulkan/command_processor.cpp`,
the real Vulkan backend's per-draw setup):
```cpp
uint32_t interpolator_mask =
    vertex_shader->writes_interpolators() &
    pixel_shader->GetInterpolatorInputMask(sq_program_cntl, sq_context_misc, ps_param_gen_pos);
```
`Shader::writes_interpolators()`/`GetInterpolatorInputMask()` are both real,
already-linked `Shader` methods (`rex/graphics/pipeline/shader/shader.h`) --
no reimplementation needed, just actually calling them and threading the
result through.

**Restructured shader caching to make this possible** (`native_command_processor.{h,cpp}`):
- Added `GetOrAnalyzeShader` (new) -- runs `AnalyzeUcode` only, cached by
  ucode hash + type in a new `analyzed_shaders_` map, independent of any
  translation modification. Needed because `interpolator_mask` depends on
  *both* shaders in a pair (their `writes_interpolators()`/
  `GetInterpolatorInputMask()`), so both must be analyzed before either can
  be translated -- the old `GetOrTranslateShader` did analysis and
  translation together in one cache keyed only by ucode, which couldn't
  support this.
- `GetOrTranslateShader` now takes `interpolator_mask` as a required
  parameter (not defaulted), folds it into the translation cache key
  alongside `host_vertex_shader_type` (a shader can legitimately need
  different masks when paired with different partner shaders), and sets
  `modification.vertex.interpolator_mask`/`pixel.interpolator_mask`
  explicitly instead of leaving the "default" (zero) value.
- `TranslatedShader::shader` changed from an owning `unique_ptr<Shader>` to
  a non-owning `Shader*` (now owned by `analyzed_shaders_`) -- all existing
  `->shader->method()` call sites (`vertex_bindings()`, `texture_bindings()`,
  `constant_register_map()`) needed no changes, since they only ever
  dereferenced through the pointer.
- `TryDraw` now analyzes both shaders via `GetOrAnalyzeShader` first,
  computes `interpolator_mask` from their real usage, then passes it to both
  `GetOrTranslateShader` calls.
- `DestroyPipelineResources` clears the new `analyzed_shaders_` map alongside
  the existing `shader_cache_`/`pipeline_cache_`.

**Verified via a fresh capture:** event 112's pixel shader SPIR-V now
declares `Input float4* xe_in_interpolator_0 : [[Location(0)]];` and its
body does `xe_var_registers[0] = *xe_in_interpolator_0` before using it --
the interpolated data path is real now, confirmed by inspecting the actual
compiled SPIR-V, not just by reasoning about the source change.

**This specific draw's pixel is still black, but that's now understood to be
correct, not a bug.** Traced its actual vertex data (from the existing
`vfetch draw#N` debug log): 3 vertices at 7 floats/vertex (`xyz` position +
`rgba` color, matching the shader's `vfetch_full`+`vfetch_mini Offset=3`
layout), vertex 0's color = `(0.0, 0.0, 0.0, 1.0)` -- **the guest explicitly
wrote opaque black** into this quad's vertex buffer. Its `blend_control`
(step 11) is identity/opaque, so a genuinely black vertex color renders as a
solid black rectangle correctly. Likely a letterbox/border or fade-related
element, not a decode error -- draw#1's black square specifically was never
the bug; the *absence of any interpolated data for every other draw* was.

**Net:** this is expected to be the highest-impact fix of the session by a
wide margin -- unlike the DXT/blend fixes (each scoped to specific draws/
formats), a zeroed `interpolator_mask` affected every shader pair translated
since native rendering began. Not yet re-verified against a full interactive
run (only captured frames were inspected this session) -- **the user should
rebuild and run interactively next** to see whether overall visual
correctness improved beyond just this one draw's (already-correct) black
square.

## Step 13 (2026-07-11, same day): the actual root cause of "everything is black" -- `color_exp_bias` left at 0.0 instead of 1.0

The user rebuilt and ran interactively after step 12 and reported **no
change at all** -- still all-black. That should have been surprising if
step 12's fix were the whole story (it fixed a real, confirmed bug), which
motivated tracing the pixel shader's SPIR-V further rather than assuming the
fix "just needs more testing."

**Found it:** every pixel shader `SpirvShaderTranslator` generates ends with
an unconditional `output_color = output_color * system_constants.color_exp_bias[rt]`
after the alpha-test block (visible directly in the SPIR-V dumped in step
12's investigation, lines building `_143 = _139 * _140` from
`xe_uniform_system_constants.color_exp_bias.x`). `native_command_processor.cpp`'s
`SystemConstants system_constants{};` is value-initialized -- every field,
including `color_exp_bias[4]`, starts at `0.0f`. **Every single draw's final
output color was being multiplied by zero, unconditionally, regardless of
texture sampling, vertex color, or blend state** -- this fully explains why
neither the DXT fix, the blend fix, nor the interpolator_mask fix changed
anything visible: all three were real, correct fixes operating upstream of
a final "multiply everything by zero" step nothing had touched yet.

**Real formula** (confirmed in both `rexglue-sdk`'s Vulkan and D3D12
backends, `command_processor.cpp`): `color_exp_bias[rt]` isn't a plain
scale factor computed by simple arithmetic -- it's built via direct float
bit manipulation, taking `RB_COLOR_INFO`/`RB_COLOR[1-3]_INFO`'s 6-bit signed
`color_exp_bias` field and adding it directly into `1.0f`'s IEEE-754
exponent bits (`0x3F800000 + (color_exp_bias << 23)`), equivalent to
`exp2f(color_exp_bias)`. When the guest sets no bias (the common case,
`color_exp_bias == 0`), this reduces to exactly `1.0f` -- a no-op multiply,
which is the correct default this renderer was missing entirely.

**Fix** (`native_command_processor.cpp`, `TryDraw`'s `SystemConstants`
setup): read `RB_COLOR_INFO`'s `color_exp_bias` field and compute
`std::exp2(float(color_exp_bias))` for RT0 (the only render target this
renderer has); RTs 1-3 hardcoded to `1.0f` since nothing writes to them.

**Verified with a fresh capture, not just reasoning:** `logs/rt_204_expbias.png`
(the offscreen color target before the swapchain copy) now shows **real
rendered content** -- the KONAMI splash screen, rendered correctly: a red
banner with white "KONAMI" text and its logo shape, over the game's actual
background, not a flat clear color or a black square. This is the first
frame since Phase 3 began where recognizable, correct game content has
rendered on screen.

**Net:** this was the actual root cause of the "black squares"/"all black"
symptom reported throughout this session -- not the DXT decode, not
blending, not even the interpolator_mask bug (real as it was). All of
step 10-12's fixes were still worth making (each is independently a real,
confirmed correctness bug), but none of them could have produced visible
non-black output while this one remained. **Next: have the user rebuild and
run interactively again** -- this is expected to be the fix that finally
produces a visible difference outside of RenderDoc captures.

**User confirmed interactively: it's rendering.** The KONAMI splash and the
title screen (moon, spires, gargoyle statues, "Castlevania: Symphony of the
Night" logo) all appeared on screen for the first time. Reported next issue:
"past that konami logo, everything looks misplaced and full of glitches."

## Step 14 (2026-07-11, same day): DXT tiled-texture pitch was in the wrong units -- texels, not blocks

Captured a later frame (custom script waiting 90s instead of `capture_frame.py`'s
45s, to get past the intro into the title screen) and found the title screen
mostly correct -- KONAMI box, Castlevania logo, moon, spires, gargoyle
statues all in roughly right positions -- but with a horizontal band of
colorful static/noise across the middle of the screen, plus a small
misplaced duplicate logo fragment. `pixel_history` traced the noise to a
single draw (`event_id=413`) sampling one texture (`852x480`,
`ResourceId::29100`); saving that texture directly (not just the composited
frame) showed the corruption was **in the CPU-decoded texture itself**, not
a compositing bug -- moon/spires/gargoyle content at the top and bottom of
the 480px-tall image decoded correctly, but a middle band was pure noise.
Confirmed via `dumps/textures/325984db674889d6_852x480_k_DXT4_5.png` (ground
truth): this is the intro's tombstone-frame background art, one contiguous
image, not several composited textures -- so a real per-texture decode bug,
not a draw-ordering issue.

**Root cause:** added temporary debug logging (`DEBUG tex upload`, since
removed) of the fetch constant's raw fields for this texture:
`tiled=1 packed_mips=1 pitch=28`. `pitch=28` decodes via the existing
`fetch.pitch * 32u` formula to `896` -- but step 10's DXT code then used
that value directly as the **block**-granularity pitch fed to
`GetTiledOffset2D`. `896` isn't a plausible block pitch for an 852px-wide
(213-block) texture; it's `4x` the actually-correct 32-block-aligned block
pitch (`224`). The fetch constant's `pitch` field is **texel**-granularity
uniformly across formats (`texels >> 5`, per `xenos.h`'s field comment,
which step 10 had already read but misapplied) -- for a block-compressed
format, that texel-pitch must additionally be divided by the block width
(4) to get the block-pitch tiling math actually needs. Missing that `/4`
was step 10's real bug: `pitch_blocks = std::max(fetch.pitch * 32u, block_w)`
should have been `std::max((fetch.pitch * 32u) / 4, block_w)`.

**Why this produced a periodic noise *band* instead of uniform garbage or a
diagonal shear** (worth recording, since it wasn't obvious going in): the
wrong pitch (896) was an exact integer multiple (4x) of the correct one
(224). `GetTiledOffset2D`'s tile-periodic addressing (blocks are grouped
into 32x32 tiles with address patterns that repeat with a period related to
bytes-per-block and pitch) partially aliases back to correct-looking output
for some row ranges under an exact-multiple pitch error and not others --
unlike a linear-addressing pitch bug, which would produce a monotonically
increasing diagonal shear instead. This is a useful diagnostic signature for
next time: **periodic/banded corruption in a tiled texture, with some
regions decoding perfectly, points at a pitch computed off by an integer
factor, not a fundamentally wrong tiling algorithm.**

**Fix:** one-line change in `GetOrUploadTexture`'s DXT branch
(`native_command_processor.cpp`) -- divide the texel-granularity pitch by
the block width before using it as block-pitch.

**Verified with a fresh capture:** re-ran the same 90s-wait capture recipe
after the fix; `logs/rt_fixed.png` (see this doc's companion screenshots)
shows the full title screen rendering correctly -- moon, spires, gargoyle
statues, tombstone frame, and the Castlevania logo, with **no noise band**.
Not yet confirmed whether the "misplaced duplicate logo fragment" from the
step 13 screenshot is a separate, still-open bug or was actually caused by
this same pitch error (a different draw sampling the same or a
similarly-tiled texture) -- worth re-checking now that this fix is in.
Also not yet checked: the "Press <A>" prompt text below the tombstone slab
is conspicuously absent even in the fixed capture (the slab area is blank
stone) -- likely a separate, unrelated text/UI-rendering gap, next to
investigate.

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

## Step 15 (2026-07-11, same day): multi-texture-per-stage support, and a likely memexport gap for the CPU/GPU-rendered gameplay-preview texture

Followed up on a "no gameplay image visible" report from a user-captured
`game.rdc`/`game2.rdc` (character-select or New-Game screen, gameplay music
audible during capture, confirming it's not the intro).

**Real bug, fixed: only one texture per shader stage could ever be bound.**
`get_shader_bindings` on the screen's textured draws showed `xe_texture1_2d_u/s`
completely unbound (`ResourceId::0`), even though the pixel shader's SPIR-V
interface declared it and the image binding for `xe_texture0` resolved
correctly. Root cause: `texture_layout_` (`native_command_processor.cpp`) was
a single hardcoded 2-binding (1 image + 1 sampler) `VkDescriptorSetLayout`
reused for every shader, and `TryDraw`'s texture resolution only ever bound
`shader->texture_bindings()[0]` — a real, pre-existing limitation the header
comments already called out ("this renderer only supports one texture per
stage for now"), but not yet a blocker until a shader needing two
simultaneous texture units (a base image + a glow/overlay layer, judging by
the shader's structure) was actually hit.

**Why a single fixed layout can't work at all, not just "doesn't cover N>1":**
investigated `SpirvShaderTranslator`'s actual SPIR-V generation
(`spirv_translator.cpp`'s `EmitMain`) — sampler bindings are decorated at
`texture_binding_count + i`, i.e. **the sampler binding numbers depend on
how many images that specific shader needs**, not a fixed offset. A shader
needing 1 texture has its sampler at binding 1; a shader needing 4 has its
first sampler at binding 4. So no single fixed-size layout (not even a
generously-padded one) can match every shader's actual expected binding
layout — the descriptor set layout (and therefore the pipeline layout that
embeds it) has to be built per distinct `(texture_count, sampler_count)`
shape actually seen.

**Fix, a real (not small) refactor:**
- `GetOrCreateTextureSetLayout(texture_count, sampler_count)` (new) builds
  and caches a `VkDescriptorSetLayout` per distinct shape: images at
  `[0, texture_count)`, samplers at `[texture_count, +sampler_count)`,
  matching the translator's scheme exactly.
- `GetOrCreatePipelineLayout(vs_tex, vs_smp, ps_tex, ps_smp)` (new) builds
  and caches a `VkPipelineLayout` per distinct 4-tuple of stage shapes.
- `GetOrCreatePipeline` now returns a `PipelineEntry{pipeline, layout}` (was
  bare `VkPipeline`) — callers need the exact layout a pipeline was built
  against to bind descriptor sets correctly, and that layout is no longer a
  single global.
- **Had to switch `GetOrAnalyzeShader` from constructing a base `Shader` to
  a concrete `rex::graphics::SpirvShader`.** Found the hard way (a compile
  error) that the real, dedup'd, binding-index-ordered texture/sampler lists
  SpirvShaderTranslator's SPIR-V output actually expects
  (`SpirvShader::GetTextureBindingsAfterTranslation()`/
  `GetSamplerBindingsAfterTranslation()`) are populated by
  `SpirvShaderTranslator::PostTranslation()` via a `dynamic_cast<SpirvShader*>`
  of the shader just translated — which fails (returns null) unless the
  concrete object actually is a `SpirvShader`, not just a base `Shader`. Base
  `Shader::texture_bindings()` (used previously, and still used unrelatedly
  for `vertex_bindings()`) is a *different*, non-deduped, per-fetch-
  instruction list from ucode analysis — not safe to use for sizing
  descriptor layouts, since a shader that samples the same texture at
  multiple call sites (e.g. a multi-tap blur) would overcount.
- `resolve_texture_set` (in `TryDraw`) now builds one descriptor set per
  stage sized to that shader's actual counts, resolving each
  `GetTextureBindingsAfterTranslation()[i].fetch_constant` via
  `GetOrUploadTexture` (falling back to the 1x1 default view per-slot, not
  per-whole-set, when a specific texture isn't supported/available) and
  writing every sampler slot to `default_sampler_` (unchanged simplification
  — per-binding filter/wrap state still isn't decoded).
- Descriptor sets for textures are now allocated per-draw from
  `transient_descriptor_pool_` (like the constants sets already were)
  instead of being persistent per-uploaded-texture — a combined multi-
  texture set's exact shape depends on the specific shader using it, so it
  can no longer be a property of the texture alone. `UploadedTexture` lost
  its `descriptor_set` field accordingly; `GetOrUploadTexture` now only
  manages the image/view/memory.
- Added a `kMaxTexturesPerStage = 8` safety cap (same rationale as the
  existing shader-cache/texture-cache caps): a garbage-decoded shader could
  otherwise analyze to an implausible texture count and grow descriptor
  pool/layout state unboundedly.

**Verified two ways:** headless boot logs now show pipelines created with
real multi-texture shapes (`created a graphics pipeline (topology=3
vs_tex=(0,0) ps_tex=(4,2))`, `ps_tex=(2,1)`, etc. — previously every pipeline
was implicitly `(0 or 1, 0 or 1)`), no crash, `XMP: started BGM playlist`
still reached, no `transient descriptor pool exhausted`. Then, against a
fresh user-captured `game2.rdc`: `get_draw_call_state` on the same draw that
previously showed `xe_texture1_2d_u/s` as `ResourceId::0` now shows a real
texture bound in all 4 texture/sampler slots; the composited render target
went from showing only the cloudy background (previous report) to showing
real character portraits (Alucard, Richter) and a graveyard silhouette
correctly layered over it.

**Still open: no gameplay-preview content, on a `game.rdc` confirmed to be a
real gameplay-adjacent screen (audible BGM during capture).** All 6 draws in
that frame were individually checked via `get_draw_call_state` — 3 sample
real, correctly-decoded textures (`852x480`, `852x128`, `512x256`), 2 sample
no texture at all (flat-color elements), and the last has every one of its
48 fetch constants read back as literally zero. `list_textures` on the whole
capture shows exactly the textures accounted for by those draws — no
additional, unbound-but-present large texture exists anywhere in the
capture. The user confirmed **`512x256` is the actual correct size of the
gameplay-preview texture** — but `save_texture` on it shows flat white/blank
content, not gameplay. So the texture object exists, is correctly sized, is
bound to a real draw (`event_id=132`) — but its *content* is blank, not
whatever real gameplay frame the guest is supposed to have written there
before this draw ran.

**Working hypothesis, not yet confirmed:** the user separately confirmed
(from an earlier real-xenos-backend RenderDoc session) that this content is
"rendered entirely on the CPU/GPU and output as a texture" — i.e. generated
by some path other than a normal fixed-function draw-to-render-target. The
Xenos register file exposes `RegisterFile::GetMemExportStream` (`rexglue-sdk`'s
`register_file.h`) — **memexport**, a real Xenos GPU feature where a vertex/
compute-style shader writes arbitrary data directly to guest memory, used by
some games for software/GPU-hybrid rendering instead of the normal
render-target pipeline. `NativeCommandProcessor` **does not implement
memexport in any form** — `OnPacket`/`OnDraw` only handle ordinary indexed/
non-indexed draws through the fixed vertex-shader → rasterizer → pixel-
shader path; nothing here ever calls `GetMemExportStream` or processes a
memexport-shaped draw/dispatch. If the gameplay-preview texture is populated
via memexport, that write simply never happens in this renderer, leaving
the `512x256` texture at whatever the shared-memory-backed buffer's default
content is (blank/white) when the later draw samples it. Consistent with,
but not proven by: a 4-vertex draw elsewhere in similar frames having every
fetch constant read back as zero, as if the guest gave up configuring a
*different* texture fetch once its expected memexport output never
completed.

**Not yet done, and the natural next step:** confirm the memexport
hypothesis directly (e.g. search the decoded PM4 stream for memexport-shaped
vertex-shader draws — `SQ_PROGRAM_CNTL`/`VGT_DRAW_INITIATOR` with `num_indices`
and a shader whose ucode writes via `eA`/export addressing rather than the
normal position/interpolator exports — before assuming it needs
implementing), then implement `RegisterFile::GetMemExportStream`-driven
memory writes if confirmed. This would be substantially larger in scope than
anything else done this session (steps 10-15) — a new GPU-write mechanism,
not another texture-format/binding-count gap.

## Step 16 (2026-07-11, later session): memexport implemented; then found and fixed why it still looked stuck on frame 1

The memexport hypothesis from step 15 was confirmed and implemented
(`native_command_processor.{h,cpp}`, `CollectMemExportRanges`): a shader
with `memexport_eM_written()` set has its `memexport_stream_constants()`
scanned (narrow reimplementation of `draw_util::AddMemExportRanges`, same
"can't link draw.cpp directly" reasoning as `GetHostViewportInfo`), and any
range it exports to is queued in `pending_memexport_ranges_` during
`TryDraw`. `EnsureFrameBegun` — specifically because it already does a
synchronous fence wait for the *previous* frame's GPU work before reusing
any per-frame resources — swaps that into
`memexport_ranges_ready_for_readback_` and, now that the writing GPU work is
guaranteed complete, `vkInvalidateMappedMemoryRanges` + `memcpy`s each range
from the shared-memory SSBO mirror back into real guest physical memory via
`Memory::TranslatePhysical`.

**This alone did not fix the user-visible symptom.** The user supplied two
fresh RenderDoc captures (`brk1.rdc`, `brk2.rdc`, committed at the repo
root, taken minutes apart while actually playing) specifically to diagnose
"the gameplay-preview texture renders once and then never updates, instead
of refreshing every frame like a live camera feed should." Opened both via
`renderdoc-mcp`: the `512x256` `R8G8B8A8_UNORM` gameplay-preview texture
(`ResourceId::34625` in both captures, bound at `event_id=132` in both) now
has real, non-blank content — confirming memexport writes are landing in
guest memory correctly — but `read_texture_pixels` on the same 8x8 region
in both captures returned **bit-for-bit identical values**, despite the
captures being taken at different points during live gameplay. That's
direct, ground-truth confirmation of "stuck on the first frame," not
inference from logs.

**Root cause: `GetOrUploadTexture`'s cache key never accounted for content
changes at a fixed address.** This was already flagged, unfixed, as a known
limitation in the header comment on `texture_cache_`
(`native_command_processor.h`) since milestone 3b step 7: the cache key is a
hash of the fetch constant's raw dwords only (address+format+dimensions+
tiling), so once the gameplay-preview texture's fetch constant is set up
(pointing at a fixed guest address that memexport repopulates every frame),
every subsequent `GetOrUploadTexture` call for that fetch constant hits the
cache and returns the very first upload — the fetch constant itself never
changes, only the guest memory it points at, and nothing was invalidating
the cache when that happened. This is a completely different bug from
step 15's "memexport doesn't run at all" — memexport was writing fresh
data into guest memory every frame the whole time, but the GPU-side texture
sampling it never re-read that memory after the first upload.

**Fix** (`native_command_processor.{h,cpp}`): `UploadedTexture` gained
`base_address_bytes`/`size_bytes` (the guest physical byte span the CPU
decode actually read from, computed per-format — block-pitch-based for
`k_DXT4_5`, texel-pitch-based otherwise — inside `GetOrUploadTexture`).
`EnsureFrameBegun`'s memexport-readback loop now also scans `texture_cache_`
for any entry whose range overlaps the range it just copied into guest
memory and destroys+erases it (safe to do here specifically because this is
right after the fence wait that guarantees the GPU is done with last
frame's draws, same reasoning `FreeTransientBuffers()` right below it
already relies on) — so the next draw that samples that fetch constant
takes the cache miss and re-decodes current guest memory instead of serving
the stale upload. Narrow and targeted: only textures whose source range
was actually just memexport-written get invalidated, not the whole cache
every frame (the intro's static DXT textures etc. are unaffected).

**Not yet re-verified with a fresh capture after this fix** (needs a
rebuild + a new `brk*.rdc` pair taken the same way, at least a few seconds
apart during live gameplay, to confirm the two captures now differ where
they didn't before) — the next session should do that first, since this
fix is code-reviewed and builds clean but wasn't checked against a live
capture the way steps 8-15's fixes were.

**Not actually fixed -- the user reported "no progress at all" against a
fresh `brk3.rdc`.** See step 17 immediately below: the memexport hypothesis
itself (steps 15-16) turned out to be wrong. The cache-invalidation fix in
this step is real and harmless, but it was invalidating a code path
(`pending_memexport_ranges_`) that never actually populates for this game.

## Step 17 (2026-07-11, later session): the memexport hypothesis was wrong -- it's an EDRAM resolve-to-texture, not memexport

Diagnosed by adding real logging instead of trusting inference from RenderDoc
pixel reads (which had misleadingly shown non-blank texture content and been
read as "memexport must be populating this"). Added a log line inside
`CollectMemExportRanges` that fires whenever a shader actually has
`memexport_eM_written()` set. Ran a full boot through the intro **and** a
real ~80-second interactive session that the user played from boot through
character-select into actual gameplay (`logs/nocturnerecomp_002.log`) --
**zero shaders, across the entire session, ever had `eM_written` set.** The
memexport code added in steps 15-16 is real, correctly-implemented, and
completely dead for this game's content -- the cache-invalidation fix in
step 16 was therefore fixing a bug in a path that never executes, which is
exactly why the user saw "no progress at all."

**What's actually happening, found directly in the same log:** repeated
writes to `RB_COPY_CONTROL` / `RB_COPY_DEST_BASE` / `RB_COPY_DEST_PITCH` /
`RB_COPY_DEST_INFO`, with `RB_COPY_DEST_BASE` consistently `0x1F6F8000`.
This is the standard Xenos **EDRAM resolve-to-texture** mechanism: setting
`RB_MODECONTROL.edram_mode` to `kCopy` turns what would otherwise be a
normal draw call into a "copy the current render target (or a rectangle of
it) into a guest-memory texture" operation instead of rasterizing --
confirmed against `rexglue-sdk/src/graphics/vulkan/command_processor.cpp`'s
`IssueDraw`, which checks exactly this register before doing anything else
and dispatches to `IssueCopy()` when it's set, an entirely separate code
path from the normal draw pipeline. `NativeCommandProcessor` had no
handling for this at all -- `TryDraw` always went straight into normal
draw/pipeline setup, so the resolve trigger silently did nothing and the
destination memory was never written. This is unrelated to memexport, a
different Xenos GPU feature entirely.

**Fix implemented (`native_command_processor.{h,cpp}`), `TryResolveCopy`:**
- `TryDraw` now checks `RB_MODECONTROL.edram_mode` first, before any
  shader/pipeline work; on `kCopy` it calls `TryResolveCopy()` and returns.
- The resolve rectangle isn't in a dedicated register -- per real hardware/
  D3D9 (and confirmed against `rexglue-sdk`'s `GetResolveInfo`, `draw.cpp`),
  the "draw" that triggers a resolve has its 3 covered-rectangle corners
  written into vertex fetch constant 0 by the CPU. `GetResolveInfo` itself
  isn't callable directly (same "pulls in the plugin-only `TextureCache`/
  `TraceWriter` headers" constraint as `GetHostViewportInfo`/
  `AddMemExportRanges` elsewhere in this file), but the specific vertex-rect
  computation it does is register/vertex-only, so it's narrowly
  reimplemented: read vf0's 3 vertices, fixed-point round per the top-left
  rasterization rule, apply the window offset, clamp to the scissor rect
  (`GetScissorRect`, a narrow reimplementation of `draw_util::GetScissor`,
  same file/same constraint), then clamp again to this renderer's fixed
  `kColorTargetWidth`x`kColorTargetHeight` bounds (this renderer doesn't
  emulate a real EDRAM surface sized by `RB_SURFACE_INFO` -- there's only
  ever the one fixed-size `color_target_image_`).
- Only `k_8_8_8_8`, non-array, plain-copy resolves are supported (same
  "narrow allow-list, skip+log the rest" pattern as texture formats/
  primitive types elsewhere in this file) -- confirmed sufficient since
  that's the gameplay-preview texture's own format.
- The actual copy is a synchronous mid-frame readback: ends the current
  render pass (color_target_image_'s `finalLayout` is already
  `TRANSFER_SRC_OPTIMAL`, matching `PresentFrame`'s own blit), copies the
  whole color target to a host-visible buffer via `vkCmdCopyImageToBuffer`,
  submits and waits on `submit_fence_`, then converts the covered rect from
  `VK_FORMAT_A2B10G10R10_UNORM_PACK32` (this renderer's fixed color-target
  format) down to RGBA8 on the CPU and writes it into guest memory at
  `RB_COPY_DEST_BASE` using `texture_util::GetTiledOffset2D` (the same
  tiled-addressing helper `GetOrUploadTexture` already uses for reads, now
  used symmetrically for a write) with the pitch from `RB_COPY_DEST_PITCH`
  (a plain texel pitch for this register, unlike a texture fetch constant's
  `pitch` field which needs `*32`).
- Since ending the render pass mid-frame is necessary for the CPU readback,
  but more draws may follow later in the same guest frame, a second render
  pass object (`render_pass_continue_`, same attachment but
  `loadOp=LOAD`/`initialLayout=TRANSFER_SRC_OPTIMAL` instead of
  `render_pass_`'s `CLEAR`/`UNDEFINED`) reopens recording into the same
  `framebuffer_`/`color_target_image_` right after the readback, so later
  draws this frame aren't lost and the existing per-frame present flow
  (`PresentFrame`, `EnsureFrameBegun`) needs no changes.
- Reuses the same cache-invalidation fix from step 16 (moved from being
  memexport-only to also covering resolve writes): any `texture_cache_`
  entry whose decode range overlaps what a resolve just wrote gets
  destroyed, so a texture resolved into every frame (like the gameplay
  preview) doesn't get stuck serving its first-ever upload the same way the
  (dead) memexport path would have.

**Not yet verified against a live capture/log** -- this was implemented and
builds clean, but the fix needs the same live-gameplay test that diagnosed
the bug: play to the gameplay-preview screen, wait a few seconds, and
confirm (via a fresh `.rdc` or by grepping `logs/*.log` for `"first EDRAM
resolve copy completed"` and checking the texture's content actually
changes frame to frame) that the preview is now live instead of frozen.

**Next:**
1. Verify live: rebuild, play to the gameplay-preview screen, confirm
   `logs/*.log` shows `"first EDRAM resolve copy completed"` and that a
   fresh capture's `512x256` texture content differs from `brk1`/`brk2`/
   `brk3`.
2. Once confirmed working, remove the now-confirmed-dead memexport code
   (`CollectMemExportRanges`, `pending_memexport_ranges_`,
   `memexport_ranges_ready_for_readback_`, and the memexport-specific
   temporary diagnostic logging added in steps 16-17) -- it's real,
   correctly-implemented code, but this game never exercises it, and
   keeping unreachable code around invites exactly the kind of wrong-turn
   debugging steps 16-17 just went through. Confirm dead (not just
   "unobserved in one session") before deleting -- e.g. grep across a
   longer play session, or check whether any other screen in the game might
   use it -- rather than assuming step 17's one session is exhaustive.
3. Items 3-4 from step 8's "Next" list (pathological-shader-stall root
   cause, remaining zeroed `SystemConstants` fields) still apply, along with
   step 14's still-open "Press &lt;A&gt;" text-rendering gap.

**Not fixed either -- user confirmed "not fixed at all" against a fresh
`brk4.rdc`.** See step 18: the EDRAM-resolve hypothesis was also wrong, for
this specific texture.

## Step 18 (2026-07-11, later session): the resolve was real but for a different texture; the actual mechanism is COHER_STATUS_HOST

Added per-resolve (not just first-ever) and per-texture-upload logging
instead of guessing again, and had the user play to gameplay once more
(`logs/nocturnerecomp_005.log` + siblings). Two findings, both direct from
the log:

1. **The EDRAM resolve from step 17 is real and fires every frame, but at
   `dest_base=0x1f6f8000`, always the full `1280x720` screen -- a
   completely different, unrelated texture from the gameplay preview.**
   Grepping `"texture uploaded"` across the whole session shows the actual
   512x256 gameplay-preview texture lives at **`base=0x1ed40000`** -- a
   different address that never once appears as an `RB_COPY_DEST_BASE`
   value anywhere in the log. Step 17's resolve support is legitimate
   working code for whatever the `0x1f6f8000` full-screen target is (a
   postprocess/motion-blur source, most likely), but it was never going to
   fix the gameplay preview -- that was a coincidental correlation from an
   earlier session's first, too-hasty grep of `RB_COPY_DEST_BASE` values
   that happened to only show one nonzero address in that shorter capture.
2. **The real signal was sitting right next to the texture upload log line
   the whole time:** `reg 0A30 COHER_BASE_HOST = 1ED40000` logged
   immediately before `texture uploaded 512x256 base=0x1ed40000` -- i.e.
   the guest issues a cache-coherency invalidation (`COHER_BASE_HOST`/
   `COHER_SIZE_HOST`/`COHER_STATUS_HOST`) targeting exactly this texture's
   address right when it wants the GPU to treat that range as freshly
   written. Confirmed against `rexglue-sdk/src/graphics/command_processor.cpp`'s
   `CommandProcessor::MakeCoherent()`: this is the real, general,
   documented Xenos mechanism for "something other than a normal draw (CPU,
   DMA, any path outside the GPU's own EDRAM pipeline) wrote this guest
   memory range -- any texture/vertex cache needs to invalidate it,"
   triggered whenever `COHER_STATUS_HOST.status` is set (typically from a
   `WAIT_REG_MEM`-adjacent coherency event). This is *not* memexport (step
   15-16) and *not* an EDRAM resolve (step 17) -- it's the guest explicitly
   telling the GPU "this memory changed," probably because the actual pixel
   data is written by something entirely outside this renderer's PM4
   decode (CPU/software rendering, matching the original step 15 theory
   from the user's xenos-backend session) and the coherency event is the
   *only* GPU-visible trace of that write ever happening.

**Fix (`native_command_processor.{h,cpp}`):** `OnPacket`'s existing
register-write loop (which already mirrors every decoded register write
into `registers_`) now checks whether the just-written register is
`COHER_STATUS_HOST`, and if so calls the new `InvalidateTextureCacheRange`
with the current `COHER_BASE_HOST`/`COHER_SIZE_HOST` values -- this is the
first general, content-driven texture-cache invalidation in this renderer;
everything before it (steps 16-17's memexport/resolve-specific paths) only
covered the two GPU-side write mechanisms this renderer happens to model,
neither of which turned out to be what this texture actually uses.

**Safety subtlety, worth recording:** `OnPacket` runs live as PM4 packets
stream in, potentially in the middle of recording the current guest frame's
command buffer -- a texture invalidated there might already be bound in an
earlier draw recorded (but not yet submitted/executed) in that same buffer.
Destroying its `VkImage` immediately, the way step 16/17's fence-adjacent
invalidations safely could, would be a use-after-free once the GPU actually
executes that earlier draw. Fixed by making `InvalidateTextureCacheRange`
only *queue* `(base, size)` pairs (`pending_texture_cache_invalidations_`)
instead of destroying anything itself; the actual destroy+erase now happens
only in `EnsureFrameBegun`, right after its existing fence wait -- the same
point `FreeTransientBuffers()` already relies on for "the GPU is definitely
done with everything from the previous frame." Steps 16/17's memexport- and
resolve-specific invalidation call sites were refactored to go through this
same queued path too (previously they destroyed inline, which happened to
be safe only because both of those call sites already ran right after their
own fence waits) -- one mechanism, one safety argument, instead of three
separate copies of the same destroy loop.

**Verified live by the user:** the gameplay-preview texture now updates
frame to frame instead of being stuck on its first upload. This is the
actual fix for the symptom that motivated steps 15-18 -- the
`COHER_STATUS_HOST` cache-coherency invalidation is the real mechanism, not
memexport (step 15-16, confirmed dead below) or the EDRAM resolve (step 17,
real but for an unrelated target).

**Post-fix cleanup (same session):** per user instruction, once the fix was
confirmed live:
- **Removed the memexport code entirely.** `CollectMemExportRanges`,
  `PendingMemExportRange`, `pending_memexport_ranges_`,
  `memexport_ranges_ready_for_readback_`, and the associated temporary
  diagnostic logging are gone from `native_command_processor.{h,cpp}`. Two
  full sessions (step 16's ~80s live-gameplay session and step 18's) never
  observed a single shader with `eM_written` set across intro, menus, and
  actual gameplay -- confirmed dead, not just "unobserved once." If a
  future game/mod actually needs memexport, re-derive it from
  `rexglue-sdk/src/graphics/util/draw.cpp`'s `AddMemExportRanges` rather
  than resurrecting this removed version verbatim (worth re-checking the
  real backend for any changes since).
- **Kept the EDRAM-resolve support (`TryResolveCopy`, `render_pass_continue_`,
  `GetScissorRect`).** Unlike memexport, step 18's own logging confirmed
  this fires every single frame, unconditionally, for a real (if currently
  unidentified-purpose) `1280x720` target at `dest_base=0x1f6f8000` --
  genuinely live code, not dead code, even though it turned out to be
  unrelated to the bug these steps were chasing.

**Next:**
1. Items 3-4 from step 8's "Next" list (pathological-shader-stall root
   cause, remaining zeroed `SystemConstants` fields) still apply, along with
   step 14's still-open "Press &lt;A&gt;" text-rendering gap.
2. The `0x1f6f8000` full-screen EDRAM-resolve target's actual purpose is
   still unidentified -- worth investigating if a future visual bug traces
   back to it (candidates: motion blur, a bloom/postprocess source, or a
   second full-screen preview elsewhere in the game).
3. The temporary per-resolve/per-texture-upload diagnostic logging added in
   step 18 (`debug_resolve_logged`, `debug_upload_logged`) is still in the
   tree, capped at 100 lines each -- low-impact to leave, but should be
   removed or `REXCVAR`-gated eventually, matching this file's usual policy
   on temporary diagnostics.
