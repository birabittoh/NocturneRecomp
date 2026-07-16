# Finding: force-exit the current front-end mode to the main menu

Discovered incidentally during the native-renderer fast-forward investigation
(see `docs/native-renderer-pacing-investigation.md`).

## What it is

The front-end framework object (the `a1` passed to `sub_82578A30`'s main-loop
chain — `sub_8258B8A0` / `sub_8258B3B8` / `sub_825AAE90`) has a **byte flag at
offset `+184`** that the mode/frame loop polls:

- `sub_8258B8A0`'s loop runs `do { ... } while (!*(a1 + 184));` and also
  `if (*(a1 + 184)) break;` in its inner sub-loops.

Setting `*(uint8_t*)(a1 + 184) = 1` from outside makes the current mode's loop
terminate and the framework fall back to the **main menu**. Observed live: while
sitting in the intro/attract sequence, forcing this byte to 1 immediately booted
to the main menu (it does **not** merely complete one frame — it exits the whole
current front-end mode).

`a1` at runtime: it's `r3` at the entry of `sub_8258B3B8` (guest `0x8258B3B8`)
and of `sub_825AAE90` (`0x825AAE90`) — both take the framework object as their
first argument. In a live probe it read as e.g. `0x4002F7A0`.

## Potential use: a "skip intros / attract" cvar

This is a clean, cheap way to jump straight to the main menu from the boot
logos/intro/attract loop. A `skip_intros` cvar could, on first reaching the
front-end, set `a1 + 184 = 1` once to bounce past the attract sequence to the
menu.

Caveats / TODO before using it as a real feature:
- Setting it *unconditionally / repeatedly* is destructive in-game — it will
  keep kicking the player out of whatever mode they're in back to the menu. Any
  cvar implementation must fire it **once**, gated to the intro/attract state
  only (identify that state first — e.g. via the current page index in
  `sub_825ABED0` / `sub_825CFF90`, or a mode id on the framework object).
- The exact semantics of `+184` beyond "exit current mode" weren't fully mapped
  (whether it's specifically "user requested back/exit" vs a more general
  teardown). Verify it lands on the main menu from *each* intro sub-state, not
  just the one tested.
- It was found by a diagnostic write from `FrameTickProbe`
  (`src/nocturnerecomp_hooks.cpp`); that probe is temporary and unrelated to
  this feature.

## Attempted implementation (reverted) -- +184 is the wrong lever for this

Tried a `skip_intros` cvar + midasm hook at `sub_8258B3B8`'s entry (r3 == a1,
runs once per fixed-timestep Update tick) that set `a1+184=1` whenever the FE
page id (`a1-144`) read 0, gated on `game_time` (`a1+2236`) > 0 so the mode got
at least one real tick before being cut short. Live-tested and reverted:

- **A one-shot write only skips the current logo, not the sequence.**
  `sub_8258B8A0`'s outer loop resets `a1+184`, `a1+2232`, and `a1+2236` to 0
  once at the top of *every* mode-loop iteration, and the intro is a series of
  separate iterations (one per logo) -- firing once during logo 1 gets wiped
  by logo 2's own reset before anything reads it.
- **Re-firing every tick causes an infinite loop, not a skip.** `a1+184`
  doesn't mean "advance to the next page" -- it means "tear down and fully
  reinit the current top-level mode object" (`sub_825AAE90`/`sub_825ABED0`
  chain). Reinit unconditionally sets the page id back to 0 as part of setup.
  So firing it repeatedly just repeats: enter mode -> run one Update tick
  (`game_time` 0->5) -> hook fires -> full teardown/reinit -> land back at
  page 0 -> repeat, forever, at roughly one full cycle per tick (~16ms).
  Confirmed live: `game_time` never advanced past 5 and the FE page id never
  left 0 across 20+ consecutive cycles. Visually this presented as either a
  black screen (nothing ever renders a full frame) or the logos appearing to
  loop indefinitely, depending on exactly where in the cycle the hook fired.
- The original doc finding above (single manual write from a live debugger,
  well after the intro had been running for a while, landing cleanly on the
  main menu and staying there) is **not reproduced by firing this hook
  automatically from the very first tick** -- the manual probe likely caught
  the framework in a later, different state (e.g. `dword_82E4F80C` already
  settled to the "attract" mode object rather than mid-initialization) where
  the same write has different, non-looping consequences.

### What a real fix probably needs

`a1+184` is too blunt an instrument. The page-navigation primitive the game
itself uses to move between FE pages is `sub_825CFF90` (writes the page id at
`root+4` and invokes the outgoing/incoming page's vtable teardown/setup,
`root` being `a1-148`), dispatched via `sub_825CE8E8` (see the
`LeaderboardLogonRouteToScreen`/`LeaderboardEventSigninBypass` hooks in
`nocturnerecomp_hooks.cpp` for existing examples of intercepting page-switch
call sites in this codebase). A working skip-intros feature almost certainly
needs to hook wherever the intro's own Update logic *already* calls
`sub_825CFF90`/`sub_825CE8E8` to advance off page 0 (e.g. its own logo-timer
expiry) and redirect that call's destination straight to page 1 (main menu),
the same pattern as the existing leaderboard-routing hooks -- not toggle the
mode-exit flag from outside that logic entirely.
