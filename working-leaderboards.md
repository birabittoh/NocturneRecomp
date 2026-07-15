# Leaderboards backend — working notes / handoff

## Goal

Replace the stubbed XAM stats API so the Leaderboards screen actually shows
data instead of "An error occurred while loading this Leaderboard," and so
that a "Records" submenu inside that screen doesn't crash. Backend is a
local TOML file (`LeaderboardManager`, `rexglue-sdk/include/rex/system/leaderboard_manager.h`
+ `.cpp`), keyed by `(title_id, view_id)`. This is **not** the original
plan's design in all respects — several of its assumptions turned out wrong
after live debugging. Read this whole doc before continuing; don't restart
from `LEADERBOARD_BACKEND_PLAN.md`'s original assumptions, several are
superseded below.

## Current status: WORKING (Overall + My Score); Friends still TODO

Resolved. The "Overall"/Records tab and the "My Score" tab both load and show
real scores/times. Fixes (all in the SDK, committed there):

1. **Buffer layout.** The enumerate buffer is one `XUSER_STATS_RESULTS`
   (`dwNumViews@0, pViews@4`) followed by the `XUSER_STATS_VIEW`(s), rows and
   columns — confirmed by the real xam.xex sizing `(48*rows+16)*specs + 8` and
   by the consumer `sub_82584B88`, which reads `*(*(buffer+4)+8)` =
   `pViews->NumRows`. The prior "pRows at view+4" change was a MISREAD: the
   pointer being dereferenced is `pViews` (RESULTS+4), and view+8 is a count.
   The original Xenia view order (`ViewId, TotalViewRows, NumRows, pRows`) was
   correct. Also fixed the `XUSER_STATS_ROW` sub-field offsets to the real
   8-aligned XDK layout (`xuid@0, dwRank@8, i64Rating@16, szGamertag@24,
   dwNumColumns@40, pColumns@44`).

2. **Score/time value.** The game displays each row's score/time from
   `i64Rating` (row+16), read straight from the row header by `sub_825C1E98`
   — NOT from the columns array. `LeaderboardManager::GetRows` now fills
   `LeaderboardRow::rating` from the ranking column's value and
   `WriteStatsView` writes it at row+16. (Time boards format row+16/3600000 as
   HH:MM:SS.)

3. **Param order.** The wrappers `sub_825D77C0/7820/7880` insert a literal
   `scope` as arg2, so the real prototype is
   `(a1, scope, user_index+1, num_rows, num_specs, specs, pcbBuffer, phEnum)`.
   Our old labels were shifted: real `num_specs` is arg5 (reliably 1), and
   arg3 is `user_index+1` (or a XUID = uninitialised `0xBABEBABE` on the My
   Score/Friends paths). The old code rejected the My Score call because it
   validated arg3 as num_specs. Now num_specs comes from arg5.

**Friends** tab: still shows the generic error. It uses Path A — a serialized
`XMsgInProcessCall(0xFC, 0x58020)` (packed by `sub_82813718`, driven by
`sub_82584928`/`sub_82813760`) — and fundamentally lists Xbox Live friends'
scores, which don't exist without a Live friends system. Left unimplemented.

Note: `GetRows` still ignores `scope`, so every tab shows the same global list
(a scope-tagged-gamertag diagnostic confirmed the tabs are distinct calls). To
make My Score show only the signed-in user, seed a row whose xuid matches the
real user XUID and filter by scope.

---

## "You are not signed in to Xbox Live" gate — ROOT CAUSE + FIX

The pause-menu **Leaderboards** option showed "You are not signed in to Xbox
Live." Root cause, confirmed in `default.xex`:

- The leaderboard screen's action handler `sub_825C2CB0` opens with
  `if (user_index == -1 || XamUserGetSigninState(user_index) ==
  eXamUserSigninState_SignedInToLive) { ...leaderboard... } else { show
  "not signed in" error; return 0; }`. `sub_825C5880` has the same
  `== eXamUserSigninState_SignedInToLive` (value **2**) check.
