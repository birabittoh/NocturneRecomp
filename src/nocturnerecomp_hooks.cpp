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

// The Leaderboards screen's per-frame event handler (sub_825C2CB0) gates its
// entire switch on XamUserGetSigninState(user) == SignedInToLive at its very
// top; if not signed in it immediately re-posts a page-switch event back to
// the logon interstitial/main menu (sub_825CE8E8(8, ...)) instead of falling
// through to handle the event. With no real Live signin this fires on
// *every* call -- including the very first Update tick right after
// LeaderboardActivateSpinSkip lets the screen open -- so the screen flashes
// its "error" state for a frame (set earlier in sub_825C0848, since the
// leaderboard read never completes) and then immediately bounces back to
// the main menu. Skip the signin check and always take the "signed in"
// path (loc_825C2D0C), mirroring the spin-skip: the pause-menu leaderboard
// flow proves the screen works fine locally without a real signin.
bool LeaderboardEventSigninBypass() {
  return true;
}
