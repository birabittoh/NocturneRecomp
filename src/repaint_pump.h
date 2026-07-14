// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_drawer.h>

// 0 = uncapped. Needed because neither present mode this renderer uses
// throttles host fps on its own (see nocturnerecomp_app.h's vsync cvar
// comment): mailbox is non-blocking, and immediate obviously doesn't block
// either, so without this the UI thread's repaint loop below spins as fast
// as the GPU allows, pegging a CPU core and the GPU for no visual benefit
// past the display's actual refresh rate (observed: ~800fps on a Linux/
// Wayland setup with no compositor-side throttling, vs. Windows/DWM which
// happens to throttle mailbox close to refresh on its own).
REXCVAR_DEFINE_INT32(max_host_fps, 120, "Video", "Cap host present rate (0 = uncapped)");

namespace nocturne {

// Native renderer: decouples host present rate from the guest's paced 60fps
// (see docs/native-renderer-pacing-investigation.md for why the guest side is
// paced via PresentFrame's 16ms sleep and must stay that way). This drawer
// draws nothing -- its only job is to re-arm the UI thread's paint loop every
// frame via Presenter::RequestUIPaintFromUIThread(), so PaintFromUIThread
// keeps re-presenting the latest guest-output mailbox image at monitor
// refresh instead of only once per guest frame (RefreshGuestOutput's own
// poke). This is the same paint-mode/UI-tick machinery the xenos gpu_plugin
// path relies on for its 120fps-host/60fps-guest split -- see
// rexglue-sdk src/ui/presenter.cpp (GetDesiredPaintModeFromUIThread,
// AreUITicksNeededFromUIThread, DXGIUITickThread) for the full mechanism.
// Registered/removed alongside the real imgui UI drawer in
// NocturnerecompApp::OnPreLaunchModule (nocturnerecomp_app.h).
class RepaintPumpDrawer : public rex::ui::UIDrawer {
 public:
  explicit RepaintPumpDrawer(rex::ui::Presenter* presenter) : presenter_(presenter) {}

  void Draw(rex::ui::UIDrawContext& context) override {
    int32_t max_fps = REXCVAR_GET(max_host_fps);
    if (max_fps > 0) {
      auto min_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double, std::milli>(1000.0 / max_fps));
      auto now = std::chrono::steady_clock::now();
      auto elapsed = now - last_paint_time_;
      if (elapsed < min_interval) {
        std::this_thread::sleep_for(min_interval - elapsed);
      }
      last_paint_time_ = std::chrono::steady_clock::now();
    }
    presenter_->RequestUIPaintFromUIThread();
  }

 private:
  rex::ui::Presenter* presenter_;
  std::chrono::steady_clock::time_point last_paint_time_;
};

}  // namespace nocturne
