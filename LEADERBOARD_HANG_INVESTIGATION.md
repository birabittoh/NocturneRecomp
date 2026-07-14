# Main-menu Leaderboards softlock — investigation notes

## Symptom
Two "Leaderboards" entries exist: one in the main menu, one in the pause menu.
Pause menu works correctly (shows "Xbox Live is not available" error screen).
Main menu softlocks the game completely — frozen, no button input works, nothing
in the logs.

## Confirmed via live debugging (lldb, host process)
- The hang is on the guest "Main XThread". It is **not** blocked in a Win32 wait —
  the CPU is actively executing, cycling through a small set of addresses
  repeatedly (confirmed via rapid PC sampling across multiple attach sessions).
- Reading the live `PPCContext` directly (found by noticing `rdi` holds the ctx
  pointer inside these hot functions — confirmed because `rdi+0x100` == `ctx.lr`
  consistently pointed at valid guest `.text` addresses, and `PPCContext::r3` is
  at offset 0, `lr` at offset 0x100 per `rex/ppc/context.h`) shows `ctx.lr`
  cycling between exactly:
  - `0x82525358`
  - `0x8252F848`
  - `0x8252F8CC`

  These are all within/around `sub_825252C8` and `sub_8252F840` (see below).
  Confirmed as a genuine repeating loop across 6 samples taken seconds apart —
  this is real, not a sampling artifact.

## The actual loop (IDA decompilation)

`sub_825252C8` (guest `0x825252C8`, size `0xC4`) — a generic queue/message pump:

```c
int __fastcall sub_825252C8(int result, int a2, int a3) {
  int v4 = result;
  if (a2 && *(DWORD*)(result+10780) - a2 < (uint)(*(DWORD*)(result+10780) - **(DWORD**)(result+10768))) {
    if (a2 == *(DWORD*)(result+10780)) {
      if (*(DWORD*)(result+12944)) return result;
      result = sub_825259A0();
    }
    if (*(DWORD*)(v4+10780) - a2 < (uint)(*(DWORD*)(v4+10780) - **(DWORD**)(v4+10768))) {
      sub_8252F608(v6, v4, a3);
      while (*(DWORD*)(v4+10780) - a2 < (uint)(*(DWORD*)(v4+10780) - **(DWORD**)(v4+10768))
             && sub_8252F840(v6))
        ;
      return sub_8252F638(v6);
    }
  }
  return result;
}
```

`sub_8252F840` (guest `0x8252F840`) — the actual wait-with-timeout:

```c
int __fastcall sub_8252F840(int *a1) {
  int v3 = *a1;
  ...
  if ((*(BYTE*)(v3+10813) & 4) == 0) {
    ...
    if ((uint)(v5 - a1[3]) < 0x1388)   // 0x1388 = 5000 (ticks? ms?)
      return 1;                        // keep waiting
    sub_825351A0(v3);                  // timeout handler
  }
  return 0;                            // stop waiting
}
```

So this is a **timed wait/retry loop**: it returns 1 (keep spinning) for up to
~5000 time units, then calls a timeout handler `sub_825351A0` and returns 0.
The outer `while` in `sub_825252C8` re-enters this repeatedly. Live sampling
shows `ctx.lr` revisiting this exact function repeatedly over much longer than
5 seconds of real wall-clock time, meaning: it times out, something re-triggers
the wait, it times out again, forever. This is a **retry loop with no permanent
give-up condition**, not a literal infinite spin in one instruction.

