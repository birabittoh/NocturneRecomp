#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/xvideo.h>
#include <rex/ui/flags.h>

// The guest is always told 1280x720: the title lays its UI out for that and
// sizes its D3D device from the mode. The SDK's own XGetVideoMode reports the
// saved window_width/window_height instead, and a mode that large makes
// Direct3D_CreateDevice fail with E_OUTOFMEMORY, leaving the device null. The
// host renders at the window's size on top of this (see PersistWindowSize).
namespace {

constexpr uint32_t kGuestWidth = 1280;
constexpr uint32_t kGuestHeight = 720;

void XGetVideoModeHook(rex::system::X_VIDEO_MODE* mode) {
  std::memset(mode, 0, sizeof(*mode));
  mode->display_width = kGuestWidth;
  mode->display_height = kGuestHeight;
  mode->is_interlaced = 0;
  mode->is_widescreen = 1;
  mode->is_hi_def = 1;
  mode->refresh_rate = static_cast<float>(REXCVAR_GET(video_mode_refresh_rate));
  mode->video_standard = 1;  // NTSC
  mode->unknown_0x8a = 0x4A;
  mode->unknown_0x01 = 0x01;
}

}  // namespace

REX_HOOK(__imp__XGetVideoMode, XGetVideoModeHook);
