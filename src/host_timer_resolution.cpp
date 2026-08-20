#include "host_timer_resolution.h"

#include <mutex>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#if defined(_WIN32)
#include <windows.h>

#include <mmsystem.h>
#endif

// Windows only, and deliberately not defined at all elsewhere: a POSIX host
// already sleeps at nanosecond granularity, so there is nothing to trade off
// and a knob offering the choice would be a lie. The settings row keys off this
// cvar's absence to disappear with it.
//
// 5 is the default: enough to cut the pacer's sleep overshoot to a small
// fraction of a 16.6 ms frame, without holding the whole process at the finest
// tick Windows offers. See host_timer_resolution.h.
//
// 1 is what this used to be hardcoded to, and it is overkill. The pacer sleeps
// once per frame against a ~16.6 ms interval, so 5 ms already puts the
// overshoot well inside the noise; going to 1 ms buys nothing you can see while
// holding the tick rate at its finest setting process wide for the entire run,
// which draws real power and is what was spinning fans up.
//
// 0 disables the raise entirely, leaving the stock ~15.6 ms tick. That is
// nearly a whole frame at 60 Hz and measured live it makes the game unplayable.
//
// There is no curated row for any of this: 5 ms is right for everyone, so the
// only place it surfaces is the settings dialog's Advanced section, as the raw
// millisecond value, for anyone who wants to tune it by hand.
#if defined(_WIN32)
REXCVAR_DEFINE_UINT32(host_timer_resolution_ms, 5, "Video",
                      "Host timer resolution in milliseconds, raised so the frame pacer's sleeps "
                      "land close to their deadlines. 5 is plenty; 1 is overkill and just draws "
                      "more power, and 0 leaves the host default, which is unplayable. Windows "
                      "only.");
#endif

namespace nocturne {

namespace {

#if defined(_WIN32)
// The period handed to timeBeginPeriod, so a later change or shutdown can hand
// the identical value to timeEndPeriod. 0 means nothing is currently raised.
//
// Every timeBeginPeriod has to be matched by exactly one timeEndPeriod with the
// same value: the requests are refcounted per process, so losing track here
// leaks a raised tick for the rest of the run.
UINT g_applied_period_ms = 0;

// ApplyHostTimerResolution runs once at startup on the main thread and again
// from the settings overlay on the UI thread. Those never overlap today, but
// the begin/end pairing above is exactly the kind of state that breaks
// silently and permanently if they ever do.
std::mutex g_mutex;

// Whether the resolved state has been logged at least once. Without this the
// "left at the system default" line is unreachable: with the cvar at 0 and
// nothing raised, the no-op check below matches on the very first call and
// returns before logging anything, which is exactly the case worth reporting.
bool g_logged_state = false;
#endif

}  // namespace

void ApplyHostTimerResolution() {
#if defined(_WIN32)
  // Anyone who writes the cvar gets the change applied, not just the settings
  // overlay: the F4 Advanced list, the console and mods all go through
  // SetFlagByName. Registered on the first call rather than at static init so
  // it lands after the cvar registry exists. Re-entry is not a concern, this
  // reads the cvar but never writes it.
  static std::once_flag once;
  std::call_once(once, [] {
    rex::cvar::RegisterChangeCallback(
        "host_timer_resolution_ms",
        [](std::string_view, std::string_view) { ApplyHostTimerResolution(); });
  });

  const uint32_t requested = REXCVAR_GET(host_timer_resolution_ms);

  // Ask the host what it can actually do before asking for it. A period
  // outside [wPeriodMin, wPeriodMax] is rejected outright, and clamping is
  // more useful than failing: a host that can only manage 10 ms should still
  // get 10 ms rather than staying at 15.6.
  UINT period = static_cast<UINT>(requested);
  if (period) {
    TIMECAPS caps = {};
    if (timeGetDevCaps(&caps, sizeof(caps)) == MMSYSERR_NOERROR) {
      if (period < caps.wPeriodMin)
        period = caps.wPeriodMin;
      if (period > caps.wPeriodMax)
        period = caps.wPeriodMax;
    }
  }

  std::lock_guard lock(g_mutex);
  const UINT previous = g_applied_period_ms;
  if (period == previous && g_logged_state)
    return;

  // Only touch the requests when the period actually changed. Re-running this
  // for an unchanged period would call timeBeginPeriod a second time and leak a
  // refcount, since only one timeEndPeriod is ever paired with it.
  if (period != previous) {
    // Raise before dropping the old request, so the tick rate never dips to the
    // system default in between. Windows keeps the finest outstanding request,
    // so briefly holding both is harmless.
    if (period) {
      if (timeBeginPeriod(period) != TIMERR_NOERROR) {
        REXLOG_WARN("Failed to raise the host timer resolution to {}ms; frame pacing will be lumpy",
                    period);
        return;
      }
    }
    g_applied_period_ms = period;
    if (previous)
      timeEndPeriod(previous);
  }

  g_logged_state = true;
  if (period) {
    REXLOG_INFO("Host timer resolution raised to {}ms for the frame pacer", period);
  } else {
    REXLOG_INFO(
        "Host timer resolution left at the system default; frame pacing will judder "
        "(host_timer_resolution_ms = 0)");
  }
#endif
}

void ReleaseHostTimerResolution() {
#if defined(_WIN32)
  std::lock_guard lock(g_mutex);
  if (!g_applied_period_ms)
    return;
  timeEndPeriod(g_applied_period_ms);
  g_applied_period_ms = 0;
#endif
}

}  // namespace nocturne