`sub_825252C8` has **16 call sites** across the binary (not leaderboard-specific
— it's shared generic engine infrastructure, likely a "wait for async
completion with timeout" utility used by many systems). Which caller is active
in the leaderboard-menu case was **not** identified before time ran out.

## What was tried and ruled out / partially fixed

Initial static analysis (via `search_text` for the string
`"ERROR - LoadLeaderboard failed\n"` at `0x82015224`) found two functions
referencing it:

- `sub_825C2CB0` — the **pause-menu** message dispatcher. Explicitly checks
  `XamUserGetSigninState(user) == eXamUserSigninState_SignedInToLive` **before**
  doing anything else; if not signed in, shows an error dialog via
  `sub_825CE8E8` and returns early. This is why the pause-menu path works.
- `sub_825C0848` — does the actual XAM leaderboard load calls
  (`sub_82584928` / `sub_82586810` / `sub_82586808` against the static
  controller object `dword_82E60B28`), with **no signin check** of its own.

`sub_825C1AB8` (guest `0x825C1AB8`) — the main-menu leaderboard screen's
init/OnCreate handler (only xref: a vtable slot at `0x82222180`) —
unconditionally calls `sub_825C0848` at `0x825C1CB0`
(`lwz r4,636(r31); lwz r5,536(r11); bl sub_825C0848`), unlike the pause-menu
path which gates the equivalent call behind the signin check first.

**Fix applied** (still in the repo, does not fully resolve the hang):
- `nocturnerecomp_config.toml`: added a `[[midasm_hook]]` at guest address
  `0x825C1CB0` (the `bl sub_825C0848` call site inside `sub_825C1AB8`), name
  `LeaderboardMenuSigninGuard`, `jump_address_on_true = 0x825C1CB4` (skips the
  call to `sub_825C0848` entirely if not signed in to Live, jumping straight to
  the following instruction — mirrors the pause-menu's early-out).
- `src/nocturnerecomp_hooks.cpp`: implements `LeaderboardMenuSigninGuard()`,
  calling the guest `XamUserGetSigninState(0)` import via
  `REX_IMPORT(__imp__XamUserGetSigninState, ...)` and returning true (skip) if
  the result isn't `2` (`eXamUserSigninState_SignedInToLive`).

This builds and links cleanly and the guest code was confirmed (via reading
`generated/nocturnerecomp_recomp.28.cpp`) to emit the guard correctly. **User
confirmed after rebuilding that the main-menu hang is visually identical to
before** — no observable change.

## Why the fix (as applied) likely isn't enough

Skipping the call to `sub_825C0848` means the leaderboard load never starts —
but something else (very plausibly whatever calls into the
`sub_825252C8`/`sub_8252F840` wait/retry loop found above, quite possibly
operating on the same `dword_82E60B28` leaderboard-controller singleton)
appears to be **separately** waiting for that data to become available, with
no permanent give-up path, and retries every ~5 seconds forever regardless.
The signin-guard fix stops the *load* from hanging but doesn't stop the
*waiter* from hanging, if it's a distinct code path with its own retry logic.

Also worth double-checking: is `sub_825C1AB8` actually even the function
invoked when clicking Leaderboards from the main menu? The only xref to it is
a single vtable-slot data reference (`0x82222180`) with **no discoverable
xrefs to that vtable address itself** (IDA's static analysis didn't resolve
whatever code loads that vtable pointer, likely a split `lis`/`addi` pair it
didn't correlate) — so it was never directly confirmed that main-menu
Leaderboards actually constructs an object with this vtable. This should be
verified before further chasing `sub_825252C8`'s callers.

## UPDATE (2026-07-09): root cause is NOT leaderboard/network code — it's a GPU command-ring-buffer stall

Follow-up live session with a working technique (see `scripts/resolve_gpu_wait.py`,
committed): rather than guessing register-holds-ctx by inspection, the script
attaches to the live process, scans process memory for the `PPCFuncMappings`
table (generated in `generated/nocturnerecomp_init.cpp`, an array of
`{guest_addr, host_func_ptr}` pairs — always present in memory, no PDB
needed), and uses it to resolve **any** host address (RIP, or values found by
scanning the host stack) back to an exact guest function + offset. This
gives a real, high-confidence guest call chain instead of guessing from a
single `lr` sample.

Reconstructed live call chain during the hang (leaf first):

```
sub_8252F840          <- leaf: wait up to 5000 timebase ticks for a fence to advance
  sub_825252C8+0x3ae   <- generic wait/retry pump (already known from original investigation)
  sub_82525630+0x67c
  sub_82534378+0x19ab
  sub_82534910+0x638
  sub_825134E0+0x84f   <- GPU command-ring-buffer packet submit (see below)
  sub_825D1618+0x220   <- generic "for each active screen: call vtable Update()" dispatcher
  sub_82578A30+0x286   <- Application::Run — the main loop, called once from start()
```

`sub_825134E0` writes raw GPU command packets (PM4-style `0xC0013F00`
type-3 headers) directly into a ring buffer at `a1+48`; when the ring is
full (`*(a1+48) > *(a1+56)`) it calls `sub_825259A0` to wait for space —
which is exactly the path into the `sub_825252C8` fence-wait pump.
`sub_825259A0` was already one of the 16 known callers of `sub_825252C8`
found in the original investigation, but its role (ring-buffer-full wait)
wasn't identified there.

The `sub_825351A0` timeout handler (called every ~5000 ticks when the wait
gives up) is Xbox 360 D3D's classic "GPU hang" detector. On timeout it
checks a device-state field; on the *recoverable* path it just silently
sets `dword_82E4F580 = 15` (a "device needs recovery" flag, via
`sub_82532368`) and returns — **no log output on this path**, which is why
nothing appeared in the logs despite this being the real loop location.
Only the fatal branch (`__trap()`, unreached here) prints the "GPU is hung"
strings.

Checked the actual GPU-side worker threads via lldb (`GPU Commands
(F8000018)`, `GPU VSync (F800001C)`) — both are cleanly idling in `SleepEx`
inside `rexgpu-xenos.dll`, **not** blocked or stalled on anything. This
means the ring buffer's fence/consume side is healthy but has nothing
queued to process — i.e. the ring-buffer "kick"/notify from the main thread
is either never reaching the GPU thread, or the backend isn't advancing the
consumed-fence counter for whatever was submitted.

**Working theory**: the main-menu Leaderboards screen's `OnCreate`
(`sub_825C1AB8`) does a burst of font/text-layout calls (`sub_825CFC68`,
`sub_825CEFA0`) building UI elements synchronously, generating enough GPU
state/draw packets to fill the ring buffer before the GPU thread gets
kicked/scheduled to drain it — a race/starvation condition specific to this
screen's init-time packet volume, not present on the pause-menu path (which
early-outs on signin state before doing this UI construction). This would
make it a genuine SDK-level (`rexgpu-xenos`/`rexruntime`) ring-buffer
kick/scheduling bug, not something fixable purely within NocturneRecomp's
game-side hooks — though a guest-side signin/early-out guard (already
partially applied for the pause-menu-equivalent early-out) could still be a
practical workaround if it prevents the UI construction that triggers the
burst.

Not yet done: pin down exactly *why* the ring buffer never drains — check
`rexgpu-xenos.dll`'s ring-buffer kick/consume implementation (in
`../rexglue-sdk`) for how it's signaled from `sub_825134E0`'s caller chain,
and compare packet volume between the pause-menu and main-menu leaderboard
paths to confirm the "burst fills the ring before first kick" theory.

## Suggested next steps

1. Find the actual caller of `sub_825252C8` active during the main-menu
   leaderboard hang. Candidate approach: live-attach during the hang (as
   above), but capture the **first argument** (`result`, i.e. `ctx.r3` at
   entry to `sub_825252C8`) reliably — needs breaking at function entry rather
   than sampling mid-function, since `r3` gets reused as scratch shortly
   after. IDA MCP is available and already has `default.xex` loaded (session
   was opened via `idb_open` with `mode: force_gui`, adopting the running IDA
   GUI instance — reuse `idb_list` to find the current session id) — could
   set a breakpoint via a live debugging workflow if the IDA plugin supports
   attaching to the running host process (untested; the recompiled x64 binary
   isn't a literal PPC image, so this may not be directly possible — the
   `PPCContext`-in-`rdi` technique documented above is the fallback).
2. Confirm whether `sub_825C1AB8` is genuinely the main-menu entry point by
   finding what constructs an object with vtable `0x82222180` (search for the
   `lis`/`addi` pair loading that constant, or check `sub_825259A0` /
   `sub_8252F608` / `sub_8252F638` context for clues about which subsystem
   owns the `sub_825252C8` object).
3. Once the real waiter is found, the likely correct fix is the same pattern
   as the pause menu: check `XamUserGetSigninState` before ever entering the
   wait, and/or make `sub_825351A0` (the timeout handler) permanently give up
   after one timeout instead of allowing an outer retry — whichever matches
   the game's intended behavior on repeated Live-unavailable failures.
4. The existing `LeaderboardMenuSigninGuard` hook (config + hooks.cpp) is
   harmless and arguably still correct/desirable to keep regardless — it
   prevents `sub_825C0848` from ever being called when not signed in, which
   is the right behavior on its own merits, just not sufficient by itself.

## UPDATE (2026-07-09, later same day): two things tried, both insufficient

**Tried: `SwitchToThread()` in a void midasm hook at `sub_8252F840` entry**
(CPU-starvation theory). No effect — confirmed via `scripts/resolve_gpu_wait.py`
that the hook fires (caught RIP inside the `SwitchToThread` syscall, same
guest call chain as before) but `GPU Commands` stayed idle in `SleepEx`
regardless. Ruled out scheduling starvation as the cause. This version is
still in the tree (harmless, doesn't fix anything).

**Tried and reverted: forcing `sub_8252F840` to return 0 ("stop waiting,
space available")** after ~2s of continuous retrying, via a `return_on_true`
bool hook setting `r3=0`. This is unsafe: the caller believes the ring
actually has space and writes its packet into it anyway, without the ring
having actually been drained. Observed effect when tested: background
particle effects visibly sped up (the per-frame Update loop was no longer
gated by the fence, so it free-ran) but the Leaderboards screen still never
became interactive — same underlying non-drain problem, just no longer
blocking, so it manifests as runaway frame updates with silent failure
instead of a hang. Do not reintroduce this without first fixing why the
ring never drains — see below.

Root blocker still unresolved: `sub_825257F8` (the ring's flush/kick path)
only actually reaches hardware via `sub_82525630` in the wraparound case,
and execution is already inside `sub_82525630` on the stack when the wait
triggers — i.e. the flush that would free space can't itself make forward
progress. `rexgpu-xenos.dll` (the actual GPU backend consumer) is a
prebuilt SDK plugin with no source checked out under `../rexglue-sdk` in
this environment, so its consume-side logic couldn't be inspected this
session. Next steps if resuming: either (a) find/build `rexgpu-xenos`
source and check what actually wakes `GPU Commands` from its `SleepEx` loop
and why it doesn't see this submission, or (b) pursue the original
suggestion of skipping the packet-burst-generating parts of
`sub_825C1AB8`'s OnCreate (the font/text layout calls) when not signed in
to Live, mirroring how the pause-menu path avoids constructing this UI at
all — never generate the burst rather than trying to survive it.

## UPDATE (2026-07-09, later still): GPU-ring-buffer theory was wrong; found the real OnCreate/Update, still not root-caused

A full round of testing disproved the GPU-ring-buffer-stall theory above and
found the actual (still-unfixed) blocker is elsewhere. Summary, newest
conclusion first:

**The GPU-wait call chain above is normal, healthy engine activity, not a
stall.** Added a read-only counting hook at `sub_825134E0` (the ring-buffer
packet submit function) logging call frequency and argument deltas. Result:
a steady ~122 calls/sec with distinct, changing arguments every time — not
a single packet retried forever, and not a runaway/corrupted loop. This
call chain shows up in *every* live sample regardless of what's on screen
(confirmed by re-sampling later with totally different hooks applied) —
it's just what Main XThread is doing at any given instant during normal
per-frame rendering. All earlier conclusions built on "this call chain =
the hang" (the `SwitchToThread` yield test, the ring-buffer-full theory,
the `rexgpu-xenos` consumer-starvation theory) were chasing a red herring.
The `GpuRingBufferWaitYield` hook at `sub_8252F840` is harmless and can be
removed; it was never the fix.

**`sub_825C1AB8` — the function every earlier hook in this doc targeted —
is never called for main-menu Leaderboards.** Its original identification
(a `find_bytes` hit for its address at `0x820154D4`) turned out to be a
real data table, but the *wrong entry*: `0x820154C0` is a genuine 6-entry
table of per-menu-item OnCreate handlers
(`0x825D1698, 0x825D1710, 0x825D13E8, 0x825C0E80, 0x825C1AB0, 0x825C1AB8`),
and Leaderboards is entry #4 (`sub_825C0E80`), not entry #6
(`sub_825C1AB8`). Confirmed empirically: a probe hook placed at
`sub_825C1AB8`'s entry never fired despite repeatedly navigating into
Leaderboards and checking the log. Also confirmed the softlock is
genuinely leaderboard-specific, not a generic navigation bug (other
main-menu submenus transition normally).

**The signin-state theory (this whole file, up to this point) is also
disproven as the cause**, independent of the wrong-function issue: the
SDK's `user_profile()->signin_state()` is hardcoded to always return `1`
(signed in locally, never Live —
`../rexglue-sdk/include/rex/system/xam/user_profile.h:221`). The
`LeaderboardLoadSigninGuard` hook (guarding `sub_825C0848` at its own
entry, covering every call site) has therefore fired unconditionally on
every build tested, fully skipping the load every time, and the softlock
still happens identically. Kept in the tree since it's harmless and
arguably correct regardless, but confirmed **not** the fix.

### The real OnCreate/Update, found via live tracing

Built `scripts/trace_leaderboard_entry.py`: continuously samples Main
XThread (RIP + a host-stack scan) via raw `ReadProcessMemory`/
`GetThreadContext` polling (no lldb attach overhead, ~30ms per sample),
resolving every host address through the same live `PPCFuncMappings`
table technique as `resolve_gpu_wait.py`, and prints any guest function
seen for the first time (filtering out the already-known always-running
engine/render chain). Started it before clicking into Leaderboards from
the main menu and it surfaced a burst of new functions right around the
click, including `sub_825C0A88` and (via a follow-up static xref check)
its sibling `sub_825C0E80`.

Confirmed via decompilation:
- **`sub_825C0E80` is the real OnCreate.** Creates the tab-nav arrows
  (`arrowsLTn`/`arrowsRTn`), background (`BGshapeLG`), tab line, title/
  subtitle text, and 6 rows of leaderboard entry text (indices 0-5). Ends
  by setting `*(a1+660)=5`, `*(a1+844)=*(a1+848)=*(a1+852)=0`,
  `*(a1+856)=0.0` (an animation/state block).
- **`sub_825C0A88` is the real per-frame Update.** Animates a lerp value
  at `a1+856`/`a1+860`, checks `*(a1+660) != 5` plus elapsed time before
  conditionally calling `sub_825C0848` (the load) — so the load is only
  triggered lazily well after OnCreate returns, not from OnCreate itself.
  Also handles tab-switch text refresh logic (`a1+656` vs the current tab
  index).

Retargeted the diagnostic probes to these real functions and rebuilt.
Findings from live testing:
1. **OnCreate runs successfully, twice**, with two different object
   pointers, both returning valid non-null results (no allocation
   failure). Plausibly one call is a main-menu preview widget and one is
   the real full screen (CLAUDE.md notes two separate "Leaderboards"
   entries exist in the game — main menu and pause menu — so a doubled
   construction isn't inherently suspicious).
2. **`sub_825C0A88` (Update) never fires — not even once** — despite the
   screen visibly rendering (background continues animating normally) and
   the softlock persisting. This is the real bug: the screen gets built
   but never receives per-frame ticks (or, most likely, input), while
   rendering elsewhere in the engine continues unaffected.

### Tracing the missing registration (where this session stopped)

Captured `sub_825C0E80`'s real host call stack from *inside* the hook
itself (`RtlCaptureStackBackTrace`, called in-process — no attach needed)
and resolved it: `start()` → `sub_82578A30` (`Application::Run`, the main
loop) → `sub_8258B8A0+0x65e` → `sub_825ABED0` → `sub_825C0E80`.

- `sub_825ABED0` is the front-end's one-time init function ("FE Init" per
  an embedded log string) on its *first* call; on every subsequent call
  (guarded by `dword_82E7A570`) it just tail-calls
  `sub_825CFF90(a1-148, 1, 0, -1)` — called every frame from the main
  loop, always with a **hardcoded page index of `1`**, not whatever menu
  item is currently selected.
- `sub_825CFF90` is a generic "switch to page N" function: looks up
  `*(DWORD*)(4*(a2+3) + container)` (a per-page object array), calls the
  *previous* page's vtable+12 (deactivate) if changing, then calls the
  *new* page's **vtable+60** with `(a3, a4)`.
- Since `sub_825ABED0` always requests page `1`, and OnCreate for
  Leaderboards specifically only runs when actually navigating there, the
  vtable+60 implementation for whatever object lives at page index `1`
  must be the real "front end tick", and it must be the one internally
  responsible for lazily constructing (once) and then per-frame-updating
  the currently-selected *sub*-page (Leaderboards) via some nested index
  — this nested dispatcher was not identified before stopping. (A raw
  stack-scan attempt to get more frames than `RtlCaptureStackBackTrace`
  returned came up empty — likely an implementation bug in the ad hoc
  scan, not evidence of anything; `RtlCaptureStackBackTrace`'s frame-walk
  is the reliable data point here and it bottoms out at `sub_825ABED0` as
  the immediate (post-inlining) caller of OnCreate, so the interesting
  nested-dispatch code is guest-side logic not visible as a separate host
  stack frame.)

**Next step if resuming**: find the vtable+60 implementation for the page
index `1` object (likely by decompiling around `sub_825CFF90`'s callers/
the page-array construction in `sub_825ABED0`'s one-time-init body, or by
live-probing candidate functions near `sub_825C0A88`/`sub_825C0E80` in the
same way `trace_leaderboard_entry.py` found them) and check what it's
supposed to do differently for Leaderboards vs. working submenus to decide
"call Update every frame" vs. never.

### Current state of the tree

Only `LeaderboardLoadSigninGuard` (guards `sub_825C0848` at its own entry)
remains in `nocturnerecomp_config.toml`/`src/nocturnerecomp_hooks.cpp`.
Harmless, arguably correct, **confirmed not sufficient to fix the
softlock**. All diagnostic probe hooks and the GPU-wait yield hook from
earlier sessions were removed after use.

### Reusable tooling from this session

- `scripts/resolve_gpu_wait.py` — attach to the live process, locate the
  `PPCFuncMappings` table in memory (scans for the known first entry
  `{0x82230000, host_ptr}`, no PDB needed), resolve Main XThread's RIP and
  a host-stack scan to exact guest functions.
- `scripts/trace_leaderboard_entry.py` — continuously poll Main XThread
  (lightweight `ReadProcessMemory`/`GetThreadContext`, no lldb) over a
  time window, resolve every sample via the same table technique, and
  print newly-seen guest functions live — used to find real UI entry
  points without needing a priori knowledge of which function to target.
- In-process `RtlCaptureStackBackTrace` from inside a midasm hook (see
  git history of `src/nocturnerecomp_hooks.cpp` around this update) is a
  cheap way to get an exact host call stack at a specific guest PC without
  any external attach at all — resolve the addresses afterward with the
  same table technique.

## UPDATE (2026-07-09, final): ROOT CAUSE FOUND AND FIXED — blocking spin in the page-activate method

Resolved via static analysis in IDA, building on the previous session's
correct identification of the OnCreate/Update pair. No live tracing needed
this round.

### Mapping the real vtable

The previous session's "6-entry OnCreate table at 0x820154C0" and the
"vtable slot at 0x822220d8" were both partial/misread views. Corrected:

- **`0x82222080..` is the Leaderboards class message-map** — `{handler,
  event_hash}` pairs (OnCreate hash `0xC0006CC6`, Update hash `0x40103C43`).
  A red herring for the dispatch question; not the C++ vtable.
- **`0x82015498` is the Leaderboards screen class's real C++ vtable.** Slots
  (byte offset from base):
  - +16 `0x825D15F0` — Show: `byte[obj+0xD] = 1`
  - +20 `0x825D1600` — Hide: `byte[obj+0xD] = 0`
  - +24 `0x825D1610` — IsVisible gate: `return byte[obj+0xD]`
  - +28 `sub_825C0A88` — Update (per-frame)
  - +52 `sub_825C0E80` — OnCreate
  - +60 `sub_825C1AB8` — **page-activate** (called on transition into the page)

- The container's per-frame Update (`sub_825D1618`) and Draw (`sub_825D14C0`)
  both iterate children and gate each child on vtable+24 (i.e. `byte[obj+0xD]`)
  before calling Update(+28) / Draw(+32). So a child that never gets Shown
  (byte+0xD stays 0) is silently skipped for both update and draw. This is
  why "OnCreate ran but Update never fired."

### The actual hang

`sub_825C1AB8` (vtable+60, page-activate) runs on main-menu entry and
contains a **blocking busy-spin** (guest `0x825C1B24..0x825C1B44`):

```c
sub_82584088(&dword_82E60B28, a1+536);              // register XANet listener
do { while (sub_82584C18(&dword_82E60B28) == 1) ; } // spin while status == 1
while (sub_82584C18(&dword_82E60B28) == 2);          // repeat while status == 2
```

`sub_82584C18` just returns `*(dword_82E60B28 + 148)` — the async leaderboard
read status. With Xbox Live unavailable (SDK hardcodes `signin_state()` to 1 =
local, never Live), that read never reaches a terminal status, so this loop
spins **forever on the main XThread, inside the page transition, before the
screen is ever Shown.** That accounts for every symptom: input frozen, no log
output (pure busy spin), OnCreate already ran, Update never reached (activate
never returns to let the container tick/draw the child). The pause menu avoids
it by checking signin and showing "Xbox Live not available" instead of ever
activating this page.

Note this also finally explains the very first session's live finding (a real
repeating loop) — the GPU-wait chain was indeed a red herring, but there *was*
a genuine spin; it's this one, at the guest level, not the GPU ring buffer.

### The fix (applied, builds clean, emitted correctly)

New `[[midasm_hook]]` `LeaderboardMainMenuSpinGuard` at `0x825C1B24` (spin-loop
top) with `jump_address_on_true = 0x825C1B44` (first instruction past the
spin). The hook (`src/nocturnerecomp_hooks.cpp`) returns true when
`XamUserGetSigninState(0) != SignedInToLive`, so when Live is unavailable the
spin is skipped entirely and activation proceeds — the screen becomes
interactive with empty leaderboard data instead of hanging, the same graceful
outcome the pause menu already gives. When genuinely signed in to Live the
spin runs normally and exits on read completion. Verified the guard is emitted
in `generated/nocturnerecomp_recomp.28.cpp` (`if (LeaderboardMainMenuSpinGuard())
goto loc_825C1B44;`). `LeaderboardLoadSigninGuard` is kept alongside it.

**Verification status:** builds and links; guard confirmed in generated code.
Runtime GUI confirmation (Main Menu -> Leaderboards no longer hangs) still
needs a manual play-test — could not drive the GUI from the analysis
environment.

## UPDATE (2026-07-09, session 6): spin fix confirmed real-but-insufficient; "IsVisible never set" theory DISPROVEN; diagnostic probe added

The `LeaderboardMainMenuSpinGuard` fix from session 5 is genuinely correct (it
stops a real infinite busy-spin — verified live earlier), but the user confirms
the game **still softlocks**: background animation keeps running while the
Leaderboards screen never responds to input. So there is a second, independent
bug. This session was static analysis to narrow it down before spending a live
run.

### The vtable, fully mapped (corrects prior partial reads)

Read the raw vtable bytes at `0x82015498`. Full slot map (byte offset → guest):
- +0  `0x825C2890`
- +4  `0x825C2CB0`
- +8  `0x825D1538` — **container-activate**: sets `byte[obj+0xC]=1`, calls each child's vtable+8
- +12 `0x825C1CD8` — **deactivate**: → `sub_825D1598` sets `byte[obj+0xC]=0`, calls each child's vtable+12
- +16 `0x825D15F0` — Show: `byte[obj+0xD]=1`
- +20 `0x825D1600` — Hide: `byte[obj+0xD]=0`
- +24 `0x825D1610` — IsVisible: `return byte[obj+0xD]`
- +28 `0x825C0A88` — Update (per-frame)
- +32 `0x825D14C0` — Draw
- +52 `0x825C0E80` — OnCreate
- +56 `0x825C1AB0` — (bare `blr`, no-op)
- +60 `0x825C1AB8` — page-activate (contains the now-guarded spin)

Two distinct flag bytes: **byte 0xC = "activated"**, **byte 0xD = "visible"**.
The per-frame dispatcher `sub_825D1618` gates each child's Update(+28) on
vtable+24 == `byte[child+0xD]` (visible).

### KEY CORRECTION: `byte[obj+0xD]` (IsVisible) defaults to 1 at construction

The base widget constructor `sub_825D1330` (called from the class ctor
`sub_825C2BD0` → base ctor `sub_825D1CD8` → `sub_825D1330`) ends with:
```c
*(_BYTE *)(a1 + 12) = 0;   // activated = 0
*(_BYTE *)(a1 + 14) = 1;
*(_BYTE *)(a1 + 13) = 1;   // visible/IsVisible = 1  <-- DEFAULTS TRUE
```
So the Leaderboards object is **visible by default**. Nothing in the
activation path Hides it (page-switch `sub_825CFF90` calls old page's
vtable+12/deactivate and new page's vtable+60/activate — neither touches
byte 0xD). Therefore the previous session's central hypothesis — "Update never
fires because byte 0xD is never set to 1" — is **FALSE**. byte 0xD is 1.
The single call to Show (`sub_825D15F0`) via `sub_825CC478` is for an
unrelated class; not relevant here. Do NOT add a Show/byte-0xD hook — it would
be a no-op.

Corollary: `Draw` (vtable+32) is gated on the same byte 0xD, so if the screen
draws at all, byte 0xD is 1, which is consistent with the object being visible.

### So the real second bug is NOT the visibility gate. Open candidates:
- (a) Update **does** fire now (post spin-fix) and the remaining problem is
  input routing / focus, not the per-frame tick. (The user's "renders but
  doesn't respond to input" wording is consistent with this.)
- (b) The Leaderboards object is not a child of the container whose
  `sub_825D1618` dispatcher actually runs, so its Update is never reached even
  though it's visible. (`sub_825C05F8` is a nearby container-Update that calls
  `sub_825D1618` — candidate parent, unconfirmed.)

These are distinguishable with one datum: does `sub_825C0A88` (Update) fire?

### Diagnostic probe added this session (temporary, read-only)

`LeaderboardUpdateProbe` — a void midasm hook at `0x825C0A88` (Update entry),
`registers=["r3"]`, logs (rate-limited to 8 hits) the object pointer plus
`byte[obj+0xC]` (activated) and `byte[obj+0xD]` (visible) via `REXKRNL_INFO`
tagged `[LBPROBE]`. Builds clean; confirmed emitted at the top of
`DEFINE_REX_FUNC(sub_825C0A88)` in `generated/nocturnerecomp_recomp.28.cpp`.
Both existing guard hooks left untouched.

**Awaiting one user run** (`python scripts/run.py` → Main Menu → Leaderboards):
report (1) whether any `[LBPROBE]` lines appear in the log, and (2) whether the
leaderboard screen's own elements (tab arrows, the pulsing highlight) animate
or are frozen. If `[LBPROBE]` fires → Update runs → chase input/focus routing
next. If it never fires → wrong/again-inert dispatcher → find the real parent
container that should tick this object (theory b).

## UPDATE (2026-07-14, session 8): ROOT CAUSE FOUND — missing XN_SYS_UI "open" notification from SDK's XamShowSigninUI; page 8 (Live-logon interstitial), not the Leaderboards screen, is what's stuck

Sessions 1–7 chased the wrong screen entirely. Full dispatch mechanism and the
real culprit, with every address needed to re-verify:

### The FE page-dispatch mechanism (now fully mapped)

- **`sub_825D0090`** — FE event pump. Pops events from a global queue at
  **`0x82E7A368`** (posted via **`sub_825CE8E8(type, a, b, c)`**). Event type
  **8** = "switch page": calls `sub_825CFF90(container, page=v[2], a3=v[1],
  a4=v[3])`. All other events go to the current page's vtable+4.
- **`sub_825CFF90(container, N, a3, a4)`** — page switch: deactivates current
  page (vtable+12), sets `container+4 = N`, activates new page (vtable+60 with
  a3, a4). Page slot = `container + 4*(N+3)` (i.e. +12 onward). Page count at
  `container+8` (= 22).
- **`sub_825AAE90`** — FE per-frame tick (vtable sibling of `sub_825ABED0` on
  the controller from `sub_825ABE50`). Calls the pump, then calls the
  *current* page's Update (vtable+28) every frame:
  `v12 = *(4*(*(a1-144) - 34) + a1); (*vt(v12)+28)(v12, 1)`.
  (`a1-148` = container, so `a1-144` = current-page-index field.)
- The 22-page array (constructed in `sub_825ABED0` one-time init; index →
  ctor): 0=`sub_825C6320`, 1=`sub_825C5E20` (main menu), 2=`sub_825C4F50`,
  **3=`sub_825C2BD0` (Leaderboards, vtable `0x82015498`)**, 4=`sub_825C0590`,
  5=`sub_825CA128`, 6=`sub_825BA620`, 7=`sub_825B82A8`,
  **8=`sub_825B69A0` (Live-logon interstitial, vtable `0x82013ec0`)**,
  9=`sub_825B67F8`, 10=`sub_825B5CF8`, 11=`sub_825B4E20`, 12=`sub_825BFE10`,
  13=`sub_825BEBE8`, 14=`sub_825BE2C8`, 15=`sub_825B7418`, 16=`sub_825BDCB0`,
  17=`sub_825BB608`, 18=`sub_825B3C90`, **19=`sub_825C2BD0` (2nd Leaderboards
  instance)**, 20=`sub_825B2C28`, 21=`sub_825B0C68`.
- Main menu page 1 internals: its OnCreate `sub_825C5400` builds the 6-item
  tab list via `sub_825D3A58` (menu-list class, vtable `0x82016F60`) stored at
  `page1+548`; items added by `sub_825D3DB8(list, idx, string_id)`.

### Live-verified state during the softlock (guest memory, live process)

- Guest memory host base: **`0x110000000`** (guest addr + base = host addr).
  NOTE: heap objects at guest `0x30xxxxxx` are *stored as pointers* using the
  64K-page alias **`0x40xxxxxx`** (alias = +0x10000000) — memory scans for
  object references must search both forms.
- FE container (the `sub_825ABE50` object, vtable `0x82012698`): guest
  **`0x300fdca0`**. `+4` current page = **8**. `+8` count = 22.
  Page array at +12: `[3]=0x402d6740`, `[8]=0x402d8500`, `[19]=0x402d97f0`.
- Both Leaderboards instances (`0x302d6740`, `0x302d97f0`): OnCreate ran
  (`+660==5`), `visible(+13)=1`, `activated(+12)=0` — never activated. Their
  Update `sub_825C0A88` correctly never runs because **they are not the
  current page — page 8 is.**
- Page 8 object guest `0x302d8500`: `activated=1`, mode `+544=1`, `+548=0`,
  **flag `+552=0` (stuck)**.

### Page 8 = "logging on to Xbox Live" interstitial (the actual stuck screen)

Vtable `0x82013ec0`: +28 Update=`sub_825B6AB0`, +52 OnCreate=`sub_825B6868`,
+60 activate=`sub_825B69F8`.

- Activate (`sub_825B69F8`): stores mode/target, calls
  `sub_82584540(&dword_82E60B28, v6)` = **`XamShowSigninUI(1, flags)`**, logs
  "manual logon: ...", clears `+552`.
- Update (`sub_825B6AB0`): `if (netmgr->vt+112()) +552 = 1;` — until then it
  only animates a spinner and returns. Once latched it checks logon state and
  posts event 8 to move on (to page 3/Leaderboards on success, or back to
  page 1/main menu on failure) — **both outcomes are graceful; only the
  never-latching case softlocks.**
- Network manager: global object ptr at **`dword_82E4F808`** (live obj
  `0x4002f7a0`, vtable `0x820099c8`). vt+112 = `0x82577B28` =
  `return byte[this+0x9B0]` — "system UI currently open".
- `byte+0x9B0` is written ONLY by the XNotify pump **`sub_825774A8`**
  (case id **9 = XN_SYS_UI**: param 1 → set, param 0 → clear; id 0xA =
  XN_SYS_SIGNINCHANGED handled separately). The pump drains ALL queued
  notifications every frame, so an "open" and "close" broadcast queued
  together are both applied within one frame and the open state is never
  observable by page 8's Update.

### The bug and the fix (in `../rexglue-sdk`, not this repo)

`rexglue-sdk/src/kernel/xam/xam_user.cpp` `XamShowSigninUI_entry` broadcast
only `XN_SYS_SIGNINCHANGED(1)` + `XN_SYS_UI(0)` — the **"UI opened"
(`XN_SYS_UI`, id 9, param 1) edge was never sent**, so `byte+0x9B0` never
became 1 and page 8 waited forever. Renderer-independent — explains identical
softlock on xenos Vulkan, xenos D3D12, and the native renderer.

Fix applied to the SDK: broadcast `XN_SYS_UI(1)` immediately, then via
`rex::thread::QueueTimerOnce` (+150ms, so it lands in a *different* guest
notification drain) broadcast `XN_SYS_SIGNINCHANGED(1)` then `XN_SYS_UI(0)`.
This mirrors xenia-canary's `xeXamDispatchDialog` pre/post pattern
(`xenia-canary/src/xenia/kernel/xam/xam_ui.cc`, which sleeps 100ms before the
UI-off broadcast for exactly this reason).

Note: earlier sessions' `[LBPROBE]`-never-fires result is fully explained —
`sub_825C0A88` was the wrong screen's Update. The two guard hooks
(`LeaderboardLoadSigninGuard`, `LeaderboardMainMenuSpinGuard`) remain and are
still plausibly needed for the *post-logon* path (pages 3/19), since the
signin spin at `0x825C1B24` is real code that runs when page 3 activates.

## Live debugging technique notes (for reuse)

- The running exe has no matching PDB (host x64 symbols are stripped in
  Release builds); guest PC cannot be read directly from host stack frames.
- Key trick: `PPCContext` layout (`rex/ppc/context.h`) has `r3` at offset 0
  and `lr` at offset `0x100` (32 registers × 8 bytes). Inside a guest
  function's hot loop, `rdi` (or similar callee-saved register, verify per
  function) commonly holds `&ctx` for the whole function body — confirmed
  by reading `[rdi+0x100]` and checking the result falls in the guest
  `.text` range (`0x82230000`-`0x828799CC` per the XEX's function table log
  line). This lets you recover the actual guest link register (last `bl`
  target's return address) from a live, symbol-less host process without
  a matching PDB.
- lldb thread indices are **not stable** across separate `lldb -p <pid>`
  invocations (they get renumbered each attach) — always re-resolve the
  thread of interest by name (e.g. `thread list | grep "Main XThread"`)
  within the same attach session before reading registers/backtraces.

## UPDATE (2026-07-10, session 7): `[LBPROBE]` never fires — theory (a) ruled out; deep dive into the missing dispatch path; **paused by user request, unresolved**

The user ran the build with `LeaderboardUpdateProbe` in place, reproduced the
hang, and checked the freshest log: **zero `[LBPROBE]` lines.** This settles
the session-6 fork definitively: `sub_825C0A88` (Update) genuinely never runs,
even once, after the spin-fix. Theory (a) ("Update fires, it's an input/focus
bug") is ruled out. Theory (b) ("wrong/absent dispatcher") stands. The rest of
this session chased (b) and did not land a fix — read this before continuing.

### Confirmed: `sub_825CFF90` (the page-switch function used throughout this
whole investigation) is not the per-submenu dispatcher at all

Grepped every generated `.cpp` under `generated/` for literal calls to
`sub_825CFF90(ctx, base)` (safe, no IDA/lldb needed — the user correctly
suggested reading the generated recompiled source directly instead of
re-deriving everything through IDA). **There are exactly 3 occurrences in the
entire recompiled binary**, and only 2 are real call sites (the 3rd resolved
to be inside `sub_825ABED0` itself, a duplicate hit from grep context) — both
are inside `sub_825ABED0` ("FE Init"):
- Guest `0x825ABF08`: `sub_825CFF90(a1-148, 1, 0, -1)` — called every frame
  once `dword_82E7A570` (an unrelated persistent pointer, see correction
  below) is truthy.
- Guest `0x825ACBB0`: `sub_825CFF90(a1-148, 0, 0, -1)` — called once, on the
  very first invocation (the one-time init branch).

So `sub_825CFF90` only ever switches between **page 0** (presumably a
one-time boot/logo state) and **page 1** (the persistent main-menu
front-end) of a 2 (or more, but only 0/1 ever used) -slot container living at
`a1-148`. It is **not** used anywhere to switch between main-menu *items*
(Load/Save/Options/Leaderboards/etc.) — that original assumption, threaded
through several earlier sessions of this doc, was wrong. Whatever wires up
main-menu item pages (including Leaderboards) is a **different, still
unidentified** mechanism.

### Traced page 1's own per-frame Update — leads to an unresolved nested container

Page 1's object is `sub_825C5E20()`'s return value (vtable base
`0x82015960`). Read that vtable's own slots via IDA `get_int` (safe, static
data read): **`+28` (Update) = `sub_825C5A08`.** Read it from
`generated/nocturnerecomp_recomp.28.cpp:57394` directly (no decompiler
needed). It:
- Handles d-pad left/right navigation between exactly **6** main-menu tabs
  and updates highlighted tab-label text (`sub_825CFC68` with different
  string-table constants per case 0..5) via a `switch` on
  `*(*(r30+548)+536)` (a "current tab index" field read through a **second,
  nested object** at `r30+548`).
- Also calls, on that *same* nested object at `r30+548`: its own
  **vtable+56** (if some counter `< 2`) or **vtable+60** (otherwise) —
  i.e. this nested object has its own polymorphic Activate/Update-ish pair,
  separate from page 1's own vtable and separate from `sub_825CFF90`.

This nested object (`r30+548`) — call it the **tab container** — is the
strongest remaining candidate for "the thing that should tick Leaderboards
but doesn't." Its own vtable+56/+60 calls are the most likely place where a
per-tab OnCreate/Update actually gets wired up, or where the wiring is
missing for Leaderboards specifically (e.g. if the tab container keeps its
own small array of 6 child pointers, one per tab, and Leaderboards' slot is
never populated, or is populated but this dispatch path doesn't reach
`sub_825D1618`-style child iteration the way other UI containers do).

**Not yet found:** where the tab container's class vtable is defined, what its
+56/+60 implementations actually do, and — critically — where/how
`page1_obj + 548` gets its value in the first place (i.e. the tab container's
construction site). A blind grep for `u32 + 548,` `REX_STORE` sites across
`generated/` returned 70+ hits — offset 548 is heavily reused as a generic
field/stack-frame offset by dozens of unrelated functions/classes throughout
the binary, so it does not converge without narrowing by which *object*
(class/vtable) each hit belongs to. This is the concrete next step for
whoever picks this back up: for each `+548` store site, check what class
constructor it's in (cross-reference against `sub_825D1CD8`/`sub_825C2BD0`-
style ctor chains, the same way page 1's and Leaderboards' own vtables were
identified this session and session 6), to find the one that's actually
initializing page 1's own object layout.

