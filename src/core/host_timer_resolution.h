// nocturnerecomp - ReXGlue Recompiled Project
//
// The frame pacer sleeps to hit each present deadline
// (NativeCommandProcessor's pacer, and the headless vblank thread). Windows
// rounds a sleep up to the current system timer resolution, which defaults to
// about 15.6 ms, so at the default tick a 16.6 ms frame interval overshoots by
// most of a frame and pacing is visibly lumpy.
//
// Raising the host tick shrinks that overshoot. It does not change the average
// frame rate: the pacer is drift-correcting, anchoring each frame to the
// intended deadline rather than to the actual wake time, so overshoot is repaid
// out of the following frame's slack either way. What the tick rate buys is
// lower frame-to-frame jitter, not a different steady-state rate.
//
// This used to be an unconditional timeBeginPeriod(1) in nocturnerecomp_app.h.
// 1 ms is finer than the pacer needs and raises the tick process wide for the
// whole run, which draws power and spins fans up on some machines, so it is a
// setting now rather than a constant.
//
// This lives here, in the game, rather than in the SDK on purpose: it is a
// property of this title's pacing, not of the runtime, and keeping it
// project-side means the shipped SDK stays the stock pinned nightly.

#pragma once

namespace nocturne {

// Raises the process-wide host timer resolution to the host_timer_resolution_ms
// cvar, if that is finer than what the host already provides. Call at startup,
// after the config files are loaded and before the guest starts, and again
// whenever the cvar changes.
//
// Safe to call repeatedly: it reads the cvar afresh, no-ops when the applied
// period is already the requested one, and otherwise swaps the outstanding
// request for the new one without letting the tick rate dip in between. That is
// what lets the setting apply live rather than needing a restart; the pacer
// reads the clock and sleeps afresh every frame, so it picks up the new
// granularity on its next sleep.
//
// No-op when the cvar is 0, and on non-Windows hosts (POSIX sleeps already run
// at nanosecond granularity).
void ApplyHostTimerResolution();

// Releases the raise requested by ApplyHostTimerResolution. Call once on
// shutdown. No-op if nothing was raised.
void ReleaseHostTimerResolution();

}  // namespace nocturne
