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