### Correction: `dword_82E7A570` is NOT the same object as `sub_825ABED0`'s
`a1` parameter — a wrong turn this session, documented so it isn't repeated

Early this session a live-inspection script (`scripts/peek_leaderboard_state.py`,
still in the tree, **read-only, no breakpoints, safe to reuse** — but its
premise needs fixing first, see below) was written assuming
`dword_82E7A570 == a1` (the parameter `sub_825ABED0` receives from its own
caller every frame). This is **wrong**: `dword_82E7A570` is a *different*,
persistent pointer — assigned once via `dword_82E7A570 = sub_82576950(16388)
? sub_825CE970() : 0;` inside `sub_825ABED0`'s one-time-init branch — that
happens to also double as the function's "already initialized" guard
(`if (dword_82E7A570) return sub_825CFF90(a1-148, 1, 0, -1);`), but its
*value* is some other resource/manager object, not `a1` itself. Running
`peek_leaderboard_state.py` against it produced `page1 read failed` (a NULL
read at `container+16`) because the arithmetic was walking the wrong
object's memory entirely.

The real `a1` is whatever object is passed by `sub_825ABED0`'s own caller.
Xrefs to `sub_825ABED0` resolve to a data slot `0x82012628` (`off_82012628`),
which `sub_825ABE50` (some other class's constructor, unrelated-looking:
sets up an object with vtables `off_82012698`/`off_820125E0`) stores into
its own `a1[37]` (byte offset 148) — i.e. `sub_825ABED0` is installed as a
**virtual slot (offset 148) on some other controller class**, and `a1` at
runtime is *that* controller instance, not anything found so far. Tracing
*that* object's construction/identity is the actual next step for finding
`sub_825ABED0`'s live `a1` value, if pursuing this further with live
inspection.

