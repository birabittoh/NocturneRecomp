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

## Current status: still crashing

The "Records" submenu inside Leaderboards still segfaults on a null/garbage
pointer dereference. The main list screen still shows "error loading this
leaderboard." Multiple hypotheses have been tested and ruled out (see
"Timeline" below). The crash is now narrowed to a **very specific
byte offset** inside a struct we don't fully control the shape of, but
fixing the one confirmed bad field didn't fix it — meaning there is at
least one more layout mismatch we haven't found yet.

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
