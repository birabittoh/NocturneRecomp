// nocturnerecomp - ReXGlue Recompiled Project
//
// The guest hooks. The Xbox 360 D3D9 runtime is statically linked into the
// game, so there are no imports to hook: the functions below are located by
// address (named in nocturnerecomp_d3d9.toml, which carries the evidence for
// each) and replaced with REX_HOOK_RAW. Each hook runs the guest's own code
// first (via __imp__<name>) so the device object keeps its shadows of every
// register the renderer reads back, then observes the call.
//
// Nothing here consumes the command buffer the guest writes packets into. The
// waits that assume a GPU is draining it are the only thing that has to be
// short-circuited.

#include <rex/hook.h>

#include "native_renderer.h"
#include "native_renderer_plume.h"

// The recompiled bodies the hooks below wrap. The strong REX_HOOK_RAW
// definitions override the weak generated aliases, so calling __imp__ runs the
// original.
extern "C" REX_FUNC(__imp__D3DDevice__Swap);

// The guest's final frame boundary: (device, ...). Present the host frame once
// the guest's own swap has run.
REX_HOOK_RAW(D3DDevice__Swap) {
  __imp__D3DDevice__Swap(ctx, base);

  nocturne::PlumePresentFrame();
  nocturne::NotifyGuestFrame();
}

// Swap brackets its frame throttle (spin while the submitted counter runs more
// than 15 frames ahead of the retired one) with Begin/Poll/End. Poll returns 1
// to keep spinning and, once a 5000 timebase tick window expires, calls the GPU
// hang recovery. Returning 0 makes every such loop exit normally. One hook
// covers every waiting caller, and keeps the hang path unreachable, which it
// otherwise would not be, since with no GPU every wait exceeds the window.
REX_HOOK_RAW(D3DDevice__ThrottleWait_Poll) {
  (void)base;
  ctx.r3.u64 = 0;
}

// The guest emits a wait-for-idle packet, submits it, and busy waits until the
// GPU clears it; nothing does, so this returns success immediately. Reached
// from the renderer init through SetRingBufferParameters, which drains the
// previous ring before replacing it. SetRingBufferParameters itself is not
// stubbed: running it keeps the ring memory allocated and the command buffer
// pointers valid, so the guest's packet writers write somewhere harmless.
REX_HOOK_RAW(D3DDevice__BlockUntilGpuIdle) {
  (void)base;
  ctx.r3.u64 = 0;
}
