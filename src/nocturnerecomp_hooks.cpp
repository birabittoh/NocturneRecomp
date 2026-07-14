#include "generated/nocturnerecomp_init.h"

#include <rex/memory/utils.h>
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

// Main Menu -> Leaderboards goes through FE page 8, the Xbox Live logon
// interstitial (sub_825B69A0). Its Update posts a "switch page" event when
// the logon attempt resolves; with no real Live signin it resolves as a
// failure and bounces back to the main menu (page 1). When the logon was
// started for the Leaderboards flow (page-8 object +544 mode == 1), redirect
// that exit to page 3 -- the real Leaderboards screen -- which renders fine
// with no Live data (its load path is a no-op without a signin). r5 holds
// the destination page index at loc_825B6E48; r31 holds the page-8 object.
void LeaderboardLogonRouteToScreen(PPCRegister& r5, PPCRegister& r31) {
  auto* mode_ptr = REX_KERNEL_STATE()->memory()->TranslateVirtual(r31.u32 + 544);
  const uint32_t mode = rex::memory::load_and_swap<uint32_t>(mode_ptr);
  if (mode == 1 && r5.u32 == 1) {
    r5.u64 = 3;
  }
}

// The Leaderboards screen's page-activate (sub_825C1AB8) busy-spins until
// the async leaderboard read leaves states 1/2 (sub_82584C18). Without a
// Live signin that read never reaches a terminal state, so the spin would
// hang the Main XThread forever. Skip it -- the pause-menu flow proves the
// screen is fine without waiting (it never enters this path when Live is
// unavailable).
bool LeaderboardActivateSpinSkip() {
  return true;
}
