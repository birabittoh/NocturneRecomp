// nocturnerecomp - ReXGlue Recompiled Project
//
// The guest's draws, replayed through Plume.
//
// Every draw this title makes goes through BeginVertices: the vertices are
// written straight into the command buffer and the draw packet is emitted by
// Begin itself. So the draw is recorded in the Begin hook, with everything it
// needs read back out of the device object's register shadows there (the bound
// shaders and vertex declaration, the constant banks and the render state),
// and only its vertices are copied later, at the next hooked D3D entry, by
// which time the guest has finished filling the block (see FlushInline in
// native_renderer_d3d.cpp). Waiting for that entry to read the state too would
// see whatever the guest set up for its next draw in between: render state
// setters are not hooked.
//
// The guest draws in 1280x720 pixels into a colour target the size of the window's
// letterboxed view of it. A Resolve copies it into
// the image the present shows and, when asked, clears it. Textures are read
// out of guest memory by the texture mirror (native_renderer_texture.h).
//
// Plume-free, like native_renderer_plume.h; the Plume half of the interface is
// in native_renderer_plume_internal.h.

#pragma once

#include <cstdint>

namespace nocturne {

// One BeginVertices block, as the hook saw it: the device, the Xenos primitive
// type, the vertex count and stride, and the guest address of the vertices.
struct GuestInlineDraw {
  uint32_t device = 0;
  uint32_t primitive = 0;
  uint32_t count = 0;
  uint32_t stride = 0;
  uint32_t data = 0;
  // Vertices the host built, already in host dword order, in place of the guest
  // block at `data`; and the pack slot of the vertex shader to run instead of
  // the bound one (-1 for the bound one).
  const uint8_t* host_vertices = nullptr;
  int vertex_slot = -1;
};

// The size the guest's target is currently built at (0x0 until the first draw
// creates it), and the call that rebuilds it. The caller must have waited for
// every frame in flight: the old images are freed here.
void GuestTargetExtent(uint32_t* width, uint32_t* height);
void GuestResizeTarget(uint32_t width, uint32_t height);

// Where a recorded draw's vertices go, and where they come from: `bytes` of
// guest memory at `source`, to be byte swapped into `destination`. Empty when
// there is nothing left to copy.
struct GuestVertexFill {
  uint8_t* destination = nullptr;
  uint32_t source = 0;
  uint32_t bytes = 0;
};

// Record one guest draw into the frame, under the device state as it is now.
// `base` is guest memory. Host vertices are copied here; guest ones are not
// written yet, so the result says where they go, for GuestFillVertices once
// they are. The copy has to happen before the frame is submitted.
GuestVertexFill GuestDrawInline(uint8_t* base, const GuestInlineDraw& draw);
void GuestFillVertices(uint8_t* base, const GuestVertexFill& fill);

// The guest's tessellated quad draw (D3DDevice::DrawVerticesTessellated): the
// four control points of stride 32 at guest address `control_points`, drawn
// with the bound pixel shader and state. See native_renderer_tessellation.cpp.
void GuestDrawTessellatedQuad(uint8_t* base, uint32_t device, uint32_t control_points);

// The guest's Resolve: (device, flags, ..., destination texture, ..., clear
// colour pointer). Copies the render target into the image the next present
// shows, which also stands in for `destination` wherever a draw samples it,
// then clears it when `flags` asks (D3DRESOLVE_CLEARRENDERTARGET).
void GuestResolve(uint8_t* base, uint32_t device, uint32_t flags, uint32_t destination,
                  uint32_t clear_color);

// The guest's ClearF: (device, flags, rect or null, float4 colour pointer).
// Clears the render target to the colour, within the rect when there is one.
void GuestClear(uint8_t* base, uint32_t device, uint32_t flags, uint32_t rect, uint32_t color);

}  // namespace nocturne
