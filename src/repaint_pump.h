// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/ui/presenter.h>
#include <rex/ui/ui_drawer.h>

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
    presenter_->RequestUIPaintFromUIThread();
  }

 private:
  rex::ui::Presenter* presenter_;
};

}  // namespace nocturne