- The SDK's `UserProfile::signin_state()` (`rexglue-sdk/include/rex/system/xam/
  user_profile.h`) hardcoded `return 1` (signed in **locally**), so the Live
  branch never ran and the error always fired.
CONFIRMED WORKING (2026-07-15): the pause-menu Leaderboards screen now opens.
Three cooperating checks all had to pass — diagnosed by logging every
`XamUser*` gate call and watching which one failed at screen-open time:

- **Part 1 — `signin_state()` 1→2.** The screen (`sub_825C2CB0` /
  `sub_825C5880`) gates on `XamUserGetSigninState() == SignedInToLive` (2).
  Necessary but not sufficient on its own.
- **Part 2 — seed `XN_SYS_SIGNINCHANGED` at listener creation.** The netmgr
  keeps its own per-user signed-in bitmask at `netmgr+0x9AC` (`dword_82E4F808`,
  vtable `0x820099c8`; `sub_82577AF0` = vtable+108 reads it), rebuilt from
  `XamUserGetSigninState` only when its pump `sub_825774A8` processes an
  `XN_SYS_SIGNINCHANGED` (id `0x0A`; handler vtable+196 `sub_825778C8` also
  sets the Live bitmask at `netmgr+0x9A8`). Nothing broadcast it at boot, so
  `xeXamNotifyCreateListener` now seeds each new listener with it (mirrors real
  hardware; mask check drops it for non-subscribers).
- **Part 3 — `XamUserCheckPrivilege` grant, not deny (the actual blocker).**
  The `[LBDIAG]` log showed the screen calling `XamUserCheckPrivilege(0,
  mask=0xF9)` every frame and the SDK returning `out_value=0` (denied). The
  screen requires that presence/online privilege granted; denying it produced
  the error. `XamUserCheckPrivilege_entry` now returns `out_value=1`.

All three committed in the SDK (`native` branch). The score-*write* path
(below) is still open — but this same gate very likely blocked it too, so it's
worth re-checking now that the user reads as fully signed in to Live.

## WRITE path (score submission) — RE findings (this session)

The read path works via XLiveBase app 0xFC, msg `0x00058020`
(`CreateStatsEnumeration`, driver `sub_82813760`). The **write** path is a
sibling on the *same* app 0xFC, but it took RE to establish that:

1. **The SDK's `XSessionWriteStats` (XGI msg `0x000B0025`) handler is dead code
   for this title.** Enumerating every `XMsgInProcessCall`/`XMsgStartIORequest`
   call site in `default.xex`, the game never sends XGI `0x000B0025` (nor
   `0xB0021`). Only XGI msg observed is `0xB0019` (`sub_82812D48`). So the
   already-implemented `SubmitRow`/`Save()` write-through in
   `xgi_app.cpp` + `leaderboard_manager.cpp` is correct but **never invoked** —
   that's why nothing hits disk.

2. **Scores are written via an *async* XLiveBase RPC**, not a synchronous
   in-process call. The write family lives in `default.xex` at
   `sub_828139F0`, `sub_82814230`, `sub_82814498`, `sub_82814778`,
   `sub_82814A08`, `sub_82814CA0`, `sub_82814EE0`. Each ends with:
   ```c
   XMsgStartIORequest((HXAMAPP)0xFC, (unsigned __int16)descriptor | 0x50000,
                      overlapped, packed_request, 0x28);
   ```
   Payload is serialized by `sub_82813718`/`sub_82813598` (not a flat struct).
   `sub_82814EE0` (RPC descriptor 1540) reads `XamUserGetXUID` + machine id +
   title id + a 16-byte blob — a logon/arbitration-style write; the score-stat
   write is one of the others in the cluster.

3. **Why there's no constant to grep for.** The message number is computed at
   runtime: `sub_82813388(rpc_id)` binary-searches a table (`dword_82E7C4FC`,
   `word_82E7C4F8` entries × 4 bytes, layout `{u16 rpc_id, u16 msg_num}`) and
   returns `msg_num`; the sent message is `msg_num | 0x50000`. The table
   pointer is populated at load/init, so it isn't statically resolvable from
   the IDB. (Validation: read path rpc_id 518 → msg `0x8020` → `0x58020`.)
   None of the write wrappers have direct code xrefs (they're XLive library
   API entrypoints resolved indirectly), so static caller tracing is a dead
   end too.

4. **Async routing IS wired in the SDK.** `XMsgStartIORequest` →
   `xeXMsgStartIORequestEx` → `AppManager::DispatchMessageAsync` →
   `App::DispatchMessageSync` (async falls straight through to the sync
   handler; see `app_manager.cpp:41`). So the write RPC **does** reach
   `XLiveBaseApp::DispatchMessageSync`, falls through to the "Unimplemented
   XLIVEBASE message" branch, returns `X_E_FAIL`, and the score is dropped.

### Concrete next step (how to finish the write path)

The exact write message number + payload layout is best captured live rather
than guessed. `xlivebase_app.cpp`'s fall-through now hex-dumps the first 128
bytes of the request buffer for any unhandled 0xFC message. So:

1. Rebuild SDK + game, run, sign in, finish a run that submits a score
   (e.g. a boss-rush/time-attack completion or beating a leaderboard entry).
2. Grep logs for `Unimplemented XLIVEBASE message` around the save — the
   `msg=000580xx` on the save is the write message; the hex dump is the
   serialized request. Correlate the dump against `sub_82813718`'s field order
   (XUID, view/score fields) to decode it.
3. Add a `case 0x000580xx:` to `XLiveBaseApp::DispatchMessageSync` that parses
   the payload and calls `kernel_state_->leaderboards().SubmitRow(title_id,
   view_id, xuid, gamertag, columns)` — reuse the existing
   `LeaderboardManager` (already write-through to
   `Documents/nocturnerecomp/leaderboards/{title_id:08X}.toml`). Use the XUID
   from the payload and the gamertag from `XamUserGetName`/the profile.
4. Complete the overlapped with success and return `X_E_SUCCESS` so the game
   doesn't treat the save as failed.

Open question: whether the game gates the write on a valid signed-in
user/username first (the repeated "reads the username" behaviour). `XamUserGetName`
(user 0) already returns the profile name, so the username itself is available;
confirm from the live log that the save RPC actually fires (vs. the game
aborting before sending it).

---

_Original handoff notes (pre-fix) below, kept for the reverse-engineering
breadcrumbs._

## Key confirmed facts (hard evidence, not guesses)

1. **Two completely separate call paths exist for "stats enumeration" in
   this game**, and it took several iterations to figure out which one the
   crashing screen actually uses:
   - **Path A**: `XMsgInProcessCall(app=0xFC, msg=0x00058020)`, driven by
     `sub_82584928` (default.xex) → `sub_82813760` → the message. This is
     what `LEADERBOARD_BACKEND_PLAN.md` originally described. **This path
     never fires for the Records submenu in practice** — the only calls to
     msg 0x058020 observed at runtime have garbage/zeroed arguments
     (`view=518, flags=2196227280, rows=0, handleOut=0`) that don't match
     `sub_82584928`'s own decompiled behavior (which always passes literal
     `rows=100` and a real stack address). This is almost certainly an
     **unrelated feature** reusing the same message ID (possibly a real
     friends-list poll) — a red herring. Implemented anyway in
     `rexglue-sdk/src/kernel/xam/apps/xlivebase_app.cpp` case `0x00058020`,
     but it is not what needs fixing for this crash.
   - **Path B**: The real kernel export `XamUserCreateStatsEnumerator`
     (ordinal `0x2F7`, was `REX_EXPORT_STUB` before this session), called
     via `sub_825D77C0`/`sub_825D7820`/`sub_825D7880` (thin wrappers) from
     `sub_82585E38` (default.xex). **This is the path the Records submenu
     actually uses.** Implemented in
     `rexglue-sdk/src/kernel/xam/xam_user.cpp` as
     `XamUserCreateStatsEnumerator_entry`.

2. **Path B's real parameter order**, reverse-engineered from
   `sub_82585E38`'s decompile (see "How to re-derive" below for the IDA
   commands):
   ```
   XamUserCreateStatsEnumerator(
     a1 = 0,                      // literal 0 in this call site
     a2 = 1,                      // literal "scope" from the wrapper (1/0/3 across the 3 wrapper variants)
     a3 = user_index + 1,
     a4 = row_count (capped to 99 if >100),
     a5 = 1,                      // literal constant — this is NumStatsSpecs, always 1 here
     a6 = specs_ptr,              // -> {ViewId: u32, NumColumnIds: u32} in THIS call (no column array; a separate branch handles a populated column buffer)
     a7 = buffer_size_ptr,        // OUT: required byte size for the enumerate buffer
     a8 = handle_ptr,             // OUT: real xam enumerator handle
   )
   ```
   Our current `XamUserCreateStatsEnumerator_entry(title_id, user_index, num_specs, num_rows, size, specs_ptr, buffer_size_ptr, handle_ptr)`
   labels positions 3/4/5 as `(num_specs, num_rows, size)` — **these labels
   are wrong** (should be `(user_index+1, row_count, NumStatsSpecs-constant-1)`)
   but by coincidence work correctly for the one call site observed
   (`user_index=0` so position3 happens to equal 1 = the same value
   `num_specs` would need to be; the real `NumStatsSpecs` constant is
   *also* always 1). **This coincidence may not hold for other call sites**
   (e.g. a different board, or a different user_index) — worth relabeling
   properly before trusting this further.
   Positions 1, 2, 6, 7, 8 are confirmed correct.

3. **Confirmed real struct sizes** (derived from `xam.xex`'s own
   buffer-sizing arithmetic in the real `XamUserCreateStatsEnumerator` at
   `0x818c0150` in the `1888PatchedDash` dashboard dump — see
   `C:\Users\m.andronaco\Downloads\1888PatchedDash\xam.xex`):
   - `XUSER_STATS_RESULTS` header (only used by the direct
     `XUserReadStats` path in `xgi_app.cpp`, NOT the enumerator path): 8
     bytes (`dwNumViews`, `pViews`).
   - `XUSER_STATS_VIEW`: 16 bytes.
   - `XUSER_STATS_ROW`: 48 bytes.
   - `XUSER_STATS_COLUMN`: 28 bytes.
   These sizes are believed solid (derived from arithmetic, not guessed).

4. **`XUSER_STATS_VIEW`'s field *order* was wrong and partially fixed this
   session.** The real consumer, `sub_82584B88` (default.xex), does:
   ```c
   int sub_82584B88(int a1) {
     if (*(a1+148) <= 2 && (v1 = *(a1+184)) && *v1)
       return *(v1[1] + 8);   // <-- dereferences offset+4 AS A POINTER
     return 0;
   }
   ```
   `*(a1+184)` is the enumerate-result buffer (allocated by
   `sub_82585E38` based on our `buffer_size_ptr` output, then filled via
   `XamEnumerate`/our `XStatsEnumerator::WriteItems`). `v1[1]` is
   `*(buffer+4)` (since `v1` is treated as `DWORD*`). The crash
   (`Access violation reading location 0x1xxxxxxxx`, low offset like 8 or
   11 past guest membase) happens because **offset+4 must be a pointer**
   (presumably `pRows`), but our original layout put a row-count there
   instead (Xenia's guessed struct: `ViewId, TotalViewRows, NumRows,
   pRows` — pointer *last*). **This was fixed** this session — see
   `leaderboard_stats.cpp`, `WriteStatsView()` — new order is `ViewId(0),
   pRows(4), TotalViewRows(8), NumRows(12)`.
   **This fix did NOT stop the crash.** Rebuilt, redeployed, retested,
   same crash. So either:
   - There's a second field/offset mismatch elsewhere in the row or
     column structs that `sub_82584B88`'s caller chain also reads before
     or after this point, or
   - `sub_82584B88` itself isn't even the final crash site in the fixed
     build (**re-check the lldb backtrace after this fix** — it may have
     moved. The last captured backtrace when this doc was written was
     from *before* the pRows-offset fix's rebuild had been tested against
     lldb; confirm with a fresh capture whether it's still exactly
     `sub_82584B88`).

5. **Row data does flow through correctly now.** Seeded a test TOML store
   at `C:\Users\m.andronaco\Documents\nocturnerecomp\leaderboards\58410847.toml`
   (real `user_data_root` is `Documents\nocturnerecomp`, NOT the `assets`
   dir — the code comment referencing title `4D530910` was a red herring
   from unrelated context; the real title_id is `0x58410847`, confirmed
   via a live `REXSYS_INFO` log line). Debug logs confirmed
   `spec: view_id=1, num_columns=0, rank_column_id=0` and
   `rows.size()=3` — the seeded rows are being found and handed to
   `WriteStatsView`. So the crash is a pure struct-layout/consumer-contract
   bug, not a data-availability bug.

6. **`num_columns=0`** in the observed spec is *expected*, not a bug: this
   particular call site (`sub_82585E38`'s no-column-buffer branch) passes
   a spec that's just `{ViewId, NumColumnIds=0}` (8 bytes, not the full
   136-byte `XUSER_STATS_SPEC` with a column-ID array) — a different
   branch of `sub_82585E38` handles the case where a column buffer *is*
   provided (`HIDWORD(v6)` nonzero), calling `sub_825D7820` instead of
   `sub_825D77C0`. That branch hasn't been reverse-engineered — worth
   checking if the Records screen actually needs that branch for column
   values (currently it's requesting 0 columns, which may itself be
   fine for whatever `sub_82584B88` is trying to read, or may indicate
   we're being called from the wrong context entirely).

## What's implemented so far

- `rexglue-sdk/include/rex/system/leaderboard_manager.h` + `.cpp`: TOML-backed
  local score store, keyed by `(title_id, view_id)`. Wired into
  `KernelState` (`leaderboard_manager_` member, `leaderboards()` accessor,
  loaded/saved under `user_data_root/leaderboards/{title_id:08X}.toml` in
  `KernelState::SetExecutableModule`).
- `rexglue-sdk/include/rex/kernel/xam/apps/leaderboard_stats.h` + `.cpp`:
  shared guest-memory marshalling — `StatsSpec`, `ReadStatsSpecs`,
  `ComputeStatsViewSize`, `WriteStatsView`, and the `XStatsEnumerator`
  class (a one-shot `XEnumerator` subclass that writes the whole
  `XUSER_STATS_VIEW` blob on the first `XamEnumerate` call).
- `rexglue-sdk/src/kernel/xam/xam_user.cpp`:
  `XamUserCreateStatsEnumerator_entry`, replacing the old
  `REX_EXPORT_STUB`. Has defensive clamps (`num_specs`/`num_rows` > 0x14
  rejected) because a wrong param-order guess previously caused a
  3-billion-iteration OOB read crash — **keep these clamps**, they're
  cheap insurance against the same class of bug recurring.
- `rexglue-sdk/src/kernel/xam/apps/xlivebase_app.cpp` case `0x00058020`:
  implements "Path A" above. Probably not load-bearing for the crash
  we're chasing, but shouldn't be removed without checking it isn't used
  by something else (it does get called every run with garbage args, so
  something is exercising it — figure out what before ripping it out).
- `rexglue-sdk/src/kernel/xam/apps/xgi_app.cpp`: `XUserReadStats` (msg
  `0x000B0021`) and `XSessionWriteStats` (msg `0x000B0025`) implemented,
  reusing the same struct helpers. **Never actually exercised by the
  crash we're chasing** (that goes through the enumerator path, not
  direct read) — untested against the real game, but likely has the same
  view-header field-order bug (already fixed to match `WriteStatsView`'s
  new order) plus whatever other layout bugs remain undiscovered.
- `nocturnerecomp.toml`: added `log_level = "debug"` (needed to see
  `REXKRNL_DEBUG`/`REXSYS_INFO` diagnostic lines — default level filters
  them out silently, which cost real time to figure out this session).
- `assets/leaderboards/58410847.toml` and
  `C:\Users\m.andronaco\Documents\nocturnerecomp\leaderboards\58410847.toml`:
  test seed data (title_id 0 and 0x58410847 both covered, view_id 1-6,
  columns 0-7, 3 rows each) — **the second path (Documents) is the real
  one that gets loaded; the assets one is leftover from an earlier wrong
  guess and can be deleted.**

## How to reproduce / debug

1. Build: `python scripts/build.py` (from this repo) after
   `python scripts/deploy-sdk.py --project NocturneRecomp` (from
   `../rexglue-sdk`) if SDK-side files changed.
2. Launch: `python scripts/run.py`.
3. Find PID: `Get-Process nocturnerecomp | Select-Object Id` (PowerShell).
4. Attach lldb via the Python driver script at
   `scratch_lldb.py` (repo root) — it polls for a real crash (skips the
   initial attach breakpoint) and dumps all thread backtraces on fault:
   ```powershell
   $env:ATTACH_PID = "<pid>"
   lldb --batch -o "command script import scratch_lldb.py" *> lldb_result.txt
   ```
   Reproduce the crash in-game, then read `lldb_result.txt` (it's UTF-16,
   pipe through `iconv -f UTF-16LE -t UTF-8` before grepping).
5. Debug logs: `log_level = "debug"` is already set in
   `nocturnerecomp.toml`. **Important**: logs get split across multiple
   files per run (`logs/nocturnerecomp_NNN.log`, `.1.log`, `.2.log`,
   etc.) — grep *all* of them, the `[sys]`/`[krnl]` category lines don't
   all land in the same numbered file.

## How to re-derive the IDA findings above

Both binaries analyzed via the `ida-pro:idalib` MCP tool:
- `C:\Users\m.andronaco\dev\NocturneRecomp\assets\default.xex` (the actual
  game) — imagebase `0x82000000`.
- `C:\Users\m.andronaco\Downloads\1888PatchedDash\xam.xex` (a real Xbox
  360 dashboard dump — has the *actual* Microsoft implementation of
  `XamUserCreateStatsEnumerator` etc., not stubs) — imagebase
  `0x81870000`.

Useful addresses in `default.xex`:
- `sub_82584928` (0x82584928): Path A driver (msg 0x058020).
- `sub_82813760` (0x82813760): packs 5 dwords, sends msg 0x058020.
- `sub_82585E38` (0x82585E38): Path B driver — **the one that matters**.
- `sub_825D77C0` / `sub_825D7820` / `sub_825D7880`: thin wrappers around
  the real `XamUserCreateStatsEnumerator` import, differing only in a
  literal "scope" argument (1/0/3).
- `sub_82584B88` (0x82584B88): the crashing consumer function.
- `sub_82576950` (0x82576950): retry-allocator (loops calling
  `sub_825E1600`, an allocate-with-retry primitive) — NOT a handle
  wrapper, easy to misread.

Useful addresses in `xam.xex` (1888PatchedDash):
- `XamUserCreateStatsEnumerator` (0x818c0150): real implementation,
  gives the buffer-size arithmetic that confirms struct sizes.
- `XamCreateEnumeratorHandle` (0x818ea8f0), `XamEnumerate` (0x818ea9e8):
  confirm the generic enumerator-handle mechanism our
  `rex::system::XEnumerator` already mirrors reasonably well.

## Next steps (recommended)

1. **Get a fresh lldb backtrace** on the current (pRows-offset-fixed)
   build to confirm whether the crash is still literally
   `sub_82584B88`, or has moved deeper/elsewhere now that one bug is
   fixed. Don't assume — check.
2. If still `sub_82584B88`: the remaining mismatch is most likely in the
   **row struct** (48 bytes) or an offset *within* `v1[1]`'s target (i.e.
   whatever `pRows` points to) — re-derive by decompiling whatever calls
   `sub_82584B88` and reads its return value, to see what it expects
   offset+8-of-the-first-row to actually contain semantically (a rank? a
   column value? a gamertag pointer?).
3. Consider instrumenting more surgically: add a temporary
   `REXKRNL_DEBUG` dump of the raw bytes we write into the enumerate
   buffer (hex dump of e.g. the first 64 bytes) right before returning
   from `XStatsEnumerator::WriteItems`, so you can directly compare
   "what we wrote" against "what the crash address computation implies
   should have been there" without another IDA round-trip.
4. Re-examine whether **Path A (msg 0x058020) is truly unrelated** or
   whether it's actually a *second, legitimate* leaderboard-adjacent call
   (e.g. the main list screen uses Path A while Records uses Path B) —
   if so, Path A's `view=518/rows=0/handleOut=0` garbage might indicate
   *that* call path also needs fixing (the args look uninitialized/wrong
   in a way that suggests OUR marshalling of msg 0x058020's request
   buffer might be reading the wrong offsets, not that the call itself is
   unrelated). This was not fully ruled out — it was deprioritized once
   Path B was confirmed to be what Records uses, but the main list still
   shows the error screen and Path A might still matter for that.
5. Double check the `num_specs`/`user_index+1` mislabeling in
   `XamUserCreateStatsEnumerator_entry` (see point 2 above) — relabel
   correctly and re-verify the clamps still make sense once properly
   named, in case a different call site (different board, different
   user) exposes the coincidence and breaks.
