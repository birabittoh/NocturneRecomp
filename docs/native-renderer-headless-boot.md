# Native renderer: getting a headless boot working

Working notes from the first steps toward replacing the general-purpose
xenos GPU plugin with a game-specific Vulkan renderer. Xenos translates
Xbox 360 GPU calls to Vulkan/D3D12 but supports every 360 title, so it's
heavier than a renderer written only for this game needs to be.

The plan is incremental: first get the game running with `--gpu_plugin`
removed entirely — black screen, but audio playing and menus navigable —
before writing a single line of the real renderer. This document is the
log of that first step.

**Status: headless boot works.** With the fixes below, the game boots
fully headless — audio plays (confirmed audible), and the guest runs its
normal frame loop indefinitely instead of hanging. See "Resolution" below
for the final fix and "Next: basic textures" for where this picks back up.

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

### Finding: headless boot never reaches the real intro command stream

With the above fixes, a decoded capture from process start shows **one**
`PM4_ME_INIT` (19 dwords) followed by **repeating, essentially static**
traffic for the entire ~20,000-packet capture window: a 3-dword
`PM4_INDIRECT_BUFFER` packet (alternating `predicated=true/false`) pointing
at what appears to be the same self-referencing jump, plus one `PM4_TYPE0`
register write per cycle to a rotating scratch register (`BIOS_7_SCRATCH`
`0x000B`, and two others the register table doesn't name — `0x005A`,
`0x1274`) always with the same value `0xC0013F00`. This repeats on an
almost exactly 100ms cadence — i.e., this is the `Headless VBlank` thread's
keepalive tick (see `StartHeadlessVblankThreadIfNeeded`), not the game
submitting real per-frame draw commands. **No register writes for viewport,
render-target format, texture fetch constants, or vertex fetch constants
appear; no `PM4_DRAW_INDX`/`_2`; no `PM4_XE_SWAP`.** This is despite the
capture running well past the point where `XMP: started BGM playlist` fires
and audio is confirmed playing, so the guest is genuinely running real game
logic — it's specifically GPU submission that never happens.

**Scope conclusion:** the intro's real texture/draw/shader command stream is
gated behind something that doesn't fire in headless mode at all — most
likely the game's actual per-frame GPU submission path checks something
tied to `config.graphics` / the D3D device / a real vsync signal that only
the `HeadlessRingWaitBypass`-patched wait loops and the VBlank keepalive
thread currently stand in for. Finding *that* gate is a prerequisite for
Phases 2-4 of the renderer plan (there's no draw stream to interpret or
texture to upload yet) and needs the same lldb-backtrace-driven RE technique
used to find the original ring-wait spin (see "Reproducing this" above),
starting from where the VBlank keepalive vs. real submission paths diverge.
This changes the phase-2 plan's assumption that Phase 1 would just be a
scope-narrowing exercise — it surfaced a second, deeper hang/gate that has
to be resolved first.
