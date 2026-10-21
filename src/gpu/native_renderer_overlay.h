// nocturne - ReXGlue Recompiled Project
//
// A Plume-backed ui::ImmediateDrawer, which draws the SDK's overlays (F3 debug,
// F4 settings, toasts, the mod manager) in native rendering mode.
//
// With no GPU plugin the SDK creates no presenter and asks the app for a drawer
// through ReXApp::OnCreateImmediateDrawer; the default returns nullptr, meaning
// "no overlay".
//
// The SDK's contract for this mode:
//  * the drawer is constructed presenter-less, and OnEnterPresenter /
//    OnLeavePresenter are never called, so all GPU setup has to be lazy;
//  * CreateTexture MUST return nullptr rather than fail loudly when the device
//    is not up yet, because the SDK uploads the ImGui font atlas on the first
//    draw, which can happen before the backend exists.

#pragma once

#include <cstdint>
#include <memory>

namespace rex::ui {
class ImmediateDrawer;
}

namespace nocturne {

// Build the overlay drawer. Returns null when the Plume backend is not up, in
// which case the SDK simply runs without overlays, exactly as it does now.
std::unique_ptr<rex::ui::ImmediateDrawer> CreatePlumeImmediateDrawer();

}  // namespace nocturne
