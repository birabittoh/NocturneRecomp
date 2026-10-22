// nocturnerecomp - ReXGlue Recompiled Project
//
// The guest's shader inventory, translated and compiled ahead of time.
//
// The game loads its shaders from a few loose files in assets/MEDIA, one
// container each. scripts/gen-guest-shaders.py translates them to HLSL
// (KryptonRecomp's xenos_hlsl.py) and compiles them with DXC at build time, and
// the result is linked into the executable's read-only data by a generated `.S`.
// Nothing here translates anything.
//
// The game has no shader table, so a bound shader object is resolved to its
// pack entry by hashing the microcode it points at. See the generator's header
// for which shaders are left out of the pack.

#pragma once

#include <cstdint>

#include "guest_shader_pack.h"

namespace nocturne {

using GuestVertexInput = krypton::GuestVertexInput;
using GuestShader = krypton::GuestShader;

// Parse the pack linked into the executable. Safe to call more than once; only
// the first call does any work. Returns false and logs if the pack is malformed,
// in which case every lookup below misses.
bool LoadGuestShaders();

// The pack slot of a bound guest shader object (device+0x3080 for vertex,
// +0x307C for pixel), or -1 if the pack does not have it. `base` is guest
// memory. Results are cached per object, so this is cheap to call per draw.
int FindGuestVertexShader(const uint8_t* base, uint32_t object);
int FindGuestPixelShader(const uint8_t* base, uint32_t object);

// The pack slot of the vertex shader with this key (see gen-guest-shaders.py),
// or -1.
int FindGuestVertexShaderByKey(uint64_t key);

const GuestShader& GuestVertexShader(uint32_t slot);
const GuestShader& GuestPixelShader(uint32_t slot);

}  // namespace nocturne
