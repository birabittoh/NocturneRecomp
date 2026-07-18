// See frame_pacer.h for the design rationale.
#include "frame_pacer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

REXCVAR_DEFINE_BOOL(frame_pacer_enabled, false, "Game",
                     "Drive the sim clock from a steady real-time pacer thread instead of the "
                     "guest's own vblank-driven clock (experimental)");

namespace nocturne {

namespace {

// Application-object field offsets (see docs/native-renderer-pacing-
// investigation.md and src/fast_forward.cpp -- kept in sync with those).
constexpr uint32_t kTargetTimeOffset = 2232;
constexpr uint32_t kGameTimeOffset = 2236;

// 300ths-of-a-second units per real second at 1x speed -- by definition of the
// game's time unit, so this is refresh-rate-independent (60Hz just means 5
// units/frame; the real-time rate is always 300/s).
constexpr double kUnitsPerSecondAt1x = 300.0;

// Cap on how much target_time may advance in a single tick, so a starved
// pacer thread (debugger break, huge host hitch) can't force one enormous
// catch-up burst -- mirrors the game's own 1-2 frame vblank clamp.
constexpr double kMaxUnitsPerTick = 30.0;  // 6 frames

// Never let target lead game_time by more than this, so recovering from a host
// stall costs a bounded catch-up rather than a multi-second fast-forward.
constexpr double kMaxLeadUnits = 150.0;  // 30 frames

// If the guest's own target_time diverges from ours by more than this, assume
// a genuine reset (screen transition zeroes the clock) or a desync rather than
// the normal <=2-frame self-bump, and re-anchor to the guest value.
constexpr double kResyncThresholdUnits = 300.0;  // 1 second

// Pacer tick period. Finer than a frame so target_time crosses each 5-unit
// Update boundary close to its real-time moment (sub-frame smoothness) and so
// the authoritative write wins promptly over the game's own per-frame writes.
constexpr auto kTickPeriod = std::chrono::milliseconds(2);

// Plausibility bounds (mirror fast_forward.cpp) -- guard against running on a
// build/state where this address doesn't hold a sane Application object.
constexpr int64_t kMaxPlausibleTimeValue = 100'000'000;

int32_t ReadGuestI32BE(rex::memory::Memory* memory, uint32_t guest_address) {
  const uint8_t* host_address = memory->TranslateVirtual<const uint8_t*>(guest_address);
  return static_cast<int32_t>(rex::memory::load_and_swap<uint32_t>(host_address));
}

void WriteGuestI32BE(rex::memory::Memory* memory, uint32_t guest_address, int32_t value) {
  uint8_t* host_address = memory->TranslateVirtual<uint8_t*>(guest_address);
  rex::memory::store_and_swap<uint32_t>(host_address, static_cast<uint32_t>(value));
}

bool RangeReadable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + size - 1) !=
                     rex::memory::PageAccess::kNoAccess;
}

}  // namespace

FramePacer::~FramePacer() { Stop(); }

void FramePacer::Bind(rex::Runtime* runtime) {
  if (running_.load()) {
    return;
  }
  runtime_ = runtime;
  running_.store(true);
  thread_ = std::thread([this] { ThreadMain(); });
}

void FramePacer::Stop() {
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void FramePacer::ThreadMain() {
  rex::thread::set_current_thread_name("Frame Pacer");

  uint32_t singleton_ptr_addr = 0;  // resolved lazily once the game is up
  bool locked = false;              // have we anchored target_ to the guest?
  double target_units = 0.0;        // our authoritative target_time accumulator
  bool guard_logged = false;

  auto next_tick = std::chrono::steady_clock::now();
  auto last_time = next_tick;

  while (running_.load()) {
    next_tick += kTickPeriod;
    rex::thread::Sleep(kTickPeriod);
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - last_time).count();
    last_time = now;

    if (!REXCVAR_GET(frame_pacer_enabled)) {
      locked = false;
      continue;
    }

    auto* memory = runtime_ ? runtime_->memory() : nullptr;
    auto* mod_registry = runtime_ ? runtime_->mod_registry() : nullptr;
    if (!memory || !mod_registry) {
      locked = false;
      continue;
    }

    if (!singleton_ptr_addr) {
      if (auto addr = mod_registry->FindAddress("app.singleton_ptr")) {
        singleton_ptr_addr = *addr;
      } else {
        locked = false;
        continue;
      }
    }

    // Dereference the singleton pointer to the live Application object.
    if (!RangeReadable(memory, singleton_ptr_addr, 4)) {
      locked = false;
      continue;
    }
    uint32_t app_base = static_cast<uint32_t>(ReadGuestI32BE(memory, singleton_ptr_addr));
    if (app_base == 0 || !RangeReadable(memory, app_base + kTargetTimeOffset, 8)) {
      locked = false;  // game not launched yet, or between-object teardown
      continue;
    }

    int32_t game_time = ReadGuestI32BE(memory, app_base + kGameTimeOffset);
    int32_t target_time = ReadGuestI32BE(memory, app_base + kTargetTimeOffset);
    if (std::abs(int64_t(game_time)) > kMaxPlausibleTimeValue ||
        std::abs(int64_t(target_time)) > kMaxPlausibleTimeValue) {
      if (!guard_logged) {
        guard_logged = true;
        REXLOG_INFO("frame_pacer: implausible game/target time -- pacer idle for this state");
      }
      locked = false;
      continue;
    }

    // (Re)anchor to the guest's own value on first lock, after any teardown,
    // or when the guest jumped far enough to mean a real clock reset.
    if (!locked ||
        std::abs(target_units - double(target_time)) > kResyncThresholdUnits) {
      target_units = double(target_time);
      locked = true;
    }

    // Advance our authoritative clock by exactly real-elapsed time, scaled by
    // the fast-forward time scalar (1.0 normally). steady_clock is host real
    // time -- unaffected by guest_time_scalar -- so the scalar is applied once,
    // here, and nowhere else double-counts it.
    double scalar = rex::chrono::Clock::guest_time_scalar();
    if (!(scalar > 0.0)) {
      scalar = 1.0;
    }
    double advance = dt * kUnitsPerSecondAt1x * scalar;
    target_units += std::clamp(advance, 0.0, kMaxUnitsPerTick);

    // Bound how far ahead of the simulation we ever get, so a host stall is
    // repaid as a short catch-up, not a visible fast-forward burst.
    target_units = std::min(target_units, double(game_time) + kMaxLeadUnits);

    WriteGuestI32BE(memory, app_base + kTargetTimeOffset,
                    static_cast<int32_t>(std::llround(target_units)));
  }
}

FramePacer& GetFramePacer() {
  static FramePacer instance;
  return instance;
}

}  // namespace nocturne
