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
// The guest renders into one 1280x720 colour target. A Resolve copies it into
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
};

// Record one guest draw into the frame. Must be called after the guest has
// written the vertices and before anything changes the device state the draw
// was issued under. `base` is guest memory.
void GuestDrawInline(uint8_t* base, const GuestInlineDraw& draw);

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
