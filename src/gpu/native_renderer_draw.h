// nocturnerecomp - ReXGlue Recompiled Project
//
// The guest's draws, replayed through Plume.
//
// Every draw this title makes goes through BeginVertices: the vertices are
// written straight into the command buffer and the draw packet is emitted by
// Begin itself, so the host consumes a draw lazily, at the next hooked D3D
// entry, by which time the guest has finished filling the block (see
// FlushInline in native_renderer_d3d.cpp). Everything a draw needs is read back
// out of the device object's register shadows at that point: the bound shaders
// and vertex declaration, the constant banks and the render state.
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

// Record one guest draw into the frame. Must be called after the guest has
// written the vertices and before anything changes the device state the draw
// was issued under. `base` is guest memory.
void GuestDrawInline(uint8_t* base, const GuestInlineDraw& draw);

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
