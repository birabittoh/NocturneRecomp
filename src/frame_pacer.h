// nocturnerecomp - steady internal-framerate pacer.
//
// The PS1 game inside this Xbox 360 title runs a fixed-timestep simulation
// clock: sub_8258B3B8 runs Update()s until game_time (app+2236) passes
// target_time (app+2232), where 300 units == one real second (one 60Hz frame
// == 5 units). Left to itself the game advances target_time from the GPU
// vblank-interrupt counter, clamped to 1-2 frames per mode-loop iteration --
// so the simulation rate is only ever as steady as the (jittery, headless-
// emulated) vblank ISR cadence beating against the present/mode-loop rate.
// That beat is what makes the guest hover at ~58-59fps and oscillate.
//
// This module removes that coupling: a dedicated steady-clock thread drives
// target_time directly at exactly 300 units per real second (scaled by the
// fast-forward time scalar), so the simulation tracks real wall-time at a
// rock-steady 60Hz regardless of vblank/present jitter -- using the game's
// own catch-up logic (target is a ceiling: when we hold it low the game runs
// zero Updates and self-creeps it up; when a host hitch leaves game behind it
// runs catch-up Updates). See docs/native-renderer-pacing-investigation.md.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

class FramePacer {
 public:
  FramePacer() = default;
  ~FramePacer();

  // Starts the pacer thread. Safe to call once runtime() is live (OnPostSetup);
  // the thread self-guards until the game has launched and app.singleton_ptr
  // resolves, so it can start before the guest is up.
  void Bind(rex::Runtime* runtime);

  // Stops and joins the thread (OnShutdown, before the runtime is torn down).
  void Stop();

 private:
  void ThreadMain();

  rex::Runtime* runtime_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

// Process-wide instance shared between the app hooks.
FramePacer& GetFramePacer();

}  // namespace nocturne
