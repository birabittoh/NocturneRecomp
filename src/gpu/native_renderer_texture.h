// nocturnerecomp - ReXGlue Recompiled Project
//
// The texture mirror: a guest texture fetch constant turned into a host texture
// and sampler.
//
// By the time a texture is bound the game has put its texels in GPU visible
// memory, and the fetch constant SetTexture copied into the device describes
// them exactly: format, extent, pitch, tiling and endianness. So nothing here
// parses a file; the texels are read out of guest memory, untiled, byte
// swapped, widened where the host has no matching format, and uploaded.
//
// Every texel is expanded in the hardware's component order (X in the lowest
// bits), and the fetch constant's swizzle is applied by the shaders
// (xe_texture_fetch_post) exactly as the console applies it. So a format only
// has to agree with Xenos about which bits are X, Y, Z and W, never about
// which of those is red.
//
// Eternal Sonata's mirror is the reference (../EternalSonataReprise); this is
// the 2D subset: level 0 only, no write watches, no eviction.
//
// Internal to the draw path; speaks Plume.

#pragma once

#include <cstdint>

#include "native_renderer_plume_internal.h"

namespace nocturne {

// A decoded fetch constant. Field positions are xenia's
// xe_gpu_texture_fetch_t.
struct TextureFetch {
  uint32_t type = 0;  // 2 is a texture
  uint32_t format = 0;
  uint32_t endianness = 0;
  uint32_t swizzle = 0x688;
  // The physical address, with the 0xE0000000 aperture's page of skew
  // applied; what a texture and the resolve that wrote it agree on.
  uint32_t base_address = 0;
  // The same field before the fixup, which is what is read through.
  uint32_t raw_base_address = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;  // in texels
  bool tiled = false;
  bool packed_mips = false;
  // Sampler half.
  uint32_t clamp_x = 0;
  uint32_t clamp_y = 0;
  uint32_t mag_filter = 0;
  uint32_t min_filter = 0;
};

// Decode six fetch constant dwords (host order).
TextureFetch DecodeTextureFetch(const uint32_t words[6]);

// The fetch constant at guest address `address` (the device's slot, device +
// 1024 + 24 * stage, or a texture object's header + 0x1C). False when it does
// not hold a texture.
bool ReadTextureFetch(const uint8_t* base, uint32_t address, TextureFetch& out);

// The host texture for `fetch`, in SHADER_READ, or null when it could not be
// produced (an unsupported format, an unreadable source). Uploads, and
// refreshes a texture whose guest bytes changed since the last frame it was
// checked in, through `commands`, so the copy is ordered against the frame's
// draws.
plume::RenderTexture* MirrorTexture(plume::RenderCommandList* commands, uint8_t* base,
                                    const TextureFetch& fetch);

// The host sampler for `fetch`'s filter and address modes.
plume::RenderSampler* MirrorSampler(const TextureFetch& fetch);

// A new frame started recording into frame slot `slot`; that slot's staging
// buffers are free again.
void TextureMirrorBeginFrame(uint32_t slot);

void ShutdownTextureMirror();

}  // namespace nocturne
