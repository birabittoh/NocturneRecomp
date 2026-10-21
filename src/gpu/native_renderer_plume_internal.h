// nocturnerecomp - ReXGlue Recompiled Project
//
// Shared between the translation units that speak Plume: the backend that owns
// the device and the swap chain (native_renderer_plume.cpp) and the overlay
// drawer that records into its command list (native_renderer_overlay.cpp).
//
// Not included anywhere else. Plume pulls in d3d12.h and windows.h, and the
// point of the public headers is to keep that out of the guest-facing code.

#pragma once

#include <plume_render_interface.h>
#include <rex/ui/presenter.h>

#include <cstdint>

namespace nocturne {

// How many frames may be recorded and submitted before the CPU blocks on the
// oldest one's fence. Two lets frame N's GPU work run while the CPU records
// frame N+1. Everything the GPU reads out of a frame's own resources therefore
// exists once per slot.
inline constexpr uint32_t kFramesInFlight = 2;

// The per-frame handoff the SDK's detached overlay mode is built around: the
// app derives from AppUIDrawContext, adds its own payload, hands it to
// ImGuiDrawer::Draw, and its ImmediateDrawer casts it back in Begin.
//
// The payload here is the live command list. There is no presenter and no
// separate UI submission: the overlay records straight into the frame the
// backend is about to present.
class PlumeUIDrawContext final : public rex::ui::AppUIDrawContext {
 public:
  PlumeUIDrawContext(uint32_t width, uint32_t height, plume::RenderCommandList* commands,
                     plume::RenderFormat render_target_format)
      : rex::ui::AppUIDrawContext(width, height),
        commands_(commands),
        render_target_format_(render_target_format) {}

  plume::RenderCommandList* commands() const { return commands_; }
  plume::RenderFormat render_target_format() const { return render_target_format_; }

 private:
  plume::RenderCommandList* commands_;
  plume::RenderFormat render_target_format_;
};

// The backend's device and queue, or null before it is up. The overlay drawer
// creates its own pipelines and buffers from these, and uses the queue only for
// one-shot texture uploads; frame recording goes through the command list the
// draw context carries.
plume::RenderDevice* PlumeDevice();
plume::RenderCommandQueue* PlumeQueue();

// Which blob format the backend's shaders have to be in. A property of the
// render interface, decided by the API that was selected, not by the adapter.
plume::RenderShaderFormat PlumeShaderFormat();

// The format the swap chain was created with. Anything that draws into the
// presented image has to build its pipeline for this.
plume::RenderFormat PlumeSwapChainFormat();

// Which slot the frame currently being recorded owns, in [0, kFramesInFlight).
uint32_t PlumeFrameSlot();

}  // namespace nocturne
