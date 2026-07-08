# Native renderer: getting a headless boot working

Working notes from the first steps toward replacing the general-purpose
xenos GPU plugin with a game-specific Vulkan renderer. Xenos translates
Xbox 360 GPU calls to Vulkan/D3D12 but supports every 360 title, so it's
heavier than a renderer written only for this game needs to be.

The plan is incremental: first get the game running with `--gpu_plugin`
removed entirely — black screen, but audio playing and menus navigable —
before writing a single line of the real renderer. This document is the
log of that first step, which is still in progress.

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

**This is where the D3D device struct reverse engineering mentioned above
actually needs to start** — guessing further from raw PPC disassembly
without a labeled struct layout is unproductive. Next session should
begin there, probably by:

1. Confirming `r31` is in fact the D3D device pointer (cross-reference
   against `game_symbols` mod data / known SOTN struct dumps if any
   exist).
2. Tracing where `r30` is set in the callers above
   (`sub_82525630`/`sub_82534378`/`sub_82534910`) to determine what value
   it actually holds relative to the ring buffer state.
3. Deciding whether "make the GPU look infinitely fast" is even the
   right mental model here, versus "make ring buffer space always look
   available" (a related but distinct invariant).

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
— the working xenos boot reaches `XMP: started BGM playlist` within
~100ms of `KernelState: Preparing module launch...`; headless currently
never gets there. `logs/` is gitignored and each run appends a new
numbered file, so `rm -f logs/*.log` before a run keeps things readable.

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
