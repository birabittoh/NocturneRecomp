// nocturnerecomp - ReXGlue Recompiled Project
//
// See native_renderer_shader_debug.h. The implementation is shared with the
// other ReXGlue titles (KryptonRecomp/native_renderer_shader_debug_impl.h);
// this file only provides the includes, the debug pack symbols, and the
// namespace to define it in.

#include "native_renderer_shader_debug.h"

#include <atomic>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>

#include "guest_shaders.h"

extern "C" {
extern const uint8_t kGuestShaderDebugPackData[];
extern const uint8_t kGuestShaderDebugPackDataEnd[];
}

namespace nocturne {
namespace {

// 'NRGD', with no entries past the two slot tables.
constexpr krypton::DebugPackTraits kGuestShaderDebugPackTraits{0x4447524E, 2, 0};

}  // namespace

#include "native_renderer_shader_debug_impl.h"
}  // namespace nocturne