### Crash root-cause, finally identified: **do not wrap `lldb -p <pid> -s <script>` in an outer shell `timeout`**

Every game-process crash across sessions 6 and 7 (four total) traces to the
same mechanism, only understood at the very end of this session:
`Bash("timeout 20 lldb -p <pid> -s <file>")` — if the breakpoint in `<file>`
doesn't hit before the outer `timeout` fires, `timeout` sends the lldb
process a kill signal. **On Windows, killing a debugger process by default
also terminates the process(es) it's attached to** (`DebugActiveProcessStop`
is never called, so the OS falls back to its "kill on debugger exit"
behavior). This is *not* related to the earlier-diagnosed 32-bit/`unsigned
long` truncation bug in an `expr` command (session 6) — that was a real,
separate bug (fixed: always use `unsigned long long`/`uint64_t` for 64-bit
host pointer arithmetic in lldb `expr`), but it was the outer-`timeout`
mechanism that actually killed the process each time, including in a
breakpoint that used no `expr` at all (this session's `sub_825ABED0` attempt,
which never even hit before the wrapper timeout fired).

**If resuming live debugging**: never wrap the lldb invocation in a
force-killing timeout while a breakpoint is armed and `continue` is pending.
Either omit the outer timeout and let the command file's own trailing
`detach`/`quit` end the session naturally (accepting that a truly-never-hit
breakpoint means the shell call blocks until manually interrupted), or find a
way to signal lldb to run its `detach` command specifically (e.g. a
sufficiently generous timeout that's virtually guaranteed to have hit the
breakpoint already, given `sub_825ABED0` should fire every rendered frame —
though notably it did *not* hit within ~15–20s in this session's one attempt,
which is itself a data point worth re-examining: either the host address
resolution was stale/wrong for that relaunch, or — more interestingly —
`sub_825ABED0` genuinely isn't being called every frame the way the
decompiled source implies, which would itself be a major clue and is worth
re-testing carefully before assuming it was just a wrapper-timeout artifact).

