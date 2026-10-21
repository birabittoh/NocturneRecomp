// nocturnerecomp - ReXGlue Recompiled Project
//
// Native renderer: draws the guest through Plume by intercepting it at the
// Direct3D 9 API boundary, with its shaders translated ahead of time by
// KryptonRecomp. Nothing emulates Xenos, so there is no ring buffer, PM4
// parsing or EDRAM.
//
// OnPreSetup leaves RuntimeConfig::graphics null, so the SDK loads no GPU
// plugin and runs headless, driving the guest's vblank interrupt from its own
// timer thread. Guest paths that wait on the GPU (the fence and throttle waits
// in Swap) are intercepted, since nothing ever advances the counters they poll.

#pragma once

#include <functional>

namespace rex::ui {
class Window;
}

namespace nocturne {

// Called once per guest frame, from the swap hook, on the guest thread that
// issued the swap. The SDK fires its own per frame callback from the emulated
// GPU's swap packet, which this renderer does not have, so anything that would
// have ridden on it (the mod registry's tick above all) has to come from here.
//
// The interval is a guest frame, not a fixed period, so a callback must derive
// elapsed time from a clock rather than counting calls.
void SetGuestFrameCallback(std::function<void()> callback);

// Runs the callback registered above. Called by the swap hook.
void NotifyGuestFrame();

// Brings up the rendering backend. Call once the window exists and before the
// guest starts executing, so no guest D3D call can arrive ahead of it. Later
// calls do nothing.
void InitNativeRenderer(rex::ui::Window* window);

}  // namespace nocturne
