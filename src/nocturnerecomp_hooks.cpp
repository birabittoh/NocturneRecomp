#include "generated/nocturnerecomp_init.h"

#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

// sub_825252C8 is the guest's generic "wait for the GPU to catch up"
// ring/pool primitive -- it polls a GPU-driven counter (address chosen per
// call site; not always the CP ring buffer's read pointer) that only a real
// GraphicsSystem ever advances. Headless (no --gpu_plugin) has no GPU to
// drive any of those counters, so the wait can never resolve on its own.
// Bypass it entirely when headless -- there's nothing to actually wait for.
// See docs/native-renderer-headless-boot.md.
bool HeadlessRingWaitBypass() {
  return REX_KERNEL_STATE()->emulator()->graphics_system() == nullptr;
}