### Current state of the tree (end of session 7)

- `LeaderboardLoadSigninGuard` (guards `sub_825C0848` at entry) — kept,
  harmless, confirmed correct-but-insufficient.
- `LeaderboardMainMenuSpinGuard` (guards the busy-spin in `sub_825C1AB8` at
  `0x825C1B24`) — kept, **confirmed to genuinely fix the busy-spin** (the
  game no longer CPU-deadlocks), but **the softlock persists** as a UI
  dispatch bug: the Leaderboards screen still never becomes interactive
  because its Update never gets called (confirmed via probe, zero hits).
- `LeaderboardUpdateProbe` (temporary diagnostic at `sub_825C0A88` entry) —
  kept per its own doc comment ("remove once the second bug is found") since
  the second bug is *not* yet found; still useful for the next session to
  re-confirm behavior after any fix attempt.
- `scripts/peek_leaderboard_state.py` — safe (read-only, no breakpoints) but
  its object-graph premise (`a1 == dword_82E7A570`) is **wrong**; needs the
  real `a1` (see "Correction" section above) before it's useful again.
- `scripts/resolve_gpu_wait.py`, `scripts/trace_leaderboard_entry.py` — still
  valid/reusable as documented in earlier sessions.

**This investigation was paused here at the user's explicit request** ("stop,
document what you learned") after four game-process crashes across two
sessions, all now understood to stem from the outer-`timeout`-kills-debuggee
mechanism above (not from anything wrong with the fixes themselves). The
softlock is **not yet fully fixed** — Main Menu → Leaderboards will still
hang from the user's perspective (screen frozen, background still animating)
even with every hook currently in the tree applied. The next concrete steps,
in priority order, are:
1. Find the real `a1` for `sub_825ABED0` (trace `sub_825ABE50`'s caller/owner
   class) — confirms whether the FE tick is even running normally, and gives
   a real object to walk from.
2. Find where `page1_obj+548` (the "tab container") gets constructed/assigned,
   by cross-referencing `+548` store sites against known ctor-chain patterns
   rather than blind grep.
3. Decompile/read the tab container's own vtable+56/+60 (its Activate/Update
   pair) to find where per-tab pages should get OnCreate'd/ticked, and why
   Leaderboards' slot (if any) isn't reached.
4. Re-run with `LeaderboardUpdateProbe` after any candidate fix to confirm
   `[LBPROBE]` actually starts firing before declaring victory — this
   session's experience shows confident-sounding static theories (byte-0xD
   visibility, in session 6) can be wrong, and the probe is the one piece of
   ground truth available without live debugging.
