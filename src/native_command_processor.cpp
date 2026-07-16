// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "native_command_processor.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <spirv-tools/libspirv.h>
#include <xxhash.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/custom_shader.h>
#include <rex/ui/flags.h>
#include <rex/ui/graphics_util.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

namespace nocturne {

// Known real dimensions of the gameplay-preview texture (see
// UploadedTexture::is_gameplay_preview) -- confirmed via repeated,
// frequent (consistent with a live-updating preview) "texture uploaded
// 512x256" log lines, distinct from the game's other, far less frequently
// re-uploaded UI textures.
constexpr uint32_t kGameplayPreviewWidth = 512;
constexpr uint32_t kGameplayPreviewHeight = 256;

namespace {

// Replicates xboxkrnl_video.cpp's GetConfiguredVideoModeWidth/Height (not
// callable directly -- file-local there) so the render target this renderer
// actually allocates matches what VdQueryVideoMode told the guest at boot,
// instead of a hardcoded guess that silently stopped matching whenever
// --resolution/video_mode_width/height picked anything other than 1280x720.
uint32_t ResolveVideoModeWidth() {
  int32_t configured_width = REXCVAR_GET(video_mode_width);
  if (!rex::cvar::HasNonDefaultValue("video_mode_width")) {
    if (rex::cvar::HasNonDefaultValue("window_width") && REXCVAR_GET(window_width) > 0) {
      configured_width = REXCVAR_GET(window_width);
    } else {
      int32_t preset_width = 0;
      int32_t preset_height = 0;
      if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                          preset_height)) {
        configured_width = preset_width;
      }
    }
  }
  return uint32_t(std::clamp(configured_width, 640, 0x0FFF));
}

uint32_t ResolveVideoModeHeight() {
  int32_t configured_height = REXCVAR_GET(video_mode_height);
  if (!rex::cvar::HasNonDefaultValue("video_mode_height")) {
    if (rex::cvar::HasNonDefaultValue("window_height") && REXCVAR_GET(window_height) > 0) {
      configured_height = REXCVAR_GET(window_height);
    } else {
      int32_t preset_width = 0;
      int32_t preset_height = 0;
      if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                          preset_height)) {
        configured_height = preset_height;
      }
    }
  }
  return uint32_t(std::clamp(configured_height, 480, 0x0FFF));
}

}  // namespace

namespace {

uint64_t HashUcode(const std::vector<uint32_t>& ucode) {
  // FNV-1a over the raw dwords -- only needs to be a good cache key, not
  // cryptographically strong.
  uint64_t hash = 0xcbf29ce484222325ull;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(ucode.data());
  size_t byte_count = ucode.size() * sizeof(uint32_t);
  for (size_t i = 0; i < byte_count; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

// Decodes one 16-byte BC3/DXT5 block (already in host byte order, see the
// caller's byteswap handling) into a 4x4 RGBA8 patch, row-major, 4 bytes/texel.
// No CPU decoder for this exists anywhere in the SDK. Standard BC3 layout, written from the format spec.
void DecompressDXT5Block(const uint8_t block[16], uint8_t out_rgba[4 * 4 * 4]) {
  uint8_t alpha0 = block[0];
  uint8_t alpha1 = block[1];
  uint64_t alpha_bits = 0;
  for (int i = 0; i < 6; ++i) {
    alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
  }
  uint8_t alpha_lut[8];
  alpha_lut[0] = alpha0;
  alpha_lut[1] = alpha1;
  if (alpha0 > alpha1) {
    // 7-step interpolation (lut[2..7]): weights must sum to 7 (the divisor),
    // not 6 -- (6-i) instead of (7-i) silently undershot every interpolated
    // value, and the loop stopping at i=5 left lut[7] uninitialized garbage
    // instead of the correct (1*alpha0 + 6*alpha1)/7. Confirmed against
    // dumps/textures/ ground truth: blocks landing on the untouched
    // lut[5]/lut[6]/lut[7] entries decoded to visibly wrong (often much
    // higher/lower, sometimes garbage) alpha, producing the scattered
    // colored-dot artifacts seen around the "Castlevania" logo text edges.
    for (int i = 1; i <= 6; ++i) {
      alpha_lut[1 + i] = uint8_t(((7 - i) * uint32_t(alpha0) + i * uint32_t(alpha1)) / 7);
    }
  } else {
    // 5-step interpolation (lut[2..5], then lut[6]=0, lut[7]=255): same class
    // of bug -- weights must sum to 5, and the loop must fill all four
    // interpolated entries (i=1..4), not stop at i=3 and leave lut[5]
    // uninitialized.
    for (int i = 1; i <= 4; ++i) {
      alpha_lut[1 + i] = uint8_t(((5 - i) * uint32_t(alpha0) + i * uint32_t(alpha1)) / 5);
    }
    alpha_lut[6] = 0;
    alpha_lut[7] = 255;
  }

  uint16_t color0 = uint16_t(block[8] | (block[9] << 8));
  uint16_t color1 = uint16_t(block[10] | (block[11] << 8));
  uint32_t color_indices =
      uint32_t(block[12]) | (uint32_t(block[13]) << 8) | (uint32_t(block[14]) << 16) |
      (uint32_t(block[15]) << 24);

  auto unpack565 = [](uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint32_t rv = (c >> 11) & 0x1F;
    uint32_t gv = (c >> 5) & 0x3F;
    uint32_t bv = c & 0x1F;
    r = uint8_t((rv << 3) | (rv >> 2));
    g = uint8_t((gv << 2) | (gv >> 4));
    b = uint8_t((bv << 3) | (bv >> 2));
  };
  uint8_t color_lut[4][3];
  unpack565(color0, color_lut[0][0], color_lut[0][1], color_lut[0][2]);
  unpack565(color1, color_lut[1][0], color_lut[1][1], color_lut[1][2]);
  // BC3's color block is always 4-color mode (unlike BC1, no punch-through
  // alpha variant), regardless of whether color0 > color1.
  for (int c = 0; c < 3; ++c) {
    color_lut[2][c] = uint8_t((2 * uint32_t(color_lut[0][c]) + uint32_t(color_lut[1][c])) / 3);
    color_lut[3][c] = uint8_t((uint32_t(color_lut[0][c]) + 2 * uint32_t(color_lut[1][c])) / 3);
  }

  for (int texel = 0; texel < 16; ++texel) {
    uint32_t ci = (color_indices >> (2 * texel)) & 3;
    uint32_t ai = uint32_t((alpha_bits >> (3 * texel)) & 7);
    uint8_t* dst = out_rgba + texel * 4;
    dst[0] = color_lut[ci][0];
    dst[1] = color_lut[ci][1];
    dst[2] = color_lut[ci][2];
    dst[3] = alpha_lut[ai];
  }
}

// Byte-swaps a whole dword-aligned buffer per the guest's Endian field,
// matching xenos::GpuSwap's semantics exactly (rex/graphics/xenos.h) --
// applied uniformly to raw bytes with no awareness of sub-field structure
// (matches how the real Vulkan backend's texture_load_*_cs compute shaders
// treat compressed blocks: the swap is mechanical over the whole block,
// not aware of BC3's alpha/color/index sub-layout). k8in16 swaps each
// 2-byte pair independently (bytes 0<->1, 2<->3); k8in32 fully reverses
// each 4-byte dword; k16in32 swaps the two 16-bit halves of each dword
// without touching byte order within each half; kNone copies unchanged.
// size must be a multiple of 4.
void GpuSwapBytes(const uint8_t* src, uint8_t* dst, size_t size, rex::graphics::xenos::Endian e) {
  using rex::graphics::xenos::Endian;
  for (size_t i = 0; i < size; i += 4) {
    const uint8_t* s = src + i;
    uint8_t* d = dst + i;
    switch (e) {
      case Endian::k8in16:
        d[0] = s[1];
        d[1] = s[0];
        d[2] = s[3];
        d[3] = s[2];
        break;
      case Endian::k8in32:
        d[0] = s[3];
        d[1] = s[2];
        d[2] = s[1];
        d[3] = s[0];
        break;
      case Endian::k16in32:
        d[0] = s[2];
        d[1] = s[3];
        d[2] = s[0];
        d[3] = s[1];
        break;
      default:
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = s[3];
        break;
    }
  }
}

// Narrow reimplementation of rex::graphics::util::GetScissor (rexglue-sdk's
// src/graphics/util/draw.cpp) -- not callable directly for the same reason
// as GetHostViewportInfo elsewhere in this file (draw.cpp pulls in the
// plugin-only TextureCache/TraceWriter headers). Only the
// clamp_to_surface_pitch=false variant used by TryResolveCopy's resolve-rect
// computation.
void GetScissorRect(const rex::graphics::RegisterFile& regs, int32_t& x0, int32_t& y0, int32_t& x1,
                    int32_t& y1) {
  using namespace rex::graphics;
  auto window_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  x0 = int32_t(window_tl.tl_x);
  y0 = int32_t(window_tl.tl_y);
  auto window_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  x1 = int32_t(window_br.br_x);
  y1 = int32_t(window_br.br_y);
  if (!window_tl.window_offset_disable) {
    auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += offset.window_x_offset;
    y0 += offset.window_y_offset;
    x1 += offset.window_x_offset;
    y1 += offset.window_y_offset;
  }
  auto screen_tl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  x0 = std::max(x0, int32_t(screen_tl.tl_x));
  y0 = std::max(y0, int32_t(screen_tl.tl_y));
  auto screen_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  x1 = std::min(x1, int32_t(screen_br.br_x));
  y1 = std::min(y1, int32_t(screen_br.br_y));
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::max(x1, x0);
  y1 = std::max(y1, y0);
}

// Maps the guest's RB_BLENDCONTROL blend-factor/op encoding to the matching
// Vulkan enum. Xenos::BlendFactor's numeric values don't line up with
// VkBlendFactor's (e.g. kSrcColor=4 vs. VK_BLEND_FACTOR_SRC_COLOR=2), so this
// has to be a real table, not a cast -- see rexglue-sdk's
// VulkanPipelineCache::WritePipelineRenderTargetDescription/kBlendFactorMap
// (pipeline_cache.cpp) for the reference this replicates (not reused
// directly -- that's a private method of a plugin-only class this renderer
// deliberately doesn't link, same rationale as SystemConstants above).
VkBlendFactor ToVkBlendFactor(rex::graphics::xenos::BlendFactor f) {
  using BF = rex::graphics::xenos::BlendFactor;
  switch (f) {
    case BF::kZero:
      return VK_BLEND_FACTOR_ZERO;
    case BF::kOne:
      return VK_BLEND_FACTOR_ONE;
    case BF::kSrcColor:
      return VK_BLEND_FACTOR_SRC_COLOR;
    case BF::kOneMinusSrcColor:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BF::kSrcAlpha:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case BF::kOneMinusSrcAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BF::kDstColor:
      return VK_BLEND_FACTOR_DST_COLOR;
    case BF::kOneMinusDstColor:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BF::kDstAlpha:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case BF::kOneMinusDstAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BF::kConstantColor:
      return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BF::kOneMinusConstantColor:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case BF::kConstantAlpha:
      return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case BF::kOneMinusConstantAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case BF::kSrcAlphaSaturate:
      return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:
      return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp ToVkBlendOp(rex::graphics::xenos::BlendOp op) {
  using BO = rex::graphics::xenos::BlendOp;
  switch (op) {
    case BO::kAdd:
      return VK_BLEND_OP_ADD;
    case BO::kSubtract:
      return VK_BLEND_OP_SUBTRACT;
    case BO::kMin:
      return VK_BLEND_OP_MIN;
    case BO::kMax:
      return VK_BLEND_OP_MAX;
    case BO::kRevSubtract:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:
      return VK_BLEND_OP_ADD;
  }
}

VkPrimitiveTopology PrimitiveTypeToVkTopology(rex::graphics::xenos::PrimitiveType prim_type) {
  using rex::graphics::xenos::PrimitiveType;
  switch (prim_type) {
    case PrimitiveType::kPointList:
      return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PrimitiveType::kLineList:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveType::kLineStrip:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveType::kTriangleList:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveType::kTriangleFan:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PrimitiveType::kTriangleStrip:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default:
      // Notably rex::graphics::xenos::PrimitiveType::kRectangleList isn't a
      // native Vulkan primitive (the SDK's real backend handles it via
      // Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) --
      // intentionally not implemented yet, see TryDraw. kQuadList is handled
      // separately in TryDraw via a host-synthesized index buffer, so it
      // never reaches this function.
      return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  }
}

// CreateDedicatedAllocationBuffer's kUpload memory type is only guaranteed
// host-*visible*, not necessarily host-*coherent* (util.h's ChooseHostMemoryType
// picks by cached/uncached preference, never checks the coherent bit) -- every
// CPU write to GPU-read mapped memory in this file was missing this flush,
// which is consistent with the observed symptom (real geometry/constants
// decode correctly on the CPU side, going by logged values, but literally
// zero pixels differ from the clear color: the GPU may never have observed
// the writes at all on a non-coherent memory type). Safe to call unconditionally
// even if the memory turns out to be coherent -- flushing coherent memory is a
// harmless no-op per the Vulkan spec.
// Always flushes the whole allocation (offset 0, VK_WHOLE_SIZE) rather than a
// sub-range, sidestepping VUID-VkMappedMemoryRange nonCoherentAtomSize
// alignment requirements entirely -- these are small, short-lived
// allocations (one buffer per upload), so the lack of partial-flush
// granularity doesn't matter.
void FlushMapped(const rex::ui::vulkan::VulkanDevice* vulkan_device, VkDeviceMemory memory) {
  VkMappedMemoryRange range{};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = memory;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  vulkan_device->functions().vkFlushMappedMemoryRanges(vulkan_device->device(), 1, &range);
}

}  // namespace

NativeCommandProcessor::NativeCommandProcessor(rex::ui::vulkan::VulkanProvider* provider,
                                                rex::ui::Presenter* presenter)
    : provider_(provider),
      presenter_(presenter),
      color_target_width_(ResolveVideoModeWidth()),
      color_target_height_(ResolveVideoModeHeight()) {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkCommandPoolCreateInfo pool_create_info;
  pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_create_info.pNext = nullptr;
  pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_create_info.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_create_info, nullptr, &command_pool_) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create the command pool");
    return;
  }

  VkCommandBufferAllocateInfo buffer_allocate_info;
  buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  buffer_allocate_info.pNext = nullptr;
  buffer_allocate_info.commandPool = command_pool_;
  buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  buffer_allocate_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &buffer_allocate_info, &command_buffer_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to allocate the command buffer");
    return;
  }

  VkFenceCreateInfo fence_create_info;
  fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_create_info.pNext = nullptr;
  // Pre-signaled: PresentFrame's first step waits on this fence as "the
  // previous submission", but on the very first call there is no previous
  // submission -- an unsignaled fence there hangs forever.
  fence_create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  if (dfn.vkCreateFence(device, &fence_create_info, nullptr, &submit_fence_) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create the submit fence");
    return;
  }

  pipeline_resources_valid_ = InitializePipelineResources();

  rex::graphics::SpirvShaderTranslator::Features features(provider_->vulkan_device());
  REXGPU_INFO(
      "NativeCommandProcessor: ready (pipeline_resources_valid={} depthClamp={} "
      "max_storage_buffer_range={} shared_memory_binding_count={})",
      pipeline_resources_valid_, provider_->vulkan_device()->properties().depthClamp,
      features.max_storage_buffer_range,
      1u << rex::graphics::SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          features.max_storage_buffer_range));
}

NativeCommandProcessor::~NativeCommandProcessor() {
  FreeTransientBuffers();
  DestroyPipelineResources();

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  if (submit_fence_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(device, submit_fence_, nullptr);
  }
  if (command_pool_ != VK_NULL_HANDLE) {
    // Also frees command_buffer_, allocated from this pool.
    dfn.vkDestroyCommandPool(device, command_pool_, nullptr);
  }
}

void NativeCommandProcessor::InitializeTextureReplacement(
    std::vector<std::filesystem::path> mod_roots, std::filesystem::path dump_root) {
  texture_replacement_ =
      std::make_unique<rex::graphics::TextureReplacement>(std::move(mod_roots), std::move(dump_root));
  REXGPU_INFO("NativeCommandProcessor: texture replacement ready ({} mod root(s))",
             texture_replacement_->mod_roots().size());
}

void NativeCommandProcessor::InitializeShaderReplacement(
    std::vector<std::filesystem::path> mod_roots, std::filesystem::path dump_root) {
  shader_mod_roots_ = std::move(mod_roots);
  shader_dump_root_ = std::move(dump_root);
  REXGPU_INFO("NativeCommandProcessor: shader replacement ready ({} mod root(s))",
             shader_mod_roots_.size());
}

bool NativeCommandProcessor::InitializePipelineResources() {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  // Set 0: guest physical memory, mirrored 1:1 -- see kSharedMemorySize.
  {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    // TESSELLATION_EVALUATION included because tessellated draws run the
    // guest "vertex" shader -- and therefore its SSBO vertex fetches -- in
    // the TES stage (see CreateTessellationHostShaders).
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &shared_memory_layout_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the shared memory descriptor layout");
      return false;
    }
  }

  // Set 1: the 5 constant buffers, binding index == SpirvShaderTranslator::
  // ConstantBuffer enum value (see rexglue-sdk's vulkan/command_processor.cpp
  // descriptor_set_layout_constants_ for the layout this replicates).
  {
    using rex::graphics::SpirvShaderTranslator;
    VkDescriptorSetLayoutBinding bindings[SpirvShaderTranslator::kConstantBufferCount]{};
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      bindings[i].descriptorCount = 1;
    }
    // Every "vertex" buffer also gets TESSELLATION_EVALUATION -- tessellated
    // draws run the guest vertex shader in the TES stage (see
    // CreateTessellationHostShaders).
    bindings[SpirvShaderTranslator::kConstantBufferSystem].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    bindings[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[SpirvShaderTranslator::kConstantBufferBoolLoop].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    bindings[SpirvShaderTranslator::kConstantBufferFetch].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
    layout_info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &constants_layout_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the constants descriptor layout");
      return false;
    }
  }

  // Sets 2/3 (vertex/pixel textures) no longer get a fixed layout here --
  // see GetOrCreateTextureSetLayout/GetOrCreatePipelineLayout, built lazily
  // per shader-pair shape once real shaders are seen.

  // Persistent descriptor pool: just the shared-memory set -- every texture
  // descriptor set (default or real) is now transient/per-draw (see
  // GetOrCreatePipelineLayout's doc comment and TryDraw's texture-set
  // resolution), allocated from transient_descriptor_pool_ instead.
  {
    VkDescriptorPoolSize pool_sizes[1] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the persistent descriptor pool");
      return false;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &shared_memory_layout_;
    if (dfn.vkAllocateDescriptorSets(device, &alloc_info, &shared_memory_set_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to allocate the persistent descriptor sets");
      return false;
    }
  }

  // Default sampler: bilinear, clamp-to-edge -- used both for the 1x1
  // default texture and (for now, see GetOrUploadTexture) every real
  // uploaded texture, since decoding the fetch constant's actual filter/clamp
  // state isn't implemented yet.
  {
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &default_sampler_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the default sampler");
      return false;
    }
  }

  // Nearest sampler: used only for the gameplay-preview texture (see
  // GetOrUploadTexture's is_gameplay_preview check) -- bilinear softened it
  // with a blur the real hardware never applied
  // (d3e285a2bbde72ca4ee5377ae0e5713db5d4755e), but every other in-game
  // texture is meant to be bilinear-filtered as normal, so this isn't the
  // global default_sampler_.
  {
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &gameplay_preview_sampler_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the gameplay-preview sampler");
      return false;
    }
  }

  // Default 1x1 opaque white texture -- see the field comment on
  // default_texture_view_.
  {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {1, 1, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, image_info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            default_texture_image_, default_texture_memory_)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the default texture image");
      return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = default_texture_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &default_texture_view_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the default texture image view");
      return false;
    }

    const uint8_t kWhiteTexel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    if (!UploadTexelsAndTransition(default_texture_image_, 1, 1, kWhiteTexel)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to upload the default texture's pixel");
      return false;
    }
  }

  // Transient descriptor pool for per-draw constants *and* (see the
  // multi-texture-per-stage comment on GetOrCreateTextureSetLayout above)
  // texture sets -- reset (not freed individually) once per frame in
  // PresentFrame. Sized generously: once kQuadList draws stopped being
  // unconditionally skipped, a single frame was observed issuing far more
  // than the original 64-draw budget. Texture-set budget assumes up to
  // kMaxTexturesPerStage images + kMaxTexturesPerStage samplers per stage,
  // per draw, for both vertex and pixel stages -- generous, not exact,
  // since real shaders need far fewer in practice.
  {
    constexpr uint32_t kMaxDrawsPerFrame = 4096;
    constexpr uint32_t kMaxTextureDescriptorsPerFrame =
        kMaxDrawsPerFrame * kMaxTexturesPerStage * 2 /* vertex + pixel stages */;
    VkDescriptorPoolSize pool_sizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         kMaxDrawsPerFrame * rex::graphics::SpirvShaderTranslator::kConstantBufferCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextureDescriptorsPerFrame},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxTextureDescriptorsPerFrame},
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kMaxDrawsPerFrame * 3 /* constants + vertex tex + pixel tex sets */;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &transient_descriptor_pool_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the transient descriptor pool");
      return false;
    }
  }

  // Shared memory buffer: guest physical memory, mirrored 1:1 by byte
  // offset. Persistently mapped; UpdateSharedMemory flushes the written
  // range explicitly after each memcpy (util::MemoryPurpose::kUpload is only
  // guaranteed host-*visible*, not host-*coherent* -- see the FlushMapped
  // comment near the top of this file).
  {
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kSharedMemorySize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kUpload, shared_memory_buffer_,
            shared_memory_memory_, &shared_memory_memory_type_)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the shared memory buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, shared_memory_memory_, 0, kSharedMemorySize, 0,
                        reinterpret_cast<void**>(&shared_memory_mapped_)) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to map the shared memory buffer");
      return false;
    }

    VkDescriptorBufferInfo buffer_info{shared_memory_buffer_, 0, kSharedMemorySize};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = shared_memory_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  }

  // Constants arena (see header): one persistently-mapped uniform buffer that
  // upload_constant_buffer suballocates from, replacing per-draw allocations.
  {
    constants_arena_alignment_ =
        std::max<VkDeviceSize>(1, vulkan_device->properties().minUniformBufferOffsetAlignment);
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kConstantsArenaSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kUpload, constants_arena_buffer_,
            constants_arena_memory_)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the constants arena buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, constants_arena_memory_, 0, kConstantsArenaSize, 0,
                        reinterpret_cast<void**>(&constants_arena_mapped_)) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to map the constants arena buffer");
      return false;
    }
  }

  // Offscreen color target draws accumulate into across a frame.
  {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    image_info.extent = {color_target_width_, color_target_height_, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, image_info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            color_target_image_, color_target_memory_)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the color target image");
      return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = color_target_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    view_info.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &color_target_view_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the color target image view");
      return false;
    }
  }

  // Render pass: single color attachment, clear on begin, and already in
  // TRANSFER_SRC_OPTIMAL when the render pass ends so PresentFrame's blit
  // needs no extra barrier on the source side.
  {
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    if (dfn.vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the render pass");
      return false;
    }

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass_;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &color_target_view_;
    framebuffer_info.width = color_target_width_;
    framebuffer_info.height = color_target_height_;
    framebuffer_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the framebuffer");
      return false;
    }

    // render_pass_continue_: same attachment/subpass, but loadOp=LOAD (must
    // preserve draws already accumulated this guest frame) and
    // initialLayout=TRANSFER_SRC_OPTIMAL (color_target_image_'s actual
    // layout after TryResolveCopy's mid-frame vkCmdCopyImageToBuffer, which
    // doesn't itself change layout). See the field comment on
    // render_pass_continue_.
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (dfn.vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass_continue_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the continue render pass");
      return false;
    }
  }

  // Staging buffer for the image->image copy (see the field comment on
  // color_target_staging_buffer_). A2B10G10R10_UNORM_PACK32 is 4 bytes/texel.
  {
    color_target_staging_size_ =
        VkDeviceSize(color_target_width_) * color_target_height_ * 4;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, color_target_staging_size_,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, color_target_staging_buffer_,
            color_target_staging_memory_)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the color target staging buffer");
      return false;
    }
  }

  // Non-fatal: tessellated draws are just skipped if this fails (logged
  // inside) -- everything else renders fine without it.
  CreateTessellationHostShaders();

  return true;
}

void NativeCommandProcessor::DestroyPipelineResources() {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  for (auto& [key, entry] : pipeline_cache_) {
    if (entry.pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, entry.pipeline, nullptr);
    }
  }
  pipeline_cache_.clear();
  for (auto& [key, layout] : pipeline_layout_cache_) {
    if (layout != VK_NULL_HANDLE) {
      dfn.vkDestroyPipelineLayout(device, layout, nullptr);
    }
  }
  pipeline_layout_cache_.clear();
  for (auto& [key, layout] : texture_set_layout_cache_) {
    if (layout != VK_NULL_HANDLE) {
      dfn.vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
  }
  texture_set_layout_cache_.clear();
  if (tess_control_point_vs_ != VK_NULL_HANDLE) {
    dfn.vkDestroyShaderModule(device, tess_control_point_vs_, nullptr);
    tess_control_point_vs_ = VK_NULL_HANDLE;
  }
  if (tess_quad_tcs_ != VK_NULL_HANDLE) {
    dfn.vkDestroyShaderModule(device, tess_quad_tcs_, nullptr);
    tess_quad_tcs_ = VK_NULL_HANDLE;
  }
  for (auto& [key, entry] : shader_cache_) {
    if (entry.module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, entry.module, nullptr);
    }
  }
  shader_cache_.clear();
  analyzed_shaders_.clear();

  if (color_target_staging_buffer_ != VK_NULL_HANDLE)
    dfn.vkDestroyBuffer(device, color_target_staging_buffer_, nullptr);
  if (color_target_staging_memory_ != VK_NULL_HANDLE)
    dfn.vkFreeMemory(device, color_target_staging_memory_, nullptr);

  if (framebuffer_ != VK_NULL_HANDLE) dfn.vkDestroyFramebuffer(device, framebuffer_, nullptr);
  if (render_pass_ != VK_NULL_HANDLE) dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
  if (render_pass_continue_ != VK_NULL_HANDLE)
    dfn.vkDestroyRenderPass(device, render_pass_continue_, nullptr);
  if (color_target_view_ != VK_NULL_HANDLE)
    dfn.vkDestroyImageView(device, color_target_view_, nullptr);
  if (color_target_image_ != VK_NULL_HANDLE) dfn.vkDestroyImage(device, color_target_image_, nullptr);
  if (color_target_memory_ != VK_NULL_HANDLE) dfn.vkFreeMemory(device, color_target_memory_, nullptr);

  if (shared_memory_mapped_) dfn.vkUnmapMemory(device, shared_memory_memory_);
  if (shared_memory_buffer_ != VK_NULL_HANDLE)
    dfn.vkDestroyBuffer(device, shared_memory_buffer_, nullptr);
  if (shared_memory_memory_ != VK_NULL_HANDLE)
    dfn.vkFreeMemory(device, shared_memory_memory_, nullptr);

  if (constants_arena_mapped_) dfn.vkUnmapMemory(device, constants_arena_memory_);
  if (constants_arena_buffer_ != VK_NULL_HANDLE)
    dfn.vkDestroyBuffer(device, constants_arena_buffer_, nullptr);
  if (constants_arena_memory_ != VK_NULL_HANDLE)
    dfn.vkFreeMemory(device, constants_arena_memory_, nullptr);

  DestroyPostProcessPipeline();

  DestroyTextureCache();
  if (default_texture_view_ != VK_NULL_HANDLE)
    dfn.vkDestroyImageView(device, default_texture_view_, nullptr);
  if (default_texture_image_ != VK_NULL_HANDLE)
    dfn.vkDestroyImage(device, default_texture_image_, nullptr);
  if (default_texture_memory_ != VK_NULL_HANDLE)
    dfn.vkFreeMemory(device, default_texture_memory_, nullptr);
  if (default_sampler_ != VK_NULL_HANDLE) dfn.vkDestroySampler(device, default_sampler_, nullptr);
  if (gameplay_preview_sampler_ != VK_NULL_HANDLE)
    dfn.vkDestroySampler(device, gameplay_preview_sampler_, nullptr);
  for (auto& [key, sampler] : sampler_cache_) {
    if (sampler != VK_NULL_HANDLE) {
      dfn.vkDestroySampler(device, sampler, nullptr);
    }
  }
  sampler_cache_.clear();

  if (transient_descriptor_pool_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorPool(device, transient_descriptor_pool_, nullptr);
  if (descriptor_pool_ != VK_NULL_HANDLE) dfn.vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
  if (shared_memory_layout_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorSetLayout(device, shared_memory_layout_, nullptr);
  if (constants_layout_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorSetLayout(device, constants_layout_, nullptr);
}

void NativeCommandProcessor::FreeTransientBuffers() {
  if (frame_transient_buffers_.empty()) {
    return;
  }
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();
  for (TransientBuffer& buf : frame_transient_buffers_) {
    if (buf.buffer != VK_NULL_HANDLE) dfn.vkDestroyBuffer(device, buf.buffer, nullptr);
    if (buf.memory != VK_NULL_HANDLE) dfn.vkFreeMemory(device, buf.memory, nullptr);
  }
  frame_transient_buffers_.clear();
}

void NativeCommandProcessor::OnPacket(const rex::graphics::PacketInfo& info,
                                       const uint8_t* packet_base) {
  for (const rex::graphics::PacketAction& action : info.actions) {
    if (action.type == rex::graphics::PacketAction::Type::kRegisterWrite &&
        action.register_write.index < rex::graphics::RegisterFile::kRegisterCount) {
      registers_[action.register_write.index] = action.register_write.value;
      // COHER_STATUS_HOST.status is the guest's actual, general "this memory
      // range was written by something other than a normal draw (CPU, DMA,
      // any GPU path this renderer doesn't model -- MakeCoherent in
      // rexglue-sdk's command_processor.cpp treats it the same regardless of
      // source) and any texture/vertex cache needs to invalidate it" signal.
      // Confirmed via a real capture to be exactly what precedes the gameplay-preview
      // texture's upload (a COHER_BASE_HOST write to its exact fetch
      // address, immediately before the game re-touches it), which neither
      // the memexport nor EDRAM-resolve hypotheses actually were. This supersedes
      // both of those narrower invalidation paths.
      if (action.register_write.index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
        InvalidateTextureCacheRange(
            registers_[rex::graphics::XE_GPU_REG_COHER_BASE_HOST],
            registers_[rex::graphics::XE_GPU_REG_COHER_SIZE_HOST]);
      }
    }
  }

  const char* name = info.type_info ? info.type_info->name : "";
  // Note: PM4_INTERRUPT (the swap-completion CPU interrupt) is dispatched by
  // the SDK's HeadlessWriteRegister decode walk, not here -- it owns the
  // scratch-register write-back state needed to check that the guest's
  // interrupt-callback slot is actually armed before firing (dispatching
  // while the slot holds the 0x0BADF00D "disarmed" filler makes the guest
  // ISR trap with "Unanticipated CPU_INTERRUPT").
  if (std::strcmp(name, "PM4_EVENT_WRITE_SHD") == 0) {
    // GPU-completion fence: on real hardware (and on the xenos backend --
    // CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD) the GPU writes
    // either the packet's literal value or the swap counter to a guest
    // physical address once the preceding commands complete. The guest's D3D
    // runtime polls these words to gauge GPU progress (frame-completion
    // fences, command-buffer consumption). Never servicing them left every
    // fence frozen, so the game's frame-pacing logic saw a GPU that made no
    // progress and periodically fast-forwarded to compensate (the ~6s
    // "skip ahead" burst).
    // Since the native path consumes commands synchronously, "the GPU is done
    // with everything decoded so far" is truthful at the moment this packet is
    // decoded, so write the fence immediately.
    auto payload_dword = [packet_base](uint32_t index) {
      return rex::memory::load_and_swap<uint32_t>(packet_base + index * 4);
    };
    uint32_t initiator = payload_dword(1);
    uint32_t address = payload_dword(2);
    uint32_t value = payload_dword(3);
    // Bit 31 of the initiator selects "write the GPU swap counter" (xenos
    // increments its counter_ once per PM4_XE_SWAP; swap_counter_ mirrors
    // that, incremented once per PresentFrame).
    uint32_t data_value = (initiator >> 31) ? swap_counter_ : value;
    auto endianness = static_cast<rex::graphics::xenos::Endian>(address & 0x3);
    address &= ~0x3u;
    data_value = rex::graphics::xenos::GpuSwap(data_value, endianness);
    if (address != 0) {
      rex::memory::store(
          rex::system::kernel_state()->memory()->TranslatePhysical(address), data_value);
    }
  } else if (std::strcmp(name, "PM4_IM_LOAD") == 0) {
    OnShaderLoad(info, packet_base, /*immediate=*/false);
  } else if (std::strcmp(name, "PM4_IM_LOAD_IMMEDIATE") == 0) {
    OnShaderLoad(info, packet_base, /*immediate=*/true);
  } else if (info.type_info && info.type_info->category == rex::graphics::PacketCategory::kDraw) {
    OnDraw(info, packet_base);
  }

  if (info.type_info && info.type_info->category == rex::graphics::PacketCategory::kSwap) {
    // Mirrors xenos's CommandProcessor: counter_ increments once per
    // PM4_XE_SWAP; EVENT_WRITE_SHD fences with the counter-select initiator
    // bit report this value (see the fence handler above).
    ++swap_counter_;
    PresentFrame();
  }
}

void NativeCommandProcessor::OnShaderLoad(const rex::graphics::PacketInfo& info,
                                          const uint8_t* packet_base, bool immediate) {
  // PacketDisassembler doesn't decode these into PacketAction entries (only
  // plain register writes get that treatment), so re-read the payload dwords
  // directly -- same layout CommandProcessor::ExecutePacketType3_IM_LOAD[_IMMEDIATE]
  // reads from the ring (src/graphics/command_processor.cpp), just via
  // packet_base instead of a RingBuffer cursor.
  auto payload_dword = [packet_base](uint32_t index) {
    return rex::memory::load_and_swap<uint32_t>(packet_base + index * 4);
  };

  // Fetch-constant/register decode is known-unreliable for some draws in
  // this stream; a garbage-decoded size here was observed making SpirvShaderTranslator
  // spend ~20s translating one bogus shader, stalling the frame loop. The
  // real intro shaders translated at ~2300 dwords; reject anything wildly
  // past that as corrupt rather than paying to translate it.
  constexpr uint32_t kMaxPlausibleUcodeDwords = 4096;

  uint32_t shader_type_raw;
  uint32_t guest_address;
  uint32_t size_dwords;
  ShaderState state;
  if (immediate) {
    shader_type_raw = payload_dword(1);
    uint32_t start_size = payload_dword(2);
    size_dwords = start_size & 0xFFFF;
    if (size_dwords > kMaxPlausibleUcodeDwords) {
      REXGPU_INFO(
          "NativeCommandProcessor: skipping shader load with implausible size_dwords={} (likely "
          "a garbage-decoded packet)",
          size_dwords);
      size_dwords = 0;
    }
    guest_address = 0;
    // The microcode is embedded right after the 2-dword header, still in
    // guest/big-endian dword order -- Shader's constructor (milestone 3b
    // step 2) does the byteswap itself given std::endian::big, so capture
    // the raw bytes as-is rather than pre-swapping.
    state.ucode.resize(size_dwords);
    const uint32_t* raw = reinterpret_cast<const uint32_t*>(packet_base) + 3;
    std::memcpy(state.ucode.data(), raw, size_dwords * sizeof(uint32_t));
  } else {
    uint32_t addr_type = payload_dword(1);
    shader_type_raw = addr_type & 0x3;
    guest_address = addr_type & ~0x3u;
    uint32_t start_size = payload_dword(2);
    size_dwords = start_size & 0xFFFF;
    if (size_dwords > kMaxPlausibleUcodeDwords) {
      REXGPU_INFO(
          "NativeCommandProcessor: skipping shader load with implausible size_dwords={} (likely "
          "a garbage-decoded packet)",
          size_dwords);
      size_dwords = 0;
    }
    if (guest_address != 0 && size_dwords != 0) {
      const uint32_t* guest_ucode =
          rex::system::kernel_state()->memory()->TranslatePhysical<uint32_t*>(guest_address);
      state.ucode.resize(size_dwords);
      std::memcpy(state.ucode.data(), guest_ucode, size_dwords * sizeof(uint32_t));
    }
  }
  state.guest_address = guest_address;

  // xenos::ShaderType: 0 = vertex, 1 = pixel.
  if (shader_type_raw == 0) {
    active_vertex_shader_ = std::move(state);
  } else if (shader_type_raw == 1) {
    active_pixel_shader_ = std::move(state);
  }
}

void NativeCommandProcessor::OnDraw(const rex::graphics::PacketInfo& info,
                                    const uint8_t* packet_base) {
  const char* name = info.type_info ? info.type_info->name : "";
  bool has_viz_token = std::strcmp(name, "PM4_DRAW_INDX") == 0;

  auto payload_dword = [packet_base](uint32_t index) {
    return rex::memory::load_and_swap<uint32_t>(packet_base + index * 4);
  };

  // PM4_DRAW_INDX: [1]=viz query token, [2]=VGT_DRAW_INITIATOR, [3..]=index
  // buffer base/size if source_select==kDMA.
  // PM4_DRAW_INDX_2: [1]=VGT_DRAW_INITIATOR directly (no viz token).
  // Layout per CommandProcessor::ExecutePacketType3Draw.
  uint32_t initiator_index = has_viz_token ? 2 : 1;
  rex::graphics::reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = payload_dword(initiator_index);

  bool is_indexed = initiator.source_select == rex::graphics::xenos::SourceSelect::kDMA;
  uint32_t index_buffer_base = 0;
  uint32_t index_buffer_size_words = 0;
  if (is_indexed) {
    index_buffer_base = payload_dword(initiator_index + 1) & 0x1FFFFFFF;
    index_buffer_size_words = payload_dword(initiator_index + 2) & 0xFFFFFF;
  }

  uint32_t num_indices = initiator.num_indices;

  ++draws_logged_;
  if (draws_logged_ <= kMaxDrawsLogged) {
    REXGPU_INFO(
        "NativeCommandProcessor: draw#{} {} prim_type={} source_select={} num_indices={} "
        "vs_addr={:08X} vs_size_dwords={} ps_addr={:08X} ps_size_dwords={} "
        "index_buffer_base={:08X} index_buffer_size_words={}",
        draws_logged_, name, static_cast<uint32_t>(initiator.prim_type),
        static_cast<uint32_t>(initiator.source_select), num_indices,
        active_vertex_shader_.guest_address, active_vertex_shader_.ucode.size(),
        active_pixel_shader_.guest_address, active_pixel_shader_.ucode.size(), index_buffer_base,
        index_buffer_size_words);
  }

  // Mirror the initiator into the register file (on real hardware
  // PM4_DRAW_INDX's first payload dword *is* a VGT_DRAW_INITIATOR register
  // write, but this decode path never routes it through a RegisterWrite
  // action) -- TryDraw reads major_mode from it for tessellation detection.
  registers_[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator.value;

  TryDraw(initiator.prim_type, num_indices);
}

rex::graphics::Shader* NativeCommandProcessor::GetOrAnalyzeShader(
    rex::graphics::xenos::ShaderType type, const std::vector<uint32_t>& ucode) {
  if (ucode.empty()) {
    return nullptr;
  }
  uint64_t key =
      HashUcode(ucode) ^ (type == rex::graphics::xenos::ShaderType::kVertex ? 0x1ull : 0x2ull);
  auto existing = analyzed_shaders_.find(key);
  if (existing != analyzed_shaders_.end()) {
    return existing->second.get();
  }
  using rex::graphics::Shader;
  // ucode_data_hash (the Shader object's ucode_data_hash(), distinct from
  // this function's own internal `key`) must be XXH3_64bits over the raw
  // guest/big-endian ucode bytes exactly as the real D3D12/Vulkan xenos
  // backends compute it in PipelineCache::LoadShader (see
  // rexglue-sdk/src/graphics/vulkan/pipeline_cache.cpp:974 -- XXH3_64bits
  // over host_address straight from guest memory, before any byteswap) --
  // NOT over shader->ucode_data(), which the Shader constructor below
  // byteswaps to host-native order for its own internal use. Passing
  // ucode_data() there would make ucode_data_hash() a different value than
  // the real backends report for the same shader, breaking hash parity for
  // mod authors migrating from the old xenos-plugin renderer (its F2
  // overlay reports the real, unswapped-input hash). Every other reader of
  // this shader's hash (GetShaderSnapshot/Details, GetOrTranslateShader,
  // TryDraw's last_active_* tracking) reads shader->ucode_data_hash()
  // instead of recomputing anything, so there is exactly one place this can
  // go wrong.
  uint64_t ucode_data_hash = XXH3_64bits(ucode.data(), ucode.size() * sizeof(uint32_t));
  // Must be a concrete SpirvShader (not the base Shader class) -- after
  // translation, GetOrCreatePipeline/TryDraw need
  // GetTextureBindingsAfterTranslation()/GetSamplerBindingsAfterTranslation(),
  // which SpirvShaderTranslator::PostTranslation only populates via a
  // successful dynamic_cast<SpirvShader*> of the shader it just translated
  // (see spirv_translator.cpp's PostTranslation). Those are the real,
  // dedup'd, binding-index-ordered lists SpirvShaderTranslator's SPIR-V
  // output actually expects -- distinct from (and not necessarily the same
  // count as) base Shader::texture_bindings(), which is the raw per-fetch-
  // instruction list from ucode analysis before any dedup.
  auto shader = std::make_unique<rex::graphics::SpirvShader>(
      type, ucode_data_hash, ucode.data(), ucode.size(), std::endian::big);
  rex::string::StringBuffer disasm_buffer;
  shader->AnalyzeUcode(disasm_buffer);
  static uint32_t disasm_logged = 0;
  if (disasm_logged < 6) {
    ++disasm_logged;
    REXGPU_DEBUG("NativeCommandProcessor: shader disasm ({} dwords, type={}):\n{}", ucode.size(),
               type == rex::graphics::xenos::ShaderType::kVertex ? "vertex" : "pixel",
               shader->ucode_disassembly());
  }
  Shader* ptr = shader.get();
  analyzed_shaders_.emplace(key, std::move(shader));
  return ptr;
}

std::vector<rex::ui::ShaderDebuggerEntry> NativeCommandProcessor::GetShaderSnapshot() const {
  std::vector<rex::ui::ShaderDebuggerEntry> out;
  out.reserve(analyzed_shaders_.size());
  for (const auto& [key, shader] : analyzed_shaders_) {
    // ucode_data_hash() is the real, SDK-parity XXH3 hash set at
    // construction time (see GetOrAnalyzeShader) -- read it, don't
    // recompute it from shader->ucode_data(), which is byteswapped to host
    // order and would give a different (wrong) hash.
    uint64_t xxh3_hash = shader->ucode_data_hash();
    rex::ui::ShaderDebuggerEntry entry;
    entry.ucode_hash = xxh3_hash;
    entry.type = static_cast<uint32_t>(shader->type());
    entry.dword_count = static_cast<uint32_t>(shader->ucode_dword_count());
    entry.disabled = disabled_shader_hashes_.count(xxh3_hash) != 0;
    entry.active = xxh3_hash == last_active_vertex_ucode_xxh3_ ||
                   xxh3_hash == last_active_pixel_ucode_xxh3_;
    if (auto it = shader_profile_.find(xxh3_hash); it != shader_profile_.end()) {
      entry.profile_total_ns = it->second.total_ns;
      entry.profile_draw_count = it->second.draw_count;
    }
    out.push_back(entry);
  }
  return out;
}

rex::ui::ShaderDebuggerDetails NativeCommandProcessor::GetShaderDetails(
    uint64_t xxh3_ucode_hash) const {
  rex::ui::ShaderDebuggerDetails out;
  for (const auto& [key, shader] : analyzed_shaders_) {
    if (shader->ucode_data_hash() != xxh3_ucode_hash) {
      continue;
    }
    out.found = true;
    out.info.ucode_hash = xxh3_ucode_hash;
    out.info.type = static_cast<uint32_t>(shader->type());
    out.info.dword_count = static_cast<uint32_t>(shader->ucode_dword_count());
    out.info.disabled = disabled_shader_hashes_.count(xxh3_ucode_hash) != 0;
    out.info.active = xxh3_ucode_hash == last_active_vertex_ucode_xxh3_ ||
                      xxh3_ucode_hash == last_active_pixel_ucode_xxh3_;
    if (auto it = shader_profile_.find(xxh3_ucode_hash); it != shader_profile_.end()) {
      out.info.profile_total_ns = it->second.total_ns;
      out.info.profile_draw_count = it->second.draw_count;
    }
    out.ucode_disassembly = shader->ucode_disassembly();
    out.ucode_dwords = shader->ucode_data();
    // Per-modification translation detail (SPIR-V disassembly, binary
    // replace) isn't tracked in a queryable-by-hash way by shader_cache_'s
    // modification-folded keys -- left empty rather than guessed.
    break;
  }
  return out;
}

void NativeCommandProcessor::DumpShaderTranslation(uint64_t ucode_hash,
                                                   rex::graphics::xenos::ShaderType type,
                                                   const std::vector<uint8_t>& spirv_bytes) const {
  if (spirv_bytes.empty()) {
    return;
  }
  const auto dump_dir =
      (shader_dump_root_.empty() ? std::filesystem::path("dumps") : shader_dump_root_) /
      "shaders";
  char name[64];
  std::snprintf(name, sizeof(name), "%016llx.%s.native.spv",
               static_cast<unsigned long long>(ucode_hash),
               type == rex::graphics::xenos::ShaderType::kVertex ? "vert" : "frag");
  const auto dest = dump_dir / name;
  // Write once per unique (hash, stage) -- same "don't hammer the disk"
  // rule as TextureReplacement::DumpTexture.
  if (std::filesystem::exists(dest)) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dump_dir, ec);
  FILE* f = std::fopen(dest.string().c_str(), "wb");
  if (!f) {
    return;
  }
  std::fwrite(spirv_bytes.data(), 1, spirv_bytes.size(), f);
  std::fclose(f);
}

bool NativeCommandProcessor::ApplyShaderReplacement(uint64_t ucode_hash,
                                                     rex::graphics::Shader::Translation& translation) {
  for (const auto& mod_root : shader_mod_roots_) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.spv", static_cast<unsigned long long>(ucode_hash));
    const auto mod_path = mod_root / name;
    FILE* f = std::fopen(mod_path.string().c_str(), "rb");
    if (!f) {
      continue;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bool applied = false;
    if (size > 0) {
      std::vector<uint8_t> replacement(static_cast<size_t>(size));
      if (std::fread(replacement.data(), 1, replacement.size(), f) == replacement.size()) {
        translation.set_translated_binary(std::move(replacement));
        REXGPU_INFO("NativeCommandProcessor: loaded replacement SPIR-V {:016x} from {}", ucode_hash,
                   mod_path.string());
        applied = true;
      }
    }
    std::fclose(f);
    if (applied) {
      return true;
    }
  }
  return false;
}

void NativeCommandProcessor::SetShaderDisabledByHash(uint64_t xxh3_ucode_hash, bool disabled) {
  if (disabled) {
    disabled_shader_hashes_.insert(xxh3_ucode_hash);
  } else {
    disabled_shader_hashes_.erase(xxh3_ucode_hash);
  }
}

NativeCommandProcessor::TranslatedShader* NativeCommandProcessor::GetOrTranslateShader(
    rex::graphics::xenos::ShaderType type, const std::vector<uint32_t>& ucode,
    uint32_t interpolator_mask, rex::graphics::Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t tessellation_mode) {
  rex::graphics::Shader* shader = GetOrAnalyzeShader(type, ucode);
  if (!shader) {
    return nullptr;
  }

  // host_vertex_shader_type only varies translation for vertex shaders (see
  // TryDraw's kRectangleList handling); interpolator_mask matters for both
  // (see the header comment on GetOrTranslateShader / TryDraw's computation
  // of it) -- fold both into the cache key so a shader reused with a
  // genuinely different modification gets its own translation instead of
  // aliasing onto the wrong one.
  uint64_t key = HashUcode(ucode) ^
                (type == rex::graphics::xenos::ShaderType::kVertex ? 0x1ull : 0x2ull) ^
                (uint64_t(host_vertex_shader_type) << 8) ^ (uint64_t(tessellation_mode) << 14) ^
                (uint64_t(interpolator_mask) << 16);
  auto existing = shader_cache_.find(key);
  if (existing != shader_cache_.end()) {
    return &existing->second;
  }

  if (shader_cache_.size() >= kMaxShaderCacheEntries) {
    if (!shader_cache_limit_logged_) {
      shader_cache_limit_logged_ = true;
      REXGPU_ERROR(
          "NativeCommandProcessor: shader cache exceeded {} distinct entries; skipping further "
          "translations (likely a garbage-decoded shader rehashing every resubmit)",
          kMaxShaderCacheEntries);
    }
    return nullptr;
  }

  using rex::graphics::Shader;
  using rex::graphics::SpirvShaderTranslator;

  auto program_cntl = registers_.Get<rex::graphics::reg::SQ_PROGRAM_CNTL>();
  uint32_t num_reg = type == rex::graphics::xenos::ShaderType::kVertex ? program_cntl.vs_num_reg
                                                                       : program_cntl.ps_num_reg;
  uint32_t dynamic_regs = shader->GetDynamicAddressableRegisterCount(num_reg);

  SpirvShaderTranslator::Features features(provider_->vulkan_device());
  SpirvShaderTranslator translator(features, /*native_2x_msaa_with_attachments=*/false,
                                   /*native_2x_msaa_no_attachments=*/false,
                                   /*edram_fragment_shader_interlock=*/false);
  SpirvShaderTranslator::Modification modification(
      type == rex::graphics::xenos::ShaderType::kVertex
          ? translator.GetDefaultVertexShaderModification(dynamic_regs, host_vertex_shader_type)
          : translator.GetDefaultPixelShaderModification(dynamic_regs));
  // Real RB_BLENDCONTROL-adjacent bug sibling: the "default" modification
  // leaves interpolator_mask at 0, meaning no data at all is passed from the
  // vertex shader's interpolator exports to the pixel shader's inputs --
  // found via RenderDoc (a 1-instruction "oC0 = r0" pixel shader with no
  // Input variable at all in its SPIR-V interface) to be why textureless
  // solid-color draws (e.g. the intro's black quads) rendered as flat black:
  // the pixel shader's r0 (meant to hold the interpolated vertex color) was
  // simply never wired up.
  if (type == rex::graphics::xenos::ShaderType::kVertex) {
    modification.vertex.interpolator_mask = interpolator_mask;
    // Selects the TES spacing execution mode (equal vs fractional-even) for
    // domain host vertex shader types; ignored otherwise.
    modification.vertex.tessellation_mode = tessellation_mode & 0x3;
  } else {
    modification.pixel.interpolator_mask = interpolator_mask;
  }
  Shader::Translation* translation = shader->GetOrCreateTranslation(modification.value);
  bool ok = translator.TranslateAnalyzedShader(*translation);

  uint64_t ucode_hash = shader->ucode_data_hash();
  if (ok && REXCVAR_GET(shader_dump_enabled)) {
    DumpShaderTranslation(ucode_hash, type, translation->translated_binary());
  }
  // Replacement is scoped to the default modification only (see
  // docs/native-renderer-shader-replacement.md, "Modification-sensitivity"):
  // a replacement module has to already match this draw's interpolator mask/
  // host vertex shader type/tessellation mode or binding/interface mismatches
  // will misrender or fail validation, so anything but the plain default
  // vertex modification (no tessellation) falls through to the normal
  // translation untouched, with a rate-limited log.
  bool is_default_modification =
      type != rex::graphics::xenos::ShaderType::kVertex ||
      (host_vertex_shader_type == Shader::HostVertexShaderType::kVertex && tessellation_mode == 0);
  if (ok && !shader_mod_roots_.empty()) {
    if (is_default_modification) {
      ApplyShaderReplacement(ucode_hash, *translation);
    } else {
      static uint32_t skipped_logged = 0;
      if (skipped_logged < 20) {
        ++skipped_logged;
        REXGPU_INFO(
            "NativeCommandProcessor: skipping shader replacement lookup for {:016x} -- "
            "non-default modification (host_vertex_shader_type={} tessellation_mode={}) not "
            "supported yet",
            ucode_hash, static_cast<uint32_t>(host_vertex_shader_type), tessellation_mode);
      }
    }
  }

  VkShaderModule module = VK_NULL_HANDLE;
  if (ok && !translation->translated_binary().empty()) {
    module = rex::ui::vulkan::util::CreateShaderModule(
        provider_->vulkan_device(),
        reinterpret_cast<const uint32_t*>(translation->translated_binary().data()),
        translation->translated_binary().size());
  }
  if (module == VK_NULL_HANDLE) {
    REXGPU_ERROR("NativeCommandProcessor: failed to translate/compile {} shader",
                type == rex::graphics::xenos::ShaderType::kVertex ? "vertex" : "pixel");
  }

  auto [inserted, _] = shader_cache_.emplace(key, TranslatedShader{shader, module});
  return &inserted->second;
}

bool NativeCommandProcessor::UploadTexelsAndTransition(VkImage image, uint32_t width,
                                                        uint32_t height, const void* rgba_data) {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkDeviceSize data_size = VkDeviceSize(width) * height * 4;
  VkBuffer staging_buffer;
  VkDeviceMemory staging_memory;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, data_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kUpload, staging_buffer, staging_memory)) {
    REXGPU_ERROR("NativeCommandProcessor: failed to allocate a texture staging buffer");
    return false;
  }
  void* mapped = nullptr;
  dfn.vkMapMemory(device, staging_memory, 0, data_size, 0, &mapped);
  std::memcpy(mapped, rgba_data, data_size);
  FlushMapped(vulkan_device, staging_memory);
  dfn.vkUnmapMemory(device, staging_memory);

  VkCommandBuffer upload_cmd;
  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = command_pool_;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;
  bool ok = dfn.vkAllocateCommandBuffers(device, &alloc_info, &upload_cmd) == VK_SUCCESS;
  if (ok) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(upload_cmd, &begin_info);

    VkImageSubresourceRange subresource_range = rex::ui::vulkan::util::InitializeSubresourceRange();

    VkImageMemoryBarrier to_transfer_dst{};
    to_transfer_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer_dst.image = image;
    to_transfer_dst.subresourceRange = subresource_range;
    dfn.vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_transfer_dst);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    dfn.vkCmdCopyBufferToImage(upload_cmd, staging_buffer, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier to_shader_read{};
    to_shader_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_shader_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_shader_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader_read.image = image;
    to_shader_read.subresourceRange = subresource_range;
    dfn.vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_shader_read);

    dfn.vkEndCommandBuffer(upload_cmd);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence upload_fence = VK_NULL_HANDLE;
    ok = dfn.vkCreateFence(device, &fence_info, nullptr, &upload_fence) == VK_SUCCESS;
    if (ok) {
      VkSubmitInfo submit_info{};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &upload_cmd;
      rex::ui::vulkan::VulkanDevice::Queue::Acquisition queue =
          vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      ok = dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, upload_fence) == VK_SUCCESS;
      if (ok) {
        constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull;
        ok = dfn.vkWaitForFences(device, 1, &upload_fence, VK_TRUE, kFenceTimeoutNs) == VK_SUCCESS;
      }
      dfn.vkDestroyFence(device, upload_fence, nullptr);
    }
    // vkFreeCommandBuffers isn't in this SDK's exposed Vulkan function table
    // (same gap as vkCmdBlitImage/vkCmdCopyImage noted elsewhere in this
    // file) -- leaked instead of freed, but capped by kMaxTextureCacheEntries
    // uploads for the process lifetime, so negligible. command_pool_'s own
    // destruction (DestroyPipelineResources) reclaims everything anyway.
  }

  dfn.vkDestroyBuffer(device, staging_buffer, nullptr);
  dfn.vkFreeMemory(device, staging_memory, nullptr);
  return ok;
}

namespace {

// Hand-assembled SPIR-V for the fixed full-screen-triangle vertex shader
// ApplyGameplayPreviewPostProcess's pipeline uses (the classic "one
// oversized triangle covering the whole viewport, no vertex buffer" trick,
// driven purely by gl_VertexIndex -- 0,1,2 map to (-1,-1), (3,-1), (-1,3) in
// NDC). Assembled from text (via SPIRV-Tools' spvTextToBinary, linked in
// already for shader disassembly needs elsewhere in the SDK) rather than
// compiled from GLSL: rex::ui::CustomShader::Compile (used just below for
// the actual post-process fragment shader) only ever compiles a *fragment*
// stage tied to its own fixed preamble -- the SDK doesn't expose glslang's
// general compiler (glslang/Public/ShaderLang.h isn't part of its installed
// include tree, only the narrower SPIRV/GlslangToSpv.h that itself needs
// glslang::TIntermediate to already exist), so there's no supported way to
// compile an arbitrary new vertex shader from GLSL here. This vertex stage
// needs no push constants or varyings -- CustomShaderConstants's
// xe_output_offset/xe_uv are derived from gl_FragCoord in the fragment
// preamble, not from any vertex-stage output.
constexpr char kFullscreenTriangleVertexShaderSpirvAsm[] = R"(
OpCapability Shader
%1 = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %gl_VertexIndex %gl_Position
OpDecorate %gl_VertexIndex BuiltIn VertexIndex
OpDecorate %gl_Position BuiltIn Position
%void = OpTypeVoid
%voidfn = OpTypeFunction %void
%int = OpTypeInt 32 1
%int_1 = OpConstant %int 1
%int_2 = OpConstant %int 2
%ptr_in_int = OpTypePointer Input %int
%gl_VertexIndex = OpVariable %ptr_in_int Input
%float = OpTypeFloat 32
%float_0 = OpConstant %float 0
%float_1 = OpConstant %float 1
%float_2 = OpConstant %float 2
%v4float = OpTypeVector %float 4
%ptr_out_v4float = OpTypePointer Output %v4float
%gl_Position = OpVariable %ptr_out_v4float Output
%main = OpFunction %void None %voidfn
%entry = OpLabel
%vidx = OpLoad %int %gl_VertexIndex
%shl = OpShiftLeftLogical %int %vidx %int_1
%bx = OpBitwiseAnd %int %shl %int_2
%by = OpBitwiseAnd %int %vidx %int_2
%fx = OpConvertSToF %float %bx
%fy = OpConvertSToF %float %by
%mx = OpFMul %float %fx %float_2
%my = OpFMul %float %fy %float_2
%sx = OpFSub %float %mx %float_1
%sy = OpFSub %float %my %float_1
%pos = OpCompositeConstruct %v4float %sx %sy %float_0 %float_1
OpStore %gl_Position %pos
OpReturn
OpFunctionEnd
)";

// Matches CustomShaderConstants (rexglue-sdk/include/rex/ui/presenter.h) and
// custom_shader.cpp's kGlslPreamble, which hardcodes its push-constant
// block's member offsets starting at 16 (bytes 0-16 are the *vertex* stage's
// rectangle constants in the SDK's own presenter pipeline -- unused here
// since this pipeline's vertex stage takes no push constants at all, but the
// fragment SPIR-V's decorations still expect them to be reserved).
struct PostProcessConstants {
  int32_t reserved_for_vertex_rect[4];
  int32_t output_offset[2];
  float output_size_inv[2];
  float source_size[2];
  float source_size_inv[2];
};
static_assert(sizeof(PostProcessConstants) == 48);

// Host-side stages for tessellated quad-patch draws (see the header comment
// on CreateTessellationHostShaders). Assembled from text via SPIRV-Tools,
// same rationale as kFullscreenTriangleVertexShaderSpirvAsm above.
//
// Vertex stage: one invocation per control point, exports gl_VertexIndex as
// a float at location 0 -- the per-control-point index the TCS gathers.
// Mirrors the D3D12 backend's tessellation_indexed_vs/"XEVERTEXID" scheme.
constexpr char kTessellationControlPointVertexShaderSpirvAsm[] = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %gl_VertexIndex %out_index
OpDecorate %gl_VertexIndex BuiltIn VertexIndex
OpDecorate %out_index Location 0
%void = OpTypeVoid
%voidfn = OpTypeFunction %void
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%ptr_in_int = OpTypePointer Input %int
%ptr_out_float = OpTypePointer Output %float
%gl_VertexIndex = OpVariable %ptr_in_int Input
%out_index = OpVariable %ptr_out_float Output
%main = OpFunction %void None %voidfn
%entry = OpLabel
%vidx = OpLoad %int %gl_VertexIndex
%fidx = OpConvertSToF %float %vidx
OpStore %out_index %fidx
OpReturn
OpFunctionEnd
)";

// Tessellation control stage for 4-control-point quad patches: gathers the 4
// control point indices into the per-patch float4 the SDK's quad-domain TES
// translation reads (SpirvShaderTranslator's xe_in_patch_control_point_indices
// -- patch-decorated, location 0), and sets every tessellation level to the
// factor pushed per draw (VGT_HOS_MAX_TESS_LEVEL + 1.0, matching the real
// backend's tessellation_factor_range_max and the D3D12 discrete_quad_4cp_hs
// hull shader's use of it). All 4 invocations write identical values to the
// patch outputs, which SPIR-V explicitly permits, so no per-invocation
// branching is needed.
constexpr char kTessellationQuadControlShaderSpirvAsm[] = R"(
OpCapability Tessellation
OpMemoryModel Logical GLSL450
OpEntryPoint TessellationControl %main "main" %in_index %out_cp_indices %gl_TessLevelOuter %gl_TessLevelInner
OpExecutionMode %main OutputVertices 4
OpDecorate %in_index Location 0
OpDecorate %out_cp_indices Location 0
OpDecorate %out_cp_indices Patch
OpDecorate %gl_TessLevelOuter BuiltIn TessLevelOuter
OpDecorate %gl_TessLevelOuter Patch
OpDecorate %gl_TessLevelInner BuiltIn TessLevelInner
OpDecorate %gl_TessLevelInner Patch
OpMemberDecorate %pc_struct 0 Offset 0
OpDecorate %pc_struct Block
%void = OpTypeVoid
%voidfn = OpTypeFunction %void
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v4float = OpTypeVector %float 4
%uint_2 = OpConstant %uint 2
%uint_4 = OpConstant %uint 4
%uint_32 = OpConstant %uint 32
%int_0 = OpConstant %int 0
%int_1 = OpConstant %int 1
%int_2 = OpConstant %int 2
%int_3 = OpConstant %int 3
%arr_in = OpTypeArray %float %uint_32
%ptr_in_arr = OpTypePointer Input %arr_in
%ptr_in_float = OpTypePointer Input %float
%in_index = OpVariable %ptr_in_arr Input
%ptr_out_v4 = OpTypePointer Output %v4float
%out_cp_indices = OpVariable %ptr_out_v4 Output
%arr_outer = OpTypeArray %float %uint_4
%ptr_out_arr4 = OpTypePointer Output %arr_outer
%gl_TessLevelOuter = OpVariable %ptr_out_arr4 Output
%arr_inner = OpTypeArray %float %uint_2
%ptr_out_arr2 = OpTypePointer Output %arr_inner
%gl_TessLevelInner = OpVariable %ptr_out_arr2 Output
%pc_struct = OpTypeStruct %float
%ptr_pc = OpTypePointer PushConstant %pc_struct
%pc = OpVariable %ptr_pc PushConstant
%ptr_pc_float = OpTypePointer PushConstant %float
%ptr_out_float = OpTypePointer Output %float
%main = OpFunction %void None %voidfn
%entry = OpLabel
%in0p = OpAccessChain %ptr_in_float %in_index %int_0
%in0 = OpLoad %float %in0p
%in1p = OpAccessChain %ptr_in_float %in_index %int_1
%in1 = OpLoad %float %in1p
%in2p = OpAccessChain %ptr_in_float %in_index %int_2
%in2 = OpLoad %float %in2p
%in3p = OpAccessChain %ptr_in_float %in_index %int_3
%in3 = OpLoad %float %in3p
%cps = OpCompositeConstruct %v4float %in0 %in1 %in2 %in3
OpStore %out_cp_indices %cps
%factorp = OpAccessChain %ptr_pc_float %pc %int_0
%factor = OpLoad %float %factorp
%outer0 = OpAccessChain %ptr_out_float %gl_TessLevelOuter %int_0
OpStore %outer0 %factor
%outer1 = OpAccessChain %ptr_out_float %gl_TessLevelOuter %int_1
OpStore %outer1 %factor
%outer2 = OpAccessChain %ptr_out_float %gl_TessLevelOuter %int_2
OpStore %outer2 %factor
%outer3 = OpAccessChain %ptr_out_float %gl_TessLevelOuter %int_3
OpStore %outer3 %factor
%inner0 = OpAccessChain %ptr_out_float %gl_TessLevelInner %int_0
OpStore %inner0 %factor
%inner1 = OpAccessChain %ptr_out_float %gl_TessLevelInner %int_1
OpStore %inner1 %factor
OpReturn
OpFunctionEnd
)";

// Assembles one of the SPIR-V text constants above into a VkShaderModule,
// VK_NULL_HANDLE on failure (logged).
VkShaderModule AssembleShaderModule(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                    const char* spirv_asm, size_t spirv_asm_length,
                                    const char* debug_name) {
  spv_context spirv_context = spvContextCreate(SPV_ENV_VULKAN_1_0);
  spv_binary assembled = nullptr;
  spv_diagnostic diagnostic = nullptr;
  spv_result_t assemble_result =
      spvTextToBinary(spirv_context, spirv_asm, spirv_asm_length, &assembled, &diagnostic);
  if (assemble_result != SPV_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to assemble {}: {}", debug_name,
                diagnostic ? diagnostic->error : "unknown error");
    if (diagnostic) spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(spirv_context);
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo module_info{};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = assembled->wordCount * sizeof(uint32_t);
  module_info.pCode = assembled->code;
  VkShaderModule module = VK_NULL_HANDLE;
  VkResult module_result = vulkan_device->functions().vkCreateShaderModule(
      vulkan_device->device(), &module_info, nullptr, &module);
  spvBinaryDestroy(assembled);
  spvContextDestroy(spirv_context);
  if (module_result != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create the {} module", debug_name);
    return VK_NULL_HANDLE;
  }
  return module;
}

}  // namespace

bool NativeCommandProcessor::CreateTessellationHostShaders() {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  if (!vulkan_device->properties().tessellationShader) {
    REXGPU_INFO(
        "NativeCommandProcessor: device has no tessellation support -- tessellated draws (the "
        "title-screen smoke overlay) will be skipped");
    return false;
  }
  tess_control_point_vs_ = AssembleShaderModule(
      vulkan_device, kTessellationControlPointVertexShaderSpirvAsm,
      sizeof(kTessellationControlPointVertexShaderSpirvAsm) - 1, "tessellation passthrough VS");
  tess_quad_tcs_ = AssembleShaderModule(vulkan_device, kTessellationQuadControlShaderSpirvAsm,
                                        sizeof(kTessellationQuadControlShaderSpirvAsm) - 1,
                                        "tessellation quad TCS");
  return tess_control_point_vs_ != VK_NULL_HANDLE && tess_quad_tcs_ != VK_NULL_HANDLE;
}

bool NativeCommandProcessor::EnsurePostProcessPipeline() {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  std::string shader_path = rex::cvar::Query<std::string>("post_process_shader_path");
  if (post_process_pipeline_valid_ && shader_path == post_process_shader_compiled_path_) {
    return !post_process_shader_compile_failed_;
  }

  // Path changed (or this is the first call): recompile the fragment shader
  // and, on the very first successful call, build everything else (render
  // pass/layouts/sampler/vertex shader/pipeline), which never needs to
  // change again -- only the fragment module and the pipeline built from it
  // get recreated when the path changes.
  post_process_shader_compiled_path_ = shader_path;
  post_process_shader_compile_failed_ = true;
  if (shader_path.empty()) {
    return false;
  }

  rex::ui::CustomShader::CompileResult compile_result =
      rex::ui::CustomShader::Compile(shader_path, /*compile_hlsl=*/false);
  if (!compile_result.success) {
    REXGPU_ERROR(
        "NativeCommandProcessor: failed to compile post_process_shader_path '{}' for the "
        "gameplay-preview texture: {}",
        shader_path, compile_result.error);
    return false;
  }

  if (post_process_fragment_shader_ != VK_NULL_HANDLE) {
    dfn.vkDestroyShaderModule(device, post_process_fragment_shader_, nullptr);
    post_process_fragment_shader_ = VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo fragment_module_info{};
  fragment_module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  fragment_module_info.codeSize = compile_result.spirv.size() * sizeof(uint32_t);
  fragment_module_info.pCode = compile_result.spirv.data();
  if (dfn.vkCreateShaderModule(device, &fragment_module_info, nullptr,
                               &post_process_fragment_shader_) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process fragment shader module");
    return false;
  }

  if (!post_process_pipeline_valid_) {
    // Vertex shader: assembled once from the fixed SPIR-V text above.
    spv_context spirv_context = spvContextCreate(SPV_ENV_VULKAN_1_0);
    spv_binary assembled = nullptr;
    spv_diagnostic diagnostic = nullptr;
    spv_result_t assemble_result = spvTextToBinary(
        spirv_context, kFullscreenTriangleVertexShaderSpirvAsm,
        sizeof(kFullscreenTriangleVertexShaderSpirvAsm) - 1, &assembled, &diagnostic);
    if (assemble_result != SPV_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to assemble the post-process vertex shader: {}",
                  diagnostic ? diagnostic->error : "unknown error");
      if (diagnostic) spvDiagnosticDestroy(diagnostic);
      spvContextDestroy(spirv_context);
      return false;
    }
    VkShaderModuleCreateInfo vertex_module_info{};
    vertex_module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertex_module_info.codeSize = assembled->wordCount * sizeof(uint32_t);
    vertex_module_info.pCode = assembled->code;
    VkResult vertex_module_result = dfn.vkCreateShaderModule(device, &vertex_module_info, nullptr,
                                                             &post_process_vertex_shader_);
    spvBinaryDestroy(assembled);
    spvContextDestroy(spirv_context);
    if (vertex_module_result != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process vertex shader module");
      return false;
    }

    // Sampler: linear, clamp-to-edge -- matches the SDK's own guest-output
    // paint sampler (see vulkan_presenter.cpp's kSamplerIndexLinearClampToEdge
    // usage), since arbitrary RetroArch-style shaders (blur/CRT curvature/
    // etc.) may rely on hardware-filtered neighbor sampling.
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &post_process_sampler_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process sampler");
      return false;
    }

    // Descriptor set layout: set 0, binding 0 = sampled image, binding 1 =
    // sampler -- matches custom_shader.cpp's kGlslPreamble exactly.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 2;
    set_layout_info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                        &post_process_descriptor_set_layout_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process descriptor set layout");
      return false;
    }

    VkPushConstantRange push_constant_range{};
    push_constant_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(PostProcessConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &post_process_descriptor_set_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                   &post_process_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process pipeline layout");
      return false;
    }

    // Descriptor pool: only ever one set alive at a time (this pass runs
    // synchronously, see ApplyGameplayPreviewPostProcess), reset and
    // reallocated fresh on every call.
    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr,
                                   &post_process_descriptor_pool_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process descriptor pool");
      return false;
    }

    // Render pass: single color attachment, R8G8B8A8_UNORM (matches every
    // uploaded game texture's format), left in SHADER_READ_ONLY_OPTIMAL so
    // the resulting image is immediately sampleable by real draws with no
    // extra barrier.
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    if (dfn.vkCreateRenderPass(device, &render_pass_info, nullptr, &post_process_render_pass_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process render pass");
      return false;
    }

    post_process_pipeline_valid_ = true;
  }

  // (Re)build the pipeline: only the fragment module differs across calls,
  // but simplest to just recreate the whole pipeline object whenever it
  // does (shader path changes are a rare, explicit user action, not a
  // per-frame occurrence).
  if (post_process_pipeline_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipeline(device, post_process_pipeline_, nullptr);
    post_process_pipeline_ = VK_NULL_HANDLE;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = post_process_vertex_shader_;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = post_process_fragment_shader_;

  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  // Fixed viewport/scissor: this pipeline is only ever used for the
  // gameplay-preview texture's known size (see GetOrUploadTexture), so no
  // dynamic state is needed.
  VkViewport viewport{0.0f, 0.0f, float(kGameplayPreviewWidth), float(kGameplayPreviewHeight), 0.0f,
                     1.0f};
  VkRect2D scissor{{0, 0}, {kGameplayPreviewWidth, kGameplayPreviewHeight}};
  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = &viewport;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = &scissor;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend_state{};
  blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend_state.attachmentCount = 1;
  blend_state.pAttachments = &blend_attachment;

  VkGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pColorBlendState = &blend_state;
  pipeline_info.layout = post_process_pipeline_layout_;
  pipeline_info.renderPass = post_process_render_pass_;
  pipeline_info.subpass = 0;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                    &post_process_pipeline_) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process pipeline");
    return false;
  }

  post_process_shader_compile_failed_ = false;
  return true;
}

bool NativeCommandProcessor::ApplyGameplayPreviewPostProcess(VkImageView source_view,
                                                              uint32_t width, uint32_t height,
                                                              VkImage& out_image,
                                                              VkDeviceMemory& out_memory,
                                                              VkImageView& out_view) {
  // Reuses the SDK's own post_process_shader_enabled/post_process_shader_path
  // cvars (normally applied to the whole composited frame). The native
  // renderer calls Presenter::SetCustomPostProcessShaderAppliesToGuestOutput
  // (nocturnerecomp_app.h) to suppress that whole-screen application, so this
  // is the only place the shader ends up applied when this renderer is
  // active -- see docs/native-rendering.md.
  if (!EnsurePostProcessPipeline()) {
    return false;
  }

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkImage filtered_image = VK_NULL_HANDLE;
  VkDeviceMemory filtered_memory = VK_NULL_HANDLE;
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          filtered_image, filtered_memory)) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create the post-processed texture image");
    return false;
  }

  VkImageView filtered_view = VK_NULL_HANDLE;
  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = filtered_image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &filtered_view) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create the post-processed texture view");
    dfn.vkDestroyImage(device, filtered_image, nullptr);
    dfn.vkFreeMemory(device, filtered_memory, nullptr);
    return false;
  }

  VkFramebufferCreateInfo framebuffer_info{};
  framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_info.renderPass = post_process_render_pass_;
  framebuffer_info.attachmentCount = 1;
  framebuffer_info.pAttachments = &filtered_view;
  framebuffer_info.width = width;
  framebuffer_info.height = height;
  framebuffer_info.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create the post-process framebuffer");
    dfn.vkDestroyImageView(device, filtered_view, nullptr);
    dfn.vkDestroyImage(device, filtered_image, nullptr);
    dfn.vkFreeMemory(device, filtered_memory, nullptr);
    return false;
  }

  dfn.vkResetDescriptorPool(device, post_process_descriptor_pool_, 0);
  VkDescriptorSetAllocateInfo set_alloc_info{};
  set_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_alloc_info.descriptorPool = post_process_descriptor_pool_;
  set_alloc_info.descriptorSetCount = 1;
  set_alloc_info.pSetLayouts = &post_process_descriptor_set_layout_;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(device, &set_alloc_info, &descriptor_set) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to allocate the post-process descriptor set");
    dfn.vkDestroyFramebuffer(device, framebuffer, nullptr);
    dfn.vkDestroyImageView(device, filtered_view, nullptr);
    dfn.vkDestroyImage(device, filtered_image, nullptr);
    dfn.vkFreeMemory(device, filtered_memory, nullptr);
    return false;
  }
  VkDescriptorImageInfo image_descriptor{};
  image_descriptor.imageView = source_view;
  image_descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo sampler_descriptor{};
  sampler_descriptor.sampler = post_process_sampler_;
  VkWriteDescriptorSet writes[2]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = descriptor_set;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[0].pImageInfo = &image_descriptor;
  writes[1] = writes[0];
  writes[1].dstBinding = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[1].pImageInfo = &sampler_descriptor;
  dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

  PostProcessConstants constants{};
  constants.output_offset[0] = 0;
  constants.output_offset[1] = 0;
  constants.output_size_inv[0] = 1.0f / float(width);
  constants.output_size_inv[1] = 1.0f / float(height);
  constants.source_size[0] = float(width);
  constants.source_size[1] = float(height);
  constants.source_size_inv[0] = 1.0f / float(width);
  constants.source_size_inv[1] = 1.0f / float(height);

  VkCommandBufferAllocateInfo cmd_alloc_info{};
  cmd_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmd_alloc_info.commandPool = command_pool_;
  cmd_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_alloc_info.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  bool ok = dfn.vkAllocateCommandBuffers(device, &cmd_alloc_info, &cmd) == VK_SUCCESS;
  if (ok) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(cmd, &begin_info);

    VkClearValue clear_value{};
    VkRenderPassBeginInfo render_pass_begin{};
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = post_process_render_pass_;
    render_pass_begin.framebuffer = framebuffer;
    render_pass_begin.renderArea = {{0, 0}, {width, height}};
    render_pass_begin.clearValueCount = 1;
    render_pass_begin.pClearValues = &clear_value;
    dfn.vkCmdBeginRenderPass(cmd, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_process_pipeline_);
    dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_process_pipeline_layout_,
                                0, 1, &descriptor_set, 0, nullptr);
    dfn.vkCmdPushConstants(cmd, post_process_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(constants), &constants);
    dfn.vkCmdDraw(cmd, 3, 1, 0, 0);
    dfn.vkCmdEndRenderPass(cmd);
    dfn.vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    ok = dfn.vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS;
    if (ok) {
      VkSubmitInfo submit_info{};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &cmd;
      rex::ui::vulkan::VulkanDevice::Queue::Acquisition queue =
          vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      ok = dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, fence) == VK_SUCCESS;
      if (ok) {
        constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull;
        ok = dfn.vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs) == VK_SUCCESS;
      }
      dfn.vkDestroyFence(device, fence, nullptr);
    }
    // vkFreeCommandBuffers isn't in this SDK's exposed Vulkan function table
    // (see UploadTexelsAndTransition) -- leaked instead of freed, bounded by
    // how often this specific texture actually re-uploads.
  }

  dfn.vkDestroyFramebuffer(device, framebuffer, nullptr);

  if (!ok) {
    REXGPU_ERROR("NativeCommandProcessor: post-process pass for the gameplay-preview texture failed");
    dfn.vkDestroyImageView(device, filtered_view, nullptr);
    dfn.vkDestroyImage(device, filtered_image, nullptr);
    dfn.vkFreeMemory(device, filtered_memory, nullptr);
    return false;
  }

  out_image = filtered_image;
  out_memory = filtered_memory;
  out_view = filtered_view;
  return true;
}

void NativeCommandProcessor::DestroyPostProcessPipeline() {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  if (post_process_pipeline_ != VK_NULL_HANDLE)
    dfn.vkDestroyPipeline(device, post_process_pipeline_, nullptr);
  if (post_process_pipeline_layout_ != VK_NULL_HANDLE)
    dfn.vkDestroyPipelineLayout(device, post_process_pipeline_layout_, nullptr);
  if (post_process_descriptor_set_layout_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorSetLayout(device, post_process_descriptor_set_layout_, nullptr);
  if (post_process_descriptor_pool_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorPool(device, post_process_descriptor_pool_, nullptr);
  if (post_process_render_pass_ != VK_NULL_HANDLE)
    dfn.vkDestroyRenderPass(device, post_process_render_pass_, nullptr);
  if (post_process_vertex_shader_ != VK_NULL_HANDLE)
    dfn.vkDestroyShaderModule(device, post_process_vertex_shader_, nullptr);
  if (post_process_fragment_shader_ != VK_NULL_HANDLE)
    dfn.vkDestroyShaderModule(device, post_process_fragment_shader_, nullptr);
  if (post_process_sampler_ != VK_NULL_HANDLE)
    dfn.vkDestroySampler(device, post_process_sampler_, nullptr);
}

NativeCommandProcessor::UploadedTexture* NativeCommandProcessor::GetOrUploadTexture(
    uint32_t fetch_constant_index) {
  using rex::graphics::xenos::DataDimension;
  using rex::graphics::xenos::TextureFormat;
  using rex::graphics::TextureReplacement;
  using rex::graphics::TextureReplacementData;

  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch = registers_.GetTextureFetch(fetch_constant_index);
  if (fetch.base_address == 0) {
    return nullptr;
  }

  // Deliberately narrow support (see the header comment on GetOrUploadTexture):
  // single-level, non-stacked 2D textures in a handful of formats --
  // confirmed against dumps/textures/ (real texture dumps from a working
  // xenos run, filenames include the decoded format) that the intro's
  // content is overwhelmingly k_DXT4_5 (block-compressed, usually tiled with
  // packed_mips set) with some uncompressed k_1_5_5_5/k_4_4_4_4/k_8_8_8_8.
  // Tiled and linear layouts are both handled (via
  // texture_util::GetTiledOffset2D for the former), for both the compressed
  // and uncompressed formats -- only dimension/mip-range/stacking and the
  // format allow-list are actually restrictive here. packed_mips is allowed
  // even though mip tail addressing isn't implemented, because with
  // mip_min==mip_max==0 there's no mip level 1+ to address -- packed_mips
  // only changes where *those* would live, not the base level's own pitch-
  // based addressing (see texture_util::util.h's big block comment on base
  // vs. mip addressing).
  bool format_supported = fetch.format == TextureFormat::k_8_8_8_8 ||
                          fetch.format == TextureFormat::k_1_5_5_5 ||
                          fetch.format == TextureFormat::k_4_4_4_4 ||
                          fetch.format == TextureFormat::k_DXT4_5 ||
                          fetch.format == TextureFormat::k_16_16_16_16;
  if (fetch.dimension != DataDimension::k2DOrStacked || fetch.stacked ||
      fetch.mip_min_level != 0 || fetch.mip_max_level != 0 || !format_supported) {
    static uint32_t skipped_logged = 0;
    if (skipped_logged < 20) {
      ++skipped_logged;
      uint32_t stacked = fetch.stacked;
      uint32_t tiled = fetch.tiled;
      uint32_t packed_mips = fetch.packed_mips;
      uint32_t mip_min = fetch.mip_min_level;
      uint32_t mip_max = fetch.mip_max_level;
      REXGPU_INFO(
          "NativeCommandProcessor: skipping texture upload for unsupported fetch constant "
          "(dimension={} stacked={} tiled={} packed_mips={} mip_min={} mip_max={} format={})",
          static_cast<uint32_t>(fetch.dimension), stacked, tiled, packed_mips, mip_min, mip_max,
          static_cast<uint32_t>(fetch.format));
    }
    return nullptr;
  }

  uint64_t key = HashUcode({fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3,
                           fetch.dword_4, fetch.dword_5});
  auto existing = texture_cache_.find(key);
  if (existing != texture_cache_.end()) {
    return &existing->second;
  }

  if (texture_cache_.size() >= kMaxTextureCacheEntries) {
    if (!texture_cache_limit_logged_) {
      texture_cache_limit_logged_ = true;
      REXGPU_ERROR(
          "NativeCommandProcessor: texture cache exceeded {} distinct entries; skipping further "
          "uploads",
          kMaxTextureCacheEntries);
    }
    return nullptr;
  }

  uint32_t width = fetch.size_2d.width + 1;
  uint32_t height = fetch.size_2d.height + 1;
  uint32_t base_address = fetch.base_address << 12;

  const uint8_t* guest_base =
      rex::system::kernel_state()->memory()->TranslatePhysical<const uint8_t*>(base_address);

  // Xenos textures typically use Endian::k8in32 (4-byte formats) or
  // Endian::k8in16 (2-byte formats/DXT blocks) -- swap each texel (or, for
  // DXT, each 16-bit field within the block) to host order before unpacking;
  // kNone is used as-is.
  bool byteswap = fetch.endianness == rex::graphics::xenos::Endian::k8in32 ||
                  fetch.endianness == rex::graphics::xenos::Endian::k8in16;

  std::vector<uint8_t> rgba;
  // Guest-memory byte span this decode actually reads, for
  // InvalidateTextureCacheRange overlap checks -- set below (differs: block-
  // vs. texel-granularity pitch), before either the replacement-lookup or
  // the guest-decode path so both cover the same range.
  uint32_t source_size_bytes;
  uint32_t rgba_width = width;
  uint32_t rgba_height = height;
  if (fetch.format == TextureFormat::k_DXT4_5) {
    uint32_t block_w = (width + 3) / 4;
    uint32_t block_h = (height + 3) / 4;
    uint32_t pitch_blocks = std::max((fetch.pitch * 32u) / 4, block_w);
    source_size_bytes = pitch_blocks * block_h * 16u;
  } else {
    uint32_t bytes_per_texel = fetch.format == TextureFormat::k_8_8_8_8   ? 4
                               : fetch.format == TextureFormat::k_16_16_16_16 ? 8
                                                                               : 2;
    uint32_t pitch_texels = std::max(fetch.pitch * 32u, width);
    source_size_bytes = pitch_texels * height * bytes_per_texel;
  }

  // Mod texture replacement (see InitializeTextureReplacement): hash the raw
  // guest bytes exactly as rex::graphics::TextureReplacement::HashGuestData
  // does for the D3D12/Vulkan xenos backends' own texture cache, so existing
  // <mod_root>/<hash16>.dds|.png mods (and this SDK's own texture-dump
  // output) resolve to the same content hash here -- content-hash-keyed, not
  // fetch-constant-keyed, so it's address/run independent like the real
  // backends' replacement lookup. The byte range hashed must match
  // TextureKey::GetGuestLayout().base.level_data_extent_bytes exactly (the
  // real backends' guest_size for both dumping and hashing, see
  // TextureCache::CommitPreparedTextureLoad) -- that's the exact upper bound
  // of addressed bytes accounting for tiling/pitch alignment, which is *not*
  // the same as source_size_bytes above (a simpler "last row includes full
  // pitch padding" estimate only good enough for this renderer's own
  // cache-invalidation range, not for byte-for-byte hash parity with mods
  // authored against the real backend's dumps).
  const TextureReplacementData* replacement = nullptr;
  if (texture_replacement_) {
    rex::graphics::texture_util::TextureGuestLayout guest_layout =
        rex::graphics::texture_util::GetGuestTextureLayout(
            fetch.dimension, fetch.pitch, width, height, /*depth_or_array_size=*/1, fetch.tiled,
            fetch.format, fetch.packed_mips, /*has_base=*/true, /*max_level=*/0);
    uint32_t hash_size_bytes = guest_layout.base.level_data_extent_bytes;
    const uint8_t* hash_base =
        rex::system::kernel_state()->memory()->TranslatePhysical<const uint8_t*>(base_address);
    uint64_t content_hash = TextureReplacement::HashGuestData(hash_base, hash_size_bytes);
    replacement = texture_replacement_->FindReplacement(content_hash);
    if (!replacement && REXCVAR_GET(texture_dump_enabled)) {
      texture_replacement_->DumpTexture(content_hash, width, height, fetch.pitch,
                                         fetch.tiled != 0, fetch.format, fetch.endianness,
                                         hash_base, hash_size_bytes);
    }
  }

  if (replacement) {
    rgba_width = replacement->width;
    rgba_height = replacement->height;
    rgba = replacement->pixels;
  } else if (fetch.format == TextureFormat::k_DXT4_5) {
    // Block-compressed: address math operates in block units (a 4x4-texel
    // block is the atomic unit for both pitch and tiling), not texel units --
    // see texture_util::GetTiledOffset2D's doc comment on util.h. The pitch
    // field ("texels >> 5") is in *texel* units uniformly across formats, not
    // pre-converted to blocks for compressed ones -- fetch.pitch*32 is a
    // texel-granularity pitch (D3D9's 32-texel alignment), which must be
    // divided by the block width (4) to get the block-granularity pitch
    // GetTiledOffset2D/the linear stride math below actually need. Missing
    // this /4 was a real, confirmed bug (not a guess): a garbled horizontal
    // noise band in tiled DXT textures whose top/bottom rows still looked
    // correct (a periodic corruption pattern -- the wrong pitch, 896 blocks
    // instead of the correct 224, was an exact 4x multiple, which
    // GetTiledOffset2D's tile-periodic addressing partially aliases back to
    // correct-looking output for some row ranges and not others, rather than
    // a uniform diagonal shear a plain linear-addressing bug would produce).
    uint32_t block_w = (width + 3) / 4;
    uint32_t block_h = (height + 3) / 4;
    constexpr uint32_t kBytesPerBlock = 16;
    constexpr uint32_t kBytesPerBlockLog2 = 4;
    uint32_t pitch_blocks = std::max((fetch.pitch * 32u) / 4, block_w);
    source_size_bytes = pitch_blocks * block_h * kBytesPerBlock;
    rgba.resize(size_t(width) * height * 4);
    for (uint32_t by = 0; by < block_h; ++by) {
      for (uint32_t bx = 0; bx < block_w; ++bx) {
        uint32_t block_offset =
            fetch.tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                              int32_t(bx), int32_t(by), pitch_blocks, kBytesPerBlockLog2))
                        : (by * pitch_blocks + bx) * kBytesPerBlock;
        const uint8_t* src = guest_base + block_offset;
        uint8_t block[16];
        GpuSwapBytes(src, block, 16, fetch.endianness);
        uint8_t decoded[4 * 4 * 4];
        DecompressDXT5Block(block, decoded);
        uint32_t rows = std::min<uint32_t>(4, height - by * 4);
        uint32_t cols = std::min<uint32_t>(4, width - bx * 4);
        for (uint32_t ty = 0; ty < rows; ++ty) {
          uint32_t y = by * 4 + ty;
          for (uint32_t tx = 0; tx < cols; ++tx) {
            uint32_t x = bx * 4 + tx;
            std::memcpy(&rgba[(size_t(y) * width + x) * 4], &decoded[(ty * 4 + tx) * 4], 4);
          }
        }
      }
    }
  } else {
    // Bytes per texel in guest memory: 4 for k_8_8_8_8, 8 for k_16_16_16_16,
    // 2 for the packed 16-bit formats. Row stride from the fetch constant's
    // pitch (texels >> 5, per the field comment in xenos.h), falling back to
    // a tightly-packed row if pitch somehow decodes smaller than the width
    // itself.
    uint32_t bytes_per_texel = fetch.format == TextureFormat::k_8_8_8_8   ? 4
                               : fetch.format == TextureFormat::k_16_16_16_16 ? 8
                                                                               : 2;
    uint32_t bytes_per_texel_log2 = fetch.format == TextureFormat::k_8_8_8_8   ? 2u
                                    : fetch.format == TextureFormat::k_16_16_16_16 ? 3u
                                                                                    : 1u;
    uint32_t pitch_texels = std::max(fetch.pitch * 32u, width);
    source_size_bytes = pitch_texels * height * bytes_per_texel;
    rgba.resize(size_t(width) * height * 4);

    // Replicates the top bits into the low bits when expanding an n-bit
    // channel to 8 bits, instead of a plain shift (which would clip white to
    // slightly-off-white) -- standard bit-replication expansion.
    auto expand5 = [](uint32_t v) -> uint8_t { return uint8_t((v << 3) | (v >> 2)); };
    auto expand4 = [](uint32_t v) -> uint8_t { return uint8_t((v << 4) | v); };

    for (uint32_t y = 0; y < height; ++y) {
      uint8_t* dst_row = &rgba[size_t(y) * width * 4];
      for (uint32_t x = 0; x < width; ++x) {
        uint32_t texel_offset =
            fetch.tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                              int32_t(x), int32_t(y), pitch_texels, bytes_per_texel_log2))
                        : (y * pitch_texels + x) * bytes_per_texel;
        const uint8_t* src = guest_base + texel_offset;
        uint8_t* dst = dst_row + x * 4;
        if (fetch.format == TextureFormat::k_8_8_8_8) {
          // D3DFMT_A8R8G8B8-style packing (same convention as k_1_5_5_5/
          // k_4_4_4_4 below): bits 31-24=A, 23-16=R, 15-8=G, 7-0=B. A raw
          // memcpy of the byte-swapped word would put the low byte (B) into
          // dst[0] (R) and the high-but-one byte (R) into dst[2] (B) --
          // swapping the R and B channels.
          uint32_t texel = byteswap ? rex::memory::load_and_swap<uint32_t>(src)
                                     : *reinterpret_cast<const uint32_t*>(src);
          dst[0] = uint8_t(texel >> 16);
          dst[1] = uint8_t(texel >> 8);
          dst[2] = uint8_t(texel);
          dst[3] = uint8_t(texel >> 24);
        } else if (fetch.format == TextureFormat::k_16_16_16_16) {
          // Plain RGBA order (same convention as k_8_8_8_8), 4x 16-bit UNORM
          // channels -- truncated to the high 8 bits per channel for this
          // renderer's RGBA8 upload path (same "downsample on CPU, reuse the
          // R8G8B8A8_UNORM upload path" approach as the 16-bit packed
          // formats below), not a rounded/dithered downsample.
          for (int c = 0; c < 4; ++c) {
            uint16_t channel = byteswap ? rex::memory::load_and_swap<uint16_t>(src + c * 2)
                                        : *reinterpret_cast<const uint16_t*>(src + c * 2);
            dst[c] = uint8_t(channel >> 8);
          }
        } else {
          uint16_t texel = byteswap ? rex::memory::load_and_swap<uint16_t>(src)
                                    : *reinterpret_cast<const uint16_t*>(src);
          if (fetch.format == TextureFormat::k_1_5_5_5) {
            // D3DFMT_A1R5G5B5-style packing: bit15=A, 14-10=R, 9-5=G, 4-0=B.
            dst[0] = expand5((texel >> 10) & 0x1F);
            dst[1] = expand5((texel >> 5) & 0x1F);
            dst[2] = expand5(texel & 0x1F);
            dst[3] = (texel & 0x8000) ? 0xFF : 0x00;
          } else {
            // k_4_4_4_4, D3DFMT_A4R4G4B4-style packing: 15-12=A, 11-8=R, 7-4=G, 3-0=B.
            dst[0] = expand4((texel >> 8) & 0xF);
            dst[1] = expand4((texel >> 4) & 0xF);
            dst[2] = expand4(texel & 0xF);
            dst[3] = expand4((texel >> 12) & 0xF);
          }
        }
      }
    }
  }

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  UploadedTexture texture;
  texture.base_address_bytes = base_address;
  texture.size_bytes = source_size_bytes;
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {rgba_width, rgba_height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          texture.image, texture.memory)) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create a texture image ({}x{})", rgba_width,
                rgba_height);
    return nullptr;
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = texture.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &texture.view) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create a texture image view");
    dfn.vkDestroyImage(device, texture.image, nullptr);
    dfn.vkFreeMemory(device, texture.memory, nullptr);
    return nullptr;
  }

  if (!UploadTexelsAndTransition(texture.image, rgba_width, rgba_height, rgba.data())) {
    REXGPU_ERROR("NativeCommandProcessor: failed to upload texture data ({}x{})", rgba_width,
                rgba_height);
    dfn.vkDestroyImageView(device, texture.view, nullptr);
    dfn.vkDestroyImage(device, texture.image, nullptr);
    dfn.vkFreeMemory(device, texture.memory, nullptr);
    return nullptr;
  }

  // Gameplay-preview texture (see UploadedTexture::is_gameplay_preview and
  // kGameplayPreviewWidth/Height's doc comment for how this size was
  // confirmed): sampled with gameplay_preview_sampler_ (nearest) instead of
  // default_sampler_ (bilinear) in TryDraw, and optionally re-filtered with
  // the SDK's own configured post-process shader before caching here. Gated
  // on the same post_process_shader_enabled/post_process_shader_path cvars
  // the SDK uses for its whole-screen effect; the native renderer redirects
  // that effect here instead (see docs/native-rendering.md and
  // ApplyGameplayPreviewPostProcess). Swaps texture.image/memory/view to the
  // filtered result on success; on any failure (feature off, no shader
  // configured, compile error), texture keeps the raw upload from just
  // above, so this can never make an otherwise-working texture disappear.
  texture.is_gameplay_preview =
      width == kGameplayPreviewWidth && height == kGameplayPreviewHeight;
  if (texture.is_gameplay_preview && rex::cvar::Query<bool>("post_process_shader_enabled") &&
      !rex::cvar::Query<std::string>("post_process_shader_path").empty()) {
    VkImage filtered_image;
    VkDeviceMemory filtered_memory;
    VkImageView filtered_view;
    if (ApplyGameplayPreviewPostProcess(texture.view, width, height, filtered_image,
                                        filtered_memory, filtered_view)) {
      dfn.vkDestroyImageView(device, texture.view, nullptr);
      dfn.vkDestroyImage(device, texture.image, nullptr);
      dfn.vkFreeMemory(device, texture.memory, nullptr);
      texture.image = filtered_image;
      texture.memory = filtered_memory;
      texture.view = filtered_view;
    }
  }

  auto [inserted, _] = texture_cache_.emplace(key, texture);
  return &inserted->second;
}

void NativeCommandProcessor::InvalidateTextureCacheRange(uint32_t base, uint32_t size) {
  if (size == 0) {
    return;
  }
  pending_texture_cache_invalidations_.emplace_back(base, size);
}

void NativeCommandProcessor::DestroyTextureCache() {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();
  for (auto& [key, texture] : texture_cache_) {
    if (texture.view != VK_NULL_HANDLE) dfn.vkDestroyImageView(device, texture.view, nullptr);
    if (texture.image != VK_NULL_HANDLE) dfn.vkDestroyImage(device, texture.image, nullptr);
    if (texture.memory != VK_NULL_HANDLE) dfn.vkFreeMemory(device, texture.memory, nullptr);
  }
  texture_cache_.clear();
}

VkSampler NativeCommandProcessor::GetOrCreateSampler(rex::graphics::xenos::ClampMode clamp_x,
                                                     rex::graphics::xenos::ClampMode clamp_y,
                                                     rex::graphics::xenos::ClampMode clamp_z,
                                                     bool nearest) {
  uint32_t key = (uint32_t(clamp_x) << 0) | (uint32_t(clamp_y) << 3) | (uint32_t(clamp_z) << 6) |
                (nearest ? 1u << 9 : 0);
  auto existing = sampler_cache_.find(key);
  if (existing != sampler_cache_.end()) {
    return existing->second;
  }

  // Same approximations as the SDK Vulkan backend: the halfway/border modes
  // Vulkan has no exact equivalent for degrade to the nearest edge-clamping
  // behavior.
  auto to_vk_address_mode = [](rex::graphics::xenos::ClampMode mode) {
    using CM = rex::graphics::xenos::ClampMode;
    switch (mode) {
      case CM::kRepeat:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case CM::kMirroredRepeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case CM::kMirrorClampToEdge:
      case CM::kMirrorClampToHalfway:
      case CM::kMirrorClampToBorder:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case CM::kClampToBorder:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      case CM::kClampToEdge:
      case CM::kClampToHalfway:
      default:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
  };

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  sampler_info.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = to_vk_address_mode(clamp_x);
  sampler_info.addressModeV = to_vk_address_mode(clamp_y);
  sampler_info.addressModeW = to_vk_address_mode(clamp_z);
  sampler_info.maxLod = 0.0f;
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  VkSampler sampler = VK_NULL_HANDLE;
  if (vulkan_device->functions().vkCreateSampler(vulkan_device->device(), &sampler_info, nullptr,
                                                 &sampler) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: failed to create a sampler (clamp=({},{},{}))",
                uint32_t(clamp_x), uint32_t(clamp_y), uint32_t(clamp_z));
    return VK_NULL_HANDLE;
  }
  sampler_cache_.emplace(key, sampler);
  return sampler;
}

VkDescriptorSetLayout NativeCommandProcessor::GetOrCreateTextureSetLayout(uint32_t texture_count,
                                                                          uint32_t sampler_count) {
  uint64_t key = (uint64_t(texture_count) << 32) | sampler_count;
  auto existing = texture_set_layout_cache_.find(key);
  if (existing != texture_set_layout_cache_.end()) {
    return existing->second;
  }

  std::vector<VkDescriptorSetLayoutBinding> bindings;
  bindings.reserve(size_t(texture_count) + sampler_count);
  // Images at [0, texture_count), samplers at [texture_count, +sampler_count)
  // -- matches SpirvShaderTranslator::EmitMain's binding-decoration scheme
  // exactly (rexglue-sdk's spirv_translator.cpp: sampler bindings are
  // decorated at texture_binding_count + i), not something invented here.
  for (uint32_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = i;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    bindings.push_back(binding);
  }
  for (uint32_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = texture_count + i;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    bindings.push_back(binding);
  }

  VkDescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = uint32_t(bindings.size());
  layout_info.pBindings = bindings.empty() ? nullptr : bindings.data();

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (vulkan_device->functions().vkCreateDescriptorSetLayout(
          vulkan_device->device(), &layout_info, nullptr, &layout) != VK_SUCCESS) {
    REXGPU_ERROR(
        "NativeCommandProcessor: failed to create a ({}, {}) texture descriptor set layout",
        texture_count, sampler_count);
    return VK_NULL_HANDLE;
  }
  texture_set_layout_cache_.emplace(key, layout);
  return layout;
}

VkPipelineLayout NativeCommandProcessor::GetOrCreatePipelineLayout(uint32_t vertex_texture_count,
                                                                    uint32_t vertex_sampler_count,
                                                                    uint32_t pixel_texture_count,
                                                                    uint32_t pixel_sampler_count) {
  uint64_t key = (uint64_t(vertex_texture_count) << 48) | (uint64_t(vertex_sampler_count) << 32) |
                (uint64_t(pixel_texture_count) << 16) | uint64_t(pixel_sampler_count);
  auto existing = pipeline_layout_cache_.find(key);
  if (existing != pipeline_layout_cache_.end()) {
    return existing->second;
  }

  VkDescriptorSetLayout vertex_texture_layout =
      GetOrCreateTextureSetLayout(vertex_texture_count, vertex_sampler_count);
  VkDescriptorSetLayout pixel_texture_layout =
      GetOrCreateTextureSetLayout(pixel_texture_count, pixel_sampler_count);
  if (vertex_texture_layout == VK_NULL_HANDLE || pixel_texture_layout == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }

  VkDescriptorSetLayout set_layouts[4] = {shared_memory_layout_, constants_layout_,
                                          vertex_texture_layout, pixel_texture_layout};
  // Tessellation factor for tessellated quad-patch draws, read by the fixed
  // quad TCS (see CreateTessellationHostShaders) and pushed per draw in
  // TryDraw. Declared on every layout (harmless for pipelines without
  // tessellation stages) so tessellated and plain pipelines stay
  // layout-compatible and share this cache.
  VkPushConstantRange push_constant_range{};
  push_constant_range.stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = sizeof(float);
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 4;
  layout_info.pSetLayouts = set_layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_constant_range;

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (vulkan_device->functions().vkCreatePipelineLayout(vulkan_device->device(), &layout_info,
                                                         nullptr, &layout) != VK_SUCCESS) {
    REXGPU_ERROR(
        "NativeCommandProcessor: failed to create a pipeline layout for texture counts "
        "vs=({},{}) ps=({},{})",
        vertex_texture_count, vertex_sampler_count, pixel_texture_count, pixel_sampler_count);
    return VK_NULL_HANDLE;
  }
  pipeline_layout_cache_.emplace(key, layout);
  return layout;
}

NativeCommandProcessor::PipelineEntry NativeCommandProcessor::GetOrCreatePipeline(
    TranslatedShader* vertex_shader, TranslatedShader* pixel_shader, VkPrimitiveTopology topology,
    bool primitive_restart_enable, uint32_t blend_control, uint32_t color_write_mask,
    bool tessellated_quad) {
  uint64_t key = reinterpret_cast<uintptr_t>(vertex_shader) ^
                (reinterpret_cast<uintptr_t>(pixel_shader) * 3) ^ (uint64_t(topology) << 1) ^
                (primitive_restart_enable ? 0x8000000000000000ull : 0) ^
                (tessellated_quad ? 0x4000000000000000ull : 0) ^
                (uint64_t(blend_control) << 20) ^ (uint64_t(color_write_mask) << 8);
  auto existing = pipeline_cache_.find(key);
  if (existing != pipeline_cache_.end()) {
    return existing->second;
  }

  // Must read the post-translation, dedup'd, binding-index-ordered lists
  // (SpirvShader::GetTextureBindingsAfterTranslation/
  // GetSamplerBindingsAfterTranslation), not base Shader::texture_bindings()
  // -- see GetOrAnalyzeShader's doc comment. Both shaders were already
  // translated by GetOrTranslateShader before this is called, so these are
  // populated. dynamic_cast can't fail here (GetOrAnalyzeShader always
  // constructs a SpirvShader) but is used instead of static_cast to fail
  // loudly (null-deref) rather than silently if that ever changes.
  auto* vertex_spirv_shader = dynamic_cast<rex::graphics::SpirvShader*>(vertex_shader->shader);
  auto* pixel_spirv_shader = dynamic_cast<rex::graphics::SpirvShader*>(pixel_shader->shader);
  uint32_t vertex_texture_count =
      uint32_t(vertex_spirv_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t vertex_sampler_count =
      uint32_t(vertex_spirv_shader->GetSamplerBindingsAfterTranslation().size());
  uint32_t pixel_texture_count =
      uint32_t(pixel_spirv_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t pixel_sampler_count =
      uint32_t(pixel_spirv_shader->GetSamplerBindingsAfterTranslation().size());
  // Safety cap, same rationale as kMaxShaderCacheEntries/kMaxTextureCacheEntries
  // -- a garbage-decoded shader could plausibly analyze to an implausible texture count;
  // bound the resulting descriptor set/pool/layout growth instead of trusting
  // guest-derived counts unconditionally.
  if (vertex_texture_count > kMaxTexturesPerStage || vertex_sampler_count > kMaxTexturesPerStage ||
      pixel_texture_count > kMaxTexturesPerStage || pixel_sampler_count > kMaxTexturesPerStage) {
    REXGPU_ERROR(
        "NativeCommandProcessor: shader pair needs more than {} textures/samplers in one stage "
        "(vs=({},{}) ps=({},{})); skipping draw",
        kMaxTexturesPerStage, vertex_texture_count, vertex_sampler_count, pixel_texture_count,
        pixel_sampler_count);
    return {};
  }
  VkPipelineLayout layout = GetOrCreatePipelineLayout(vertex_texture_count, vertex_sampler_count,
                                                      pixel_texture_count, pixel_sampler_count);
  if (layout == VK_NULL_HANDLE) {
    return {};
  }

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  // Plain draws: [VS, FS]. Tessellated quad-patch draws: [passthrough VS,
  // quad TCS, guest shader as TES, FS] -- see CreateTessellationHostShaders.
  VkPipelineShaderStageCreateInfo stages[4]{};
  uint32_t stage_count;
  if (tessellated_quad) {
    if (tess_control_point_vs_ == VK_NULL_HANDLE || tess_quad_tcs_ == VK_NULL_HANDLE) {
      return {};
    }
    stage_count = 4;
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = tess_control_point_vs_;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    stages[1].module = tess_quad_tcs_;
    stages[2] = stages[0];
    stages[2].stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    stages[2].module = vertex_shader->module;
    stages[3] = stages[0];
    stages[3].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[3].module = pixel_shader->module;
  } else {
    stage_count = 2;
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader->module;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pixel_shader->module;
  }

  // No classic vertex input state: translated shaders read vertex data via
  // raw shared-memory (SSBO) loads computed from the guest vertex fetch
  // constants, not VkVertexInputAttributeDescription bindings.
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = topology;
  input_assembly.primitiveRestartEnable = primitive_restart_enable ? VK_TRUE : VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // No culling: guest winding convention isn't verified yet, and getting it
  // wrong would silently drop every triangle instead of just looking wrong.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  // Depth clamp instead of the default Vulkan behavior of clipping primitives
  // whose Z falls outside [0,1] after the W-divide: real Xenos GPUs don't
  // truly clip like that either (see draw_util::GetHostViewportInfo's
  // comments on WARP vs. real hardware), and our screen-space "clip
  // disabled" system-constants path (see TryDraw) doesn't attempt to
  // constrain Z into a valid range at all -- without this, a Z value outside
  // [0,1] silently discards the whole primitive instead of just drawing with
  // an out-of-range depth, which was indistinguishable from "nothing
  // rendered" in earlier testing. Matches the real Vulkan backend's own
  // condition (pipeline_cache.cpp: depthClamp feature available && guest
  // clip_disable) rather than always requesting it.
  raster.depthClampEnable = (provider_->vulkan_device()->properties().depthClamp &&
                            registers_.Get<rex::graphics::reg::PA_CL_CLIP_CNTL>().clip_disable)
                               ? VK_TRUE
                               : VK_FALSE;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Real RB_BLENDCONTROL0-driven blend state, replacing what used to be a
  // hardcoded zero-initialized (i.e. blendEnable=false, opaque overwrite)
  // attachment -- found via RenderDoc pixel_history to be the root cause of
  // "black squares" visible over the intro: draws meant to blend
  // translucently (e.g. fades) were instead unconditionally overwriting the
  // framebuffer with their raw shader output regardless of alpha, since
  // nothing ever read the guest's actual blend registers.
  rex::graphics::reg::RB_BLENDCONTROL rb_blendcontrol0;
  rb_blendcontrol0.value = blend_control;
  VkBlendFactor src_color = ToVkBlendFactor(rb_blendcontrol0.color_srcblend);
  VkBlendFactor dst_color = ToVkBlendFactor(rb_blendcontrol0.color_destblend);
  VkBlendOp color_op = ToVkBlendOp(rb_blendcontrol0.color_comb_fcn);
  VkBlendFactor src_alpha = ToVkBlendFactor(rb_blendcontrol0.alpha_srcblend);
  VkBlendFactor dst_alpha = ToVkBlendFactor(rb_blendcontrol0.alpha_destblend);
  VkBlendOp alpha_op = ToVkBlendOp(rb_blendcontrol0.alpha_comb_fcn);
  bool is_identity_blend = src_color == VK_BLEND_FACTOR_ONE && dst_color == VK_BLEND_FACTOR_ZERO &&
                           color_op == VK_BLEND_OP_ADD && src_alpha == VK_BLEND_FACTOR_ONE &&
                           dst_alpha == VK_BLEND_FACTOR_ZERO && alpha_op == VK_BLEND_OP_ADD;

  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VkColorComponentFlags(color_write_mask);
  blend_attachment.blendEnable =
      (color_write_mask != 0 && !is_identity_blend) ? VK_TRUE : VK_FALSE;
  blend_attachment.srcColorBlendFactor = src_color;
  blend_attachment.dstColorBlendFactor = dst_color;
  blend_attachment.colorBlendOp = color_op;
  blend_attachment.srcAlphaBlendFactor = src_alpha;
  blend_attachment.dstAlphaBlendFactor = dst_alpha;
  blend_attachment.alphaBlendOp = alpha_op;
  VkPipelineColorBlendStateCreateInfo blend_state{};
  blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend_state.attachmentCount = 1;
  blend_state.pAttachments = &blend_attachment;

  VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state{};
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.dynamicStateCount = 2;
  dynamic_state.pDynamicStates = dynamic_states;

  VkPipelineTessellationStateCreateInfo tessellation_state{};
  tessellation_state.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
  tessellation_state.patchControlPoints = 4;

  VkGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.stageCount = stage_count;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pTessellationState = tessellated_quad ? &tessellation_state : nullptr;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pColorBlendState = &blend_state;
  pipeline_info.pDynamicState = &dynamic_state;
  pipeline_info.layout = layout;
  pipeline_info.renderPass = render_pass_;
  pipeline_info.subpass = 0;

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                  nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: vkCreateGraphicsPipelines failed ({})",
                static_cast<int32_t>(result));
    pipeline = VK_NULL_HANDLE;
  } else {
    REXGPU_INFO(
        "NativeCommandProcessor: created a graphics pipeline (topology={} tessellated={} "
        "vs_tex=({},{}) ps_tex=({},{}))",
        static_cast<uint32_t>(topology), tessellated_quad, vertex_texture_count,
        vertex_sampler_count, pixel_texture_count, pixel_sampler_count);
  }
  PipelineEntry entry{pipeline, layout};
  pipeline_cache_.emplace(key, entry);
  return entry;
}

void NativeCommandProcessor::UpdateSharedMemory(uint32_t guest_address_dwords,
                                                uint32_t size_dwords) {
  if (size_dwords == 0) {
    return;
  }
  uint64_t byte_offset = uint64_t(guest_address_dwords) * 4;
  uint64_t byte_size = uint64_t(size_dwords) * 4;
  if (byte_offset + byte_size > kSharedMemorySize) {
    REXGPU_ERROR(
        "NativeCommandProcessor: shared memory range out of bounds (offset={:08X} size={})",
        byte_offset, byte_size);
    return;
  }
  const void* guest_ptr = rex::system::kernel_state()->memory()->TranslatePhysical<const void*>(
      uint32_t(byte_offset));
  std::memcpy(shared_memory_mapped_ + byte_offset, guest_ptr, byte_size);
  rex::ui::vulkan::util::FlushMappedMemoryRange(provider_->vulkan_device(), shared_memory_memory_,
                                                shared_memory_memory_type_, byte_offset,
                                                kSharedMemorySize, byte_size);
}

void NativeCommandProcessor::TryResolveCopy() {
  using namespace rex::graphics;

  auto rb_copy_control = registers_.Get<reg::RB_COPY_CONTROL>();
  // Narrow scope, matching this file's established "support the common case,
  // skip+log the rest" pattern: only a plain color copy (not a clear-only
  // resolve, not a depth copy -- copy_src_select >= 4 means depth per
  // RB_COPY_CONTROL's own field comment).
  if (rb_copy_control.color_clear_enable || rb_copy_control.depth_clear_enable ||
      rb_copy_control.copy_src_select >= 4) {
    return;
  }
  if (rb_copy_control.copy_command != xenos::CopyCommand::kRaw &&
      rb_copy_control.copy_command != xenos::CopyCommand::kConvert) {
    return;
  }

  // The resolve rectangle isn't in a dedicated register -- per real
  // hardware/D3D9 (and rexglue-sdk's GetResolveInfo, draw.cpp), a resolve is
  // issued as a "draw" whose vf0 vertices (always CPU-written) define the
  // covered rectangle. Register-only computation (no TextureCache
  // dependency), so safe to reimplement narrowly here.
  xenos::xe_gpu_vertex_fetch_t fetch = registers_.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    return;
  }
  const float* vertices_guest = rex::system::kernel_state()->memory()->TranslatePhysical<const float*>(
      fetch.address * 4);
  if (!vertices_guest) {
    return;
  }
  float half_pixel_offset =
      registers_.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f
                                                                                        : 0.0f;
  int32_t vf[6];
  for (int i = 0; i < 6; ++i) {
    vf[i] = rex::ui::FloatToD3D11Fixed16p8(xenos::GpuSwap(vertices_guest[i], fetch.endian) +
                                           half_pixel_offset);
  }
  int32_t x0 = std::min({vf[0], vf[2], vf[4]});
  int32_t y0 = std::min({vf[1], vf[3], vf[5]});
  int32_t x1 = std::max({vf[0], vf[2], vf[4]});
  int32_t y1 = std::max({vf[1], vf[3], vf[5]});
  // Top-left rasterization rule: include .5, exclude .5 on the bottom-right.
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;

  if (registers_.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    auto offset = registers_.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += offset.window_x_offset;
    y0 += offset.window_y_offset;
    x1 += offset.window_x_offset;
    y1 += offset.window_y_offset;
  }

  int32_t scissor_x0, scissor_y0, scissor_x1, scissor_y1;
  GetScissorRect(registers_, scissor_x0, scissor_y0, scissor_x1, scissor_y1);
  x0 = std::clamp(x0, scissor_x0, scissor_x1);
  y0 = std::clamp(y0, scissor_y0, scissor_y1);
  x1 = std::clamp(x1, scissor_x0, scissor_x1);
  y1 = std::clamp(y1, scissor_y0, scissor_y1);

  // Also clamp to the color target's own bounds -- this renderer's color
  // target is a fixed color_target_width_ x color_target_height_ buffer, not a
  // real EDRAM surface sized by RB_SURFACE_INFO, so a guest-specified
  // rectangle extending past it would read out of bounds below.
  x0 = std::clamp(x0, 0, int32_t(color_target_width_));
  x1 = std::clamp(x1, 0, int32_t(color_target_width_));
  y0 = std::clamp(y0, 0, int32_t(color_target_height_));
  y1 = std::clamp(y1, 0, int32_t(color_target_height_));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }

  auto copy_dest_info = registers_.Get<reg::RB_COPY_DEST_INFO>();
  // Same allow-list as GetOrUploadTexture -- this is the direction guest
  // textures are actually sampled back as later, so supporting the same
  // format covers the same real content (confirmed: the gameplay-preview
  // texture this exists for is k_8_8_8_8).
  if (copy_dest_info.copy_dest_format != xenos::ColorFormat::k_8_8_8_8 ||
      copy_dest_info.copy_dest_array) {
    static uint32_t skipped_logged = 0;
    if (skipped_logged < 20) {
      ++skipped_logged;
      REXGPU_INFO(
          "NativeCommandProcessor: skipping EDRAM resolve copy with unsupported dest "
          "format={} array={}",
          uint32_t(copy_dest_info.copy_dest_format), uint32_t(copy_dest_info.copy_dest_array));
    }
    return;
  }

  auto copy_dest_pitch = registers_.Get<reg::RB_COPY_DEST_PITCH>();
  uint32_t dest_base = registers_[XE_GPU_REG_RB_COPY_DEST_BASE];
  if (!dest_base) {
    return;
  }
  uint32_t width = uint32_t(x1 - x0);
  uint32_t height = uint32_t(y1 - y0);
  uint32_t pitch_texels = std::max(copy_dest_pitch.copy_dest_pitch, width);

  // Read back color_target_image_'s current content -- this must happen now,
  // synchronously, since the resolve needs the render target's contents as
  // of exactly this point in the guest's command stream (subsequent draws
  // this same guest frame may render over the same pixels for a different
  // purpose). Ends the render pass (color_target_image_'s finalLayout is
  // already TRANSFER_SRC_OPTIMAL, matching PresentFrame's own blit) and
  // submits + waits on submit_fence_, then reopens recording via
  // render_pass_continue_ (loadOp=LOAD) so draws after this resolve in the
  // same guest frame still land in the same buffer.
  EnsureFrameBegun();

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  dfn.vkCmdEndRenderPass(command_buffer_);

  VkBuffer readback_buffer = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, color_target_staging_size_, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kReadback, readback_buffer, readback_memory)) {
    REXGPU_ERROR("NativeCommandProcessor: resolve copy failed to allocate a readback buffer");
    return;
  }

  VkBufferImageCopy buffer_image_copy{};
  buffer_image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  buffer_image_copy.imageExtent = {color_target_width_, color_target_height_, 1};
  dfn.vkCmdCopyImageToBuffer(command_buffer_, color_target_image_,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                             &buffer_image_copy);
  dfn.vkEndCommandBuffer(command_buffer_);

  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer_;
  rex::ui::vulkan::VulkanDevice::Queue::Acquisition queue =
      vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
  dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, submit_fence_);
  constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull;
  if (dfn.vkWaitForFences(device, 1, &submit_fence_, VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: timed out waiting for the resolve copy's fence");
  }
  dfn.vkResetFences(device, 1, &submit_fence_);

  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, readback_memory, 0, color_target_staging_size_, 0, &mapped) ==
      VK_SUCCESS) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = readback_memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);

    // color_target_image_ is VK_FORMAT_A2B10G10R10_UNORM_PACK32 -- convert
    // to RGBA8 (this narrow implementation's only supported dest format) on
    // the CPU, same "unpack on CPU, reuse the simple uncompressed-format
    // write path" approach GetOrUploadTexture uses for its packed 16-bit
    // formats.
    const uint32_t* src_pixels = reinterpret_cast<const uint32_t*>(mapped);
    uint8_t* guest_base = rex::system::kernel_state()->memory()->TranslatePhysical<uint8_t*>(dest_base);
    bool byteswap = copy_dest_info.copy_dest_endian != xenos::Endian128::kNone;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        uint32_t src_x = x0 + x;
        uint32_t src_y = y0 + y;
        uint32_t packed = src_pixels[src_y * color_target_width_ + src_x];
        uint8_t rgba[4];
        // A2B10G10R10_UNORM_PACK32: R in bits 0-9, G in 10-19, B in 20-29,
        // A in 30-31 -- downsampled to 8 bits/channel (truncated, not
        // rounded/dithered, matching this file's other 16-to-8-bit
        // downsamples).
        rgba[0] = uint8_t((packed >> 2) & 0xFF);
        rgba[1] = uint8_t((packed >> 12) & 0xFF);
        rgba[2] = uint8_t((packed >> 22) & 0xFF);
        rgba[3] = uint8_t(((packed >> 30) & 0x3) * 85);
        if (byteswap) {
          std::swap(rgba[0], rgba[3]);
          std::swap(rgba[1], rgba[2]);
        }
        uint32_t dest_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(x), int32_t(y), pitch_texels, /*bytes_per_texel_log2=*/2u));
        std::memcpy(guest_base + dest_offset, rgba, 4);
      }
    }
    dfn.vkUnmapMemory(device, readback_memory);
  }

  dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);

  // Invalidate any cached texture upload whose decode source overlaps what
  // was just written -- same staleness problem the COHER_STATUS_HOST
  // handler in OnPacket fixes generally, kept here too as defense in depth.
  uint32_t written_size_bytes = 0;
  for (uint32_t y = 0; y < height; ++y) {
    written_size_bytes = std::max(
        written_size_bytes,
        uint32_t(rex::graphics::texture_util::GetTiledOffset2D(int32_t(width - 1), int32_t(y),
                                                                pitch_texels, 2u)) +
            4);
  }
  InvalidateTextureCacheRange(dest_base, written_size_bytes);

  // Reopen recording so subsequent draws in this same guest frame still
  // land in color_target_image_ -- see render_pass_continue_'s doc comment.
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(command_buffer_, &begin_info);

  VkRenderPassBeginInfo rp_begin{};
  rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp_begin.renderPass = render_pass_continue_;
  rp_begin.framebuffer = framebuffer_;
  rp_begin.renderArea.extent = {color_target_width_, color_target_height_};
  dfn.vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
}

void NativeCommandProcessor::EnsureFrameBegun() {
  if (frame_active_) {
    return;
  }

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  // Waiting here (rather than in PresentFrame) is what makes it safe to
  // free last frame's transient buffers/descriptor sets right after: by the
  // time this returns, the GPU is done with everything submitted last frame.
  constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull;
  if (dfn.vkWaitForFences(device, 1, &submit_fence_, VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: timed out waiting for the previous frame's fence");
  }
  dfn.vkResetFences(device, 1, &submit_fence_);

  // Now safe to actually destroy anything queued by InvalidateTextureCacheRange
  // since the fence wait above -- see pending_texture_cache_invalidations_'s
  // doc comment.
  if (!pending_texture_cache_invalidations_.empty()) {
    for (const auto& [inv_base, inv_size] : pending_texture_cache_invalidations_) {
      uint32_t inv_end = inv_base + inv_size;
      for (auto it = texture_cache_.begin(); it != texture_cache_.end();) {
        uint32_t tex_start = it->second.base_address_bytes;
        uint32_t tex_end = tex_start + it->second.size_bytes;
        if (tex_start < inv_end && inv_base < tex_end) {
          if (it->second.view != VK_NULL_HANDLE) dfn.vkDestroyImageView(device, it->second.view, nullptr);
          if (it->second.image != VK_NULL_HANDLE) dfn.vkDestroyImage(device, it->second.image, nullptr);
          if (it->second.memory != VK_NULL_HANDLE) dfn.vkFreeMemory(device, it->second.memory, nullptr);
          it = texture_cache_.erase(it);
        } else {
          ++it;
        }
      }
    }
    pending_texture_cache_invalidations_.clear();
  }

  FreeTransientBuffers();
  dfn.vkResetDescriptorPool(device, transient_descriptor_pool_, 0);

  // The fence wait above guarantees the GPU has finished reading last frame's
  // constants, so it's now safe to reuse the arena from the top.
  constants_arena_offset_ = 0;

  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(command_buffer_, &begin_info);

  // Same blue as milestone 3a's clear -- the visible result when a frame
  // has zero (or all-skipped) draws.
  VkClearValue clear_value{};
  clear_value.color = {{0.05f, 0.05f, 0.35f, 1.0f}};
  VkRenderPassBeginInfo rp_begin{};
  rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp_begin.renderPass = render_pass_;
  rp_begin.framebuffer = framebuffer_;
  rp_begin.renderArea = {{0, 0}, {color_target_width_, color_target_height_}};
  rp_begin.clearValueCount = 1;
  rp_begin.pClearValues = &clear_value;
  dfn.vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{0.0f,
                      0.0f,
                      float(color_target_width_),
                      float(color_target_height_),
                      0.0f,
                      1.0f};
  VkRect2D scissor{{0, 0}, {color_target_width_, color_target_height_}};
  dfn.vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
  dfn.vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

  frame_active_ = true;
  frame_has_draws_ = false;
}

void NativeCommandProcessor::TryDraw(rex::graphics::xenos::PrimitiveType prim_type,
                                     uint32_t num_indices) {
  if (!pipeline_resources_valid_) {
    return;
  }

  // EDRAM resolve dispatch: on real hardware, RB_MODECONTROL.edram_mode ==
  // kCopy turns what would otherwise be a normal draw into a resolve
  // (render-target -> guest-texture copy) instead -- see TryResolveCopy's
  // doc comment. Checked before the num_indices==0 early-out below since a
  // resolve "draw" legitimately has a tiny (3-vertex) index count that
  // isn't zero, but isn't a real draw either.
  if (registers_.Get<rex::graphics::reg::RB_MODECONTROL>().edram_mode ==
      rex::graphics::xenos::EdramMode::kCopy) {
    TryResolveCopy();
    return;
  }

  if (num_indices == 0) {
    return;
  }

  // Fetch-constant/register decode is known-unreliable for some draws in this
  // stream -- a garbage-decoded num_indices was observed making the quad-list index
  // buffer below balloon to hundreds of MB and stall the process. No real
  // draw in this game's UI/intro content needs anywhere near this many
  // vertices, so treat an implausibly large count as corrupt and skip it.
  constexpr uint32_t kMaxPlausibleIndices = 65536;
  if (num_indices > kMaxPlausibleIndices) {
    static uint32_t skipped_logged = 0;
    if (skipped_logged < 20) {
      ++skipped_logged;
      REXGPU_INFO(
          "NativeCommandProcessor: skipping draw with implausible num_indices={} (likely a "
          "garbage-decoded packet)",
          num_indices);
    }
    return;
  }

  // kQuadList (4 vertices/quad, no native Vulkan primitive) is drawn as two
  // triangles per quad via a host-synthesized index buffer -- built once
  // num_indices is known, below.
  bool is_quad_list = prim_type == rex::graphics::xenos::PrimitiveType::kQuadList;
  uint32_t quad_count = is_quad_list ? num_indices / 4 : 0;
  if (is_quad_list && quad_count == 0) {
    return;
  }

  // kRectangleList (3 guest vertices/rectangle, not native either) is drawn
  // as a triangle strip with primitive restart, matching the SDK's own
  // Vulkan backend (PrimitiveProcessor::InitializeCache /
  // Shader::HostVertexShaderType::kRectangleListAsTriangleStrip): the
  // translated vertex shader itself reconstructs the missing 4th corner as
  // v0+v2-v1 from a synthetic gl_VertexIndex this step feeds it via the
  // index buffer built below, so unlike kQuadList this needs a distinct
  // shader translation (host_vertex_shader_type), not just host-side index
  // remapping.
  bool is_rect_list = prim_type == rex::graphics::xenos::PrimitiveType::kRectangleList;
  uint32_t rect_count = is_rect_list ? num_indices / 3 : 0;
  if (is_rect_list && rect_count == 0) {
    return;
  }

  // Hardware tessellation (found via the title-screen smoke overlay, which
  // never rendered): when an explicit-major-mode draw has VGT_OUTPUT_PATH_CNTL
  // selecting kTessellationEnable, the "vertex shader" is really a *domain*
  // shader -- for a quad list, each group of 4 vertices is one 4-control-point
  // quad patch, and the guest shader expects r0.xy = tess coords and
  // r0.z/r1.xyz = the 4 control point indices (kQuadDomainCPIndexed), not a
  // plain vertex index in r0.x. Running it as a normal vertex shader
  // deterministically collapses every vertex onto control point 0 (all those
  // registers read 0) and rasterizes nothing. Matches the SDK
  // PrimitiveProcessor's tessellation detection (primitive_processor.cpp) and
  // the D3D12 backend's discrete/continuous quad 4cp hull shader path, which
  // is the configuration this game's smoke draw actually uses (verified in a
  // D3D12 xenos capture: HS+DS bound, 4-CP patch topology, ~600 tessellated
  // vertices). Adaptive tessellation (per-edge factors in the index buffer)
  // and non-quad patch types aren't used by observed content and fall through
  // to the old path with a rate-limited log.
  bool tessellation_enabled =
      registers_.Get<rex::graphics::reg::VGT_DRAW_INITIATOR>().major_mode ==
          rex::graphics::xenos::MajorMode::kExplicit &&
      registers_.Get<rex::graphics::reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          rex::graphics::xenos::VGTOutputPath::kTessellationEnable;
  rex::graphics::xenos::TessellationMode tessellation_mode =
      registers_.Get<rex::graphics::reg::VGT_HOS_CNTL>().tess_mode;
  bool is_tessellated_quad =
      tessellation_enabled && is_quad_list &&
      (tessellation_mode == rex::graphics::xenos::TessellationMode::kDiscrete ||
       tessellation_mode == rex::graphics::xenos::TessellationMode::kContinuous) &&
      tess_control_point_vs_ != VK_NULL_HANDLE && tess_quad_tcs_ != VK_NULL_HANDLE;
  if (tessellation_enabled && !is_tessellated_quad) {
    static uint32_t unsupported_tess_logged = 0;
    if (unsupported_tess_logged < 20) {
      ++unsupported_tess_logged;
      REXGPU_INFO(
          "NativeCommandProcessor: tessellated draw with unsupported configuration "
          "(prim_type={} tess_mode={} host_shaders_ready={}) -- drawing untessellated",
          static_cast<uint32_t>(prim_type), static_cast<uint32_t>(tessellation_mode),
          tess_control_point_vs_ != VK_NULL_HANDLE && tess_quad_tcs_ != VK_NULL_HANDLE);
    }
  }

  VkPrimitiveTopology topology;
  if (is_tessellated_quad) {
    topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
  } else if (is_quad_list) {
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  } else if (is_rect_list) {
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  } else {
    topology = PrimitiveTypeToVkTopology(prim_type);
  }
  if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) {
    static uint32_t skipped_logged = 0;
    if (skipped_logged < 20) {
      ++skipped_logged;
      REXGPU_INFO("NativeCommandProcessor: skipping draw with unsupported primitive type {}",
                 static_cast<uint32_t>(prim_type));
    }
    return;
  }

  // Analyze both shaders (cheap, cached independently of modification --
  // see GetOrAnalyzeShader) before translating either, since the
  // interpolator_mask each needs to be *translated* with depends on both:
  // real backend formula (rexglue-sdk's command_processor.cpp) is
  // "what the vertex shader actually writes" intersected with "what the
  // pixel shader actually reads" -- getting this right is what makes
  // per-vertex color/UV/etc. data actually reach the pixel shader at all
  // (see GetOrTranslateShader's doc comment).
  rex::graphics::Shader* vertex_shader_analyzed =
      GetOrAnalyzeShader(rex::graphics::xenos::ShaderType::kVertex, active_vertex_shader_.ucode);
  rex::graphics::Shader* pixel_shader_analyzed =
      GetOrAnalyzeShader(rex::graphics::xenos::ShaderType::kPixel, active_pixel_shader_.ucode);
  if (!vertex_shader_analyzed || !pixel_shader_analyzed) {
    return;
  }

  // F2 shader debugger overlay: honor the user's disable toggle (see
  // SetShaderDisabledByHash) by skipping the draw entirely, mirroring the
  // real backend's shader-blacklist behavior closely enough for the toggle
  // to visibly do something. Also records which two shaders this draw
  // bound, for GetShaderSnapshot's "active" flag.
  last_active_vertex_ucode_xxh3_ = vertex_shader_analyzed->ucode_data_hash();
  last_active_pixel_ucode_xxh3_ = pixel_shader_analyzed->ucode_data_hash();
  if (disabled_shader_hashes_.count(last_active_vertex_ucode_xxh3_) ||
      disabled_shader_hashes_.count(last_active_pixel_ucode_xxh3_)) {
    return;
  }

  // F2 profiling (see SetShaderProfilingEnabled): times everything from here
  // to the end of TryDraw (pipeline/descriptor setup + vkCmdDraw*), i.e. the
  // actual per-draw submission cost, not shader translation/analysis above
  // (cached, amortized across many draws). Attributed to both shaders on
  // every return path below via a scope guard, since TryDraw has several
  // early-out returns between here and the real draw call.
  std::chrono::steady_clock::time_point profile_start;
  if (shader_profiling_enabled_) {
    profile_start = std::chrono::steady_clock::now();
  }
  struct ProfileScopeGuard {
    NativeCommandProcessor* self;
    std::chrono::steady_clock::time_point* start;
    ~ProfileScopeGuard() {
      if (!self->shader_profiling_enabled_) {
        return;
      }
      uint64_t ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - *start)
              .count());
      for (uint64_t hash :
           {self->last_active_vertex_ucode_xxh3_, self->last_active_pixel_ucode_xxh3_}) {
        auto& profile = self->shader_profile_[hash];
        profile.total_ns += ns;
        profile.draw_count += 1;
      }
    }
  } profile_scope_guard{this, &profile_start};

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      vertex_shader_analyzed->writes_interpolators() &
      pixel_shader_analyzed->GetInterpolatorInputMask(
          registers_.Get<rex::graphics::reg::SQ_PROGRAM_CNTL>(),
          registers_.Get<rex::graphics::reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);

  TranslatedShader* vertex_shader = GetOrTranslateShader(
      rex::graphics::xenos::ShaderType::kVertex, active_vertex_shader_.ucode, interpolator_mask,
      is_tessellated_quad
          ? rex::graphics::Shader::HostVertexShaderType::kQuadDomainCPIndexed
          : (is_rect_list
                 ? rex::graphics::Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
                 : rex::graphics::Shader::HostVertexShaderType::kVertex),
      is_tessellated_quad ? uint32_t(tessellation_mode) : 0);
  TranslatedShader* pixel_shader = GetOrTranslateShader(
      rex::graphics::xenos::ShaderType::kPixel, active_pixel_shader_.ucode, interpolator_mask);
  if (!vertex_shader || !pixel_shader || vertex_shader->module == VK_NULL_HANDLE ||
      pixel_shader->module == VK_NULL_HANDLE) {
    return;
  }

  auto rb_color_mask = registers_.Get<rex::graphics::reg::RB_COLOR_MASK>();
  uint32_t color_write_mask = 0;
  if (rb_color_mask.write_red0) color_write_mask |= VK_COLOR_COMPONENT_R_BIT;
  if (rb_color_mask.write_green0) color_write_mask |= VK_COLOR_COMPONENT_G_BIT;
  if (rb_color_mask.write_blue0) color_write_mask |= VK_COLOR_COMPONENT_B_BIT;
  if (rb_color_mask.write_alpha0) color_write_mask |= VK_COLOR_COMPONENT_A_BIT;
  uint32_t blend_control = registers_.Get<rex::graphics::reg::RB_BLENDCONTROL>().value;

  PipelineEntry pipeline_entry =
      GetOrCreatePipeline(vertex_shader, pixel_shader, topology, /*primitive_restart_enable=*/
                         is_rect_list, blend_control, color_write_mask, is_tessellated_quad);
  if (pipeline_entry.pipeline == VK_NULL_HANDLE) {
    return;
  }

  EnsureFrameBegun();

  // Mirror only the vertex fetch constant ranges this shader pair actually
  // reads into shared memory (not a blanket copy) -- see UpdateSharedMemory.
  for (const auto& binding : vertex_shader->shader->vertex_bindings()) {
    rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch =
        registers_.GetVertexFetch(binding.fetch_constant);
    UpdateSharedMemory(fetch.address, fetch.size);

    static uint32_t vfetch_logged = 0;
    if (vfetch_logged < 10) {
      ++vfetch_logged;
      uint32_t address = fetch.address;
      uint32_t size = fetch.size;
      const uint8_t* raw_verts =
          rex::system::kernel_state()->memory()->TranslatePhysical<uint8_t*>(address * 4);
      // Dump the whole declared range (up to 4 vertices' worth), not just the
      // first vertex -- earlier logging only ever sampled dwords 0..7 (vertex
      // 0), which made a shader reading vertex 0 correctly but vertices 1+ as
      // all-zero look identical to "everything's fine" in the log.
      uint32_t dwords_to_dump = std::min(size, 32u);
      float v[32];
      for (uint32_t i = 0; i < dwords_to_dump; ++i) {
        uint32_t dword = rex::memory::load_and_swap<uint32_t>(raw_verts + i * 4);
        std::memcpy(&v[i], &dword, 4);
      }
      std::string dump;
      char num_buf[32];
      for (uint32_t i = 0; i < dwords_to_dump; ++i) {
        std::snprintf(num_buf, sizeof(num_buf), "%.4f", v[i]);
        dump += num_buf;
        if (i + 1 < dwords_to_dump) dump += ',';
      }
      REXGPU_INFO(
          "NativeCommandProcessor: vfetch draw#{} fetch_constant={} address={:08X} size={} "
          "type={} dwords=({})",
          draws_logged_, binding.fetch_constant, address, size, static_cast<uint32_t>(fetch.type),
          dump);
    }
  }

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkDescriptorSetAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc_info.descriptorPool = transient_descriptor_pool_;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &constants_layout_;
  VkDescriptorSet constants_set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(device, &alloc_info, &constants_set) != VK_SUCCESS) {
    REXGPU_ERROR("NativeCommandProcessor: transient descriptor pool exhausted, skipping draw");
    return;
  }

  // Each draw gets fresh, dedicated buffer objects for its 5 constant
  // buffers rather than reusing one buffer across draws: all draws within a
  // frame get *recorded* before any of them actually execute on the GPU (the
  // whole frame submits once, on swap), so reusing+overwriting one buffer
  // between draws would make every draw in the frame read whichever draw's
  // constants were written last, not its own.
  using rex::graphics::SpirvShaderTranslator;
  auto upload_constant_buffer = [&](const void* data, size_t size, uint32_t binding) {
    // Suballocate from the persistently-mapped constants arena by offset,
    // instead of a fresh vkCreateBuffer + dedicated vkAllocateMemory per call
    // (see the constants_arena_* members' doc). The used range is flushed once
    // per frame in PresentFrame; the offset resets once per frame in
    // EnsureFrameBegun after the fence wait confirms the GPU is done reading it.
    VkDeviceSize aligned_offset = (constants_arena_offset_ + constants_arena_alignment_ - 1) &
                                  ~(constants_arena_alignment_ - 1);
    if (aligned_offset + size > kConstantsArenaSize) {
      REXGPU_ERROR("NativeCommandProcessor: constants arena exhausted, skipping a constant buffer");
      return;
    }
    std::memcpy(constants_arena_mapped_ + aligned_offset, data, size);
    constants_arena_offset_ = aligned_offset + size;

    VkDescriptorBufferInfo buffer_info{constants_arena_buffer_, aligned_offset, size};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = constants_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  };

  // System constants: derived from the real PA_CL_VTE_CNTL/PA_CL_CLIP_CNTL/
  // viewport registers instead of a hardcoded identity guess (which left
  // every draw rasterizing nothing -- the guest's actual vtx_xy_fmt/viewport
  // state controls whether vertex shader output is already in NDC/pixel
  // space or needs the W-divide+viewport-scale path, and getting this wrong
  // pushes every vertex outside the clip volume). Replicates the two
  // branches of draw_util::GetHostViewportInfo
  // (rexglue-sdk/src/graphics/util/draw.cpp) at reduced generality (no
  // resolution scaling, no depth-range remapping, no MSAA) -- that function
  // itself isn't callable directly, since draw.cpp also pulls in the
  // plugin-only TextureCache/TraceWriter headers this renderer deliberately
  // doesn't link.
  SpirvShaderTranslator::SystemConstants system_constants{};
  {
    // Every pixel shader unconditionally multiplies its final output color by
    // system_constants.color_exp_bias[rt] (see SpirvShaderTranslator's
    // generated SPIR-V) -- value-initializing SystemConstants left this at
    // 0.0f, silently zeroing every draw's color regardless of texture/vertex
    // data/blending. Found via RenderDoc after the interpolator_mask fix
    // (below) didn't change a black-quad draw's output at all, which should
    // have been impossible if the fix were insufficient by itself -- tracing
    // the SPIR-V further up from the interpolator load found this multiply.
    // Real backend (rexglue-sdk's command_processor.cpp) computes this as
    // 1.0f with RB_COLOR_INFO's 6-bit signed color_exp_bias field added into
    // the float's exponent bits (equivalent to exp2f(color_exp_bias)); only
    // RT0 is populated here since this renderer only has one color target.
    auto rb_color_info = registers_.Get<rex::graphics::reg::RB_COLOR_INFO>();
    system_constants.color_exp_bias[0] = std::exp2(float(rb_color_info.color_exp_bias));
    system_constants.color_exp_bias[1] = 1.0f;
    system_constants.color_exp_bias[2] = 1.0f;
    system_constants.color_exp_bias[3] = 1.0f;

    auto pa_cl_clip_cntl = registers_.Get<rex::graphics::reg::PA_CL_CLIP_CNTL>();
    auto pa_cl_vte_cntl = registers_.Get<rex::graphics::reg::PA_CL_VTE_CNTL>();
    auto pa_su_sc_mode_cntl = registers_.Get<rex::graphics::reg::PA_SU_SC_MODE_CNTL>();
    auto pa_su_vtx_cntl = registers_.Get<rex::graphics::reg::PA_SU_VTX_CNTL>();

    if (pa_cl_vte_cntl.vtx_xy_fmt) {
      system_constants.flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
    }
    if (pa_cl_vte_cntl.vtx_z_fmt) {
      system_constants.flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
    }
    if (pa_cl_vte_cntl.vtx_w0_fmt) {
      system_constants.flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
    }
    if (!is_rect_list) {
      // kQuadList/standard triangle topologies are polygonal; kRectangleList
      // isn't (matches command_processor.cpp's primitive_polygonal check).
      system_constants.flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
    }

    // Alpha test: the pixel shader's alpha-test function field is 3 bits
    // starting at kSysFlag_AlphaPassIfLess_Shift, encoded so 0 means "always
    // fail" -- left unset (0) here, every fragment was silently Kill()ed by
    // the shader's own alpha-test block regardless of guest intent (found via
    // RenderDoc pixel_history: shader_discarded on every draw). Matches
    // command_processor.cpp's rb_colorcontrol.alpha_test_enable handling:
    // kAlways (disabled) unless the guest actually enabled the test.
    auto rb_colorcontrol = registers_.Get<rex::graphics::reg::RB_COLORCONTROL>();
    rex::graphics::xenos::CompareFunction alpha_test_function =
        rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                          : rex::graphics::xenos::CompareFunction::kAlways;
    system_constants.flags |= uint32_t(alpha_test_function)
                              << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
    system_constants.alpha_test_reference =
        registers_.Get<float>(rex::graphics::XE_GPU_REG_RB_ALPHA_REF);

    float scale_xy[2] = {
        pa_cl_vte_cntl.vport_x_scale_ena
            ? registers_.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE)
            : 1.0f,
        pa_cl_vte_cntl.vport_y_scale_ena
            ? registers_.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YSCALE)
            : 1.0f,
    };
    float scale_z = pa_cl_vte_cntl.vport_z_scale_ena
                        ? registers_.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE)
                        : 1.0f;
    float offset_xy[2] = {
        pa_cl_vte_cntl.vport_x_offset_ena
            ? registers_.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XOFFSET)
            : 0.0f,
        pa_cl_vte_cntl.vport_y_offset_ena
            ? registers_.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YOFFSET)
            : 0.0f,
    };
    float offset_z = pa_cl_vte_cntl.vport_z_offset_ena
                         ? registers_.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET)
                         : 0.0f;

    float offset_add_xy[2] = {0.0f, 0.0f};
    if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
      auto pa_sc_window_offset = registers_.Get<rex::graphics::reg::PA_SC_WINDOW_OFFSET>();
      offset_add_xy[0] += float(pa_sc_window_offset.window_x_offset);
      offset_add_xy[1] += float(pa_sc_window_offset.window_y_offset);
    }
    if (pa_su_vtx_cntl.pix_center == rex::graphics::xenos::PixelCenter::kD3DZero) {
      offset_add_xy[0] += 0.5f;
      offset_add_xy[1] += 0.5f;
    }

    if (pa_cl_clip_cntl.clip_disable) {
      // Screen-space draws (the common case for 2D UI/intro content):
      // vertex shader output is treated directly as pixel coordinates, no
      // host clipping -- map [0, extent] pixels to [-1, 1] NDC ourselves.
      //
      // extent here must be the guest's *actual* render-target width
      // (RB_SURFACE_INFO.surface_pitch), not this renderer's fixed
      // color_target_width_/height_: the guest sometimes renders a pass at a
      // smaller internal EDRAM surface (e.g. 640x360 for part of the KONAMI
      // intro, confirmed via RenderDoc: that draw's RB_SURFACE_INFO decoded
      // to surface_pitch=640, vs. 1280 for full-resolution draws elsewhere
      // in the same frame) and expects it to be scaled up to the real
      // display resolution by the EDRAM resolve/scaler -- a step this
      // renderer doesn't implement as a separate pass. Using the guest's own
      // surface size as the pixel-to-NDC extent instead of the fixed canvas
      // size has the same effect: it stretches that pass's screen-space
      // geometry to fill color_target_image_ (whose Vulkan viewport is
      // always the fixed full canvas), rather than only covering the
      // top-left fraction of it that a literal 1:1 pixel mapping would.
      auto rb_surface_info = registers_.Get<rex::graphics::reg::RB_SURFACE_INFO>();
      uint32_t surface_pitch = rb_surface_info.surface_pitch;
      float extent_xy[2] = {
          surface_pitch ? float(surface_pitch) : float(color_target_width_),
          surface_pitch ? float(surface_pitch) * float(color_target_height_) /
                              float(color_target_width_)
                        : float(color_target_height_),
      };
      for (uint32_t i = 0; i < 2; ++i) {
        float extent = extent_xy[i];
        float pixels_to_ndc = 2.0f / extent;
        system_constants.ndc_scale[i] = scale_xy[i] * pixels_to_ndc;
        system_constants.ndc_offset[i] =
            (offset_xy[i] - extent * 0.5f + offset_add_xy[i]) * pixels_to_ndc;
      }
      system_constants.ndc_scale[2] = scale_z;
      system_constants.ndc_offset[2] = offset_z;
    } else {
      // Clipping enabled (regular 3D-style draws): the host viewport is
      // fixed at the full color target, so ndc_scale/offset only need to
      // carry the guest viewport scale/offset themselves (already
      // normalized -1..1 input), no extra pixel remap.
      system_constants.ndc_scale[0] = scale_xy[0];
      system_constants.ndc_scale[1] = scale_xy[1];
      system_constants.ndc_scale[2] = scale_z;
      system_constants.ndc_offset[0] = offset_xy[0];
      system_constants.ndc_offset[1] = offset_xy[1];
      system_constants.ndc_offset[2] = offset_z;
    }
    // Vertex index range: the translated shader UClamps every fetched index
    // into [vertex_index_min, vertex_index_max] (see SpirvShaderTranslator's
    // vertex-fetch prologue) -- left at this struct's zero-initialized
    // default, vertex_index_max=0 clamped every single vertex to index 0,
    // collapsing all vertices of every draw onto the same fetch address
    // (found via RenderDoc: post-VS gl_Position was bit-identical across all
    // vertices of a draw). Matches command_processor.cpp's
    // vgt_min_vtx_indx/vgt_max_vtx_indx handling.
    system_constants.vertex_index_min =
        registers_.Get<uint32_t>(rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX);
    system_constants.vertex_index_max =
        registers_.Get<uint32_t>(rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX);

    static uint32_t logged = 0;
    if (logged < 10) {
      ++logged;
      REXGPU_INFO(
          "NativeCommandProcessor: sysconst draw#{} clip_cntl={:08X} vte_cntl={:08X} "
          "flags={:08X} scale=({:.3f},{:.3f},{:.3f}) offset=({:.3f},{:.3f},{:.3f}) "
          "vidx_min={} vidx_max={} blend_control={:08X} color_mask={:08X}",
          draws_logged_, pa_cl_clip_cntl.value, pa_cl_vte_cntl.value, system_constants.flags,
          system_constants.ndc_scale[0], system_constants.ndc_scale[1],
          system_constants.ndc_scale[2], system_constants.ndc_offset[0],
          system_constants.ndc_offset[1], system_constants.ndc_offset[2],
          system_constants.vertex_index_min, system_constants.vertex_index_max,
          registers_.Get<rex::graphics::reg::RB_BLENDCONTROL>().value,
          registers_.Get<rex::graphics::reg::RB_COLOR_MASK>().value);
    }
  }
  upload_constant_buffer(&system_constants, sizeof(system_constants),
                         SpirvShaderTranslator::kConstantBufferSystem);

  // Float constants: tightly packed in ascending register order, only the
  // registers the shader actually reads -- see shader.h's
  // ConstantRegisterMap::GetPackedFloatConstantIndex, replicated here.
  auto upload_float_constants = [&](const rex::graphics::Shader::ConstantRegisterMap& map,
                                    uint32_t base_register, uint32_t binding) {
    uint32_t count = std::max(map.float_count, 1u);
    std::vector<float> data(size_t(count) * 4, 0.0f);
    uint32_t out_index = 0;
    for (uint32_t block = 0; block < 4; ++block) {
      uint64_t bits = map.float_bitmap[block];
      uint32_t bit_index;
      while (rex::bit_scan_forward(bits, &bit_index)) {
        bits &= ~(uint64_t(1) << bit_index);
        uint32_t reg_index = base_register + (block << 8) + (bit_index << 2);
        if (out_index < count) {
          std::memcpy(&data[size_t(out_index) * 4], &registers_[reg_index], sizeof(float) * 4);
        }
        ++out_index;
      }
    }
    upload_constant_buffer(data.data(), data.size() * sizeof(float), binding);
  };
  upload_float_constants(vertex_shader->shader->constant_register_map(),
                         rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X,
                         SpirvShaderTranslator::kConstantBufferFloatVertex);
  upload_float_constants(pixel_shader->shader->constant_register_map(),
                         rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X,
                         SpirvShaderTranslator::kConstantBufferFloatPixel);

  // Bool/loop and fetch constants: always copied in full (small, fixed size),
  // no packing -- see command_processor.cpp's UpdateBindings.
  uint32_t bool_loop[8 + 32];
  std::memcpy(bool_loop, &registers_[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
              sizeof(bool_loop));
  upload_constant_buffer(bool_loop, sizeof(bool_loop),
                         SpirvShaderTranslator::kConstantBufferBoolLoop);

  uint32_t fetch_constants[6 * 32];
  std::memcpy(fetch_constants, &registers_[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
              sizeof(fetch_constants));
  upload_constant_buffer(fetch_constants, sizeof(fetch_constants),
                         SpirvShaderTranslator::kConstantBufferFetch);

  // Builds a single descriptor set covering *every* texture_bindings()/
  // sampler_bindings() entry a shader stage actually needs (not just the
  // first one -- see GetOrCreateTextureSetLayout's doc comment on why a
  // fixed single-texture layout was wrong for shaders needing multiple
  // simultaneous texture units, e.g. a base image + glow/overlay layer).
  // Each missing/unsupported real texture falls back to the 1x1 white
  // default view for just that slot, not the whole set. Allocated fresh
  // per draw from transient_descriptor_pool_ (same reasoning as
  // upload_constant_buffer above: a combined set's exact layout depends on
  // this specific shader, so it can't be a persistent per-texture set
  // anymore).
  auto resolve_texture_set = [&](TranslatedShader* shader) -> VkDescriptorSet {
    // Must match GetOrCreatePipeline's counts/ordering exactly -- see
    // GetOrAnalyzeShader's doc comment on why this is
    // SpirvShader::GetTextureBindingsAfterTranslation()/
    // GetSamplerBindingsAfterTranslation(), not base Shader::texture_bindings().
    auto* spirv_shader = dynamic_cast<rex::graphics::SpirvShader*>(shader->shader);
    const auto& tex_bindings = spirv_shader->GetTextureBindingsAfterTranslation();
    const auto& smp_bindings = spirv_shader->GetSamplerBindingsAfterTranslation();
    uint32_t tex_count = uint32_t(tex_bindings.size());
    uint32_t smp_count = uint32_t(smp_bindings.size());
    VkDescriptorSetLayout layout = GetOrCreateTextureSetLayout(tex_count, smp_count);
    if (layout == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = transient_descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (dfn.vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
      REXGPU_ERROR(
          "NativeCommandProcessor: transient descriptor pool exhausted (texture set), skipping "
          "draw");
      return VK_NULL_HANDLE;
    }
    if (tex_count == 0 && smp_count == 0) {
      return set;
    }

    std::vector<VkDescriptorImageInfo> image_infos(tex_count);
    std::vector<VkDescriptorImageInfo> sampler_infos(smp_count);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(size_t(tex_count) + smp_count);
    // True if any texture bound in this draw's set is the gameplay-preview
    // texture -- texture and sampler bindings are compacted/dedup'd
    // independently (see the doc comment on GetOrCreateTextureSetLayout), so
    // there's no reliable per-index texture<->sampler correspondence to key
    // off of; scoping the nearest-vs-bilinear choice to "this draw" instead
    // of "this exact binding slot" is the simplification that still keeps it
    // off every other texture in the game (the preview is always its own
    // standalone draw, never combined with other unrelated textures).
    bool draw_samples_gameplay_preview = false;
    for (uint32_t i = 0; i < tex_count; ++i) {
      UploadedTexture* texture = GetOrUploadTexture(uint32_t(tex_bindings[i].fetch_constant));
      image_infos[i] = {VK_NULL_HANDLE, texture ? texture->view : default_texture_view_,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      if (texture && texture->is_gameplay_preview) {
        draw_samples_gameplay_preview = true;
      }
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = set;
      write.dstBinding = i;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &image_infos[i];
      writes.push_back(write);
    }
    for (uint32_t i = 0; i < smp_count; ++i) {
      // Address modes come from each binding's texture fetch constant (see
      // GetOrCreateSampler -- the smoke overlay scrolls U past 1.0 and needs
      // kRepeat); filtering stays the fixed linear/nearest choice.
      auto tf = registers_.GetTextureFetch(uint32_t(smp_bindings[i].fetch_constant));
      VkSampler sampler = GetOrCreateSampler(tf.clamp_x, tf.clamp_y, tf.clamp_z,
                                             draw_samples_gameplay_preview);
      if (sampler == VK_NULL_HANDLE) {
        sampler = draw_samples_gameplay_preview ? gameplay_preview_sampler_ : default_sampler_;
      }
      sampler_infos[i] = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = set;
      write.dstBinding = tex_count + i;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      write.pImageInfo = &sampler_infos[i];
      writes.push_back(write);
    }
    dfn.vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    return set;
  };
  VkDescriptorSet vertex_texture_set = resolve_texture_set(vertex_shader);
  VkDescriptorSet pixel_texture_set = resolve_texture_set(pixel_shader);
  if (vertex_texture_set == VK_NULL_HANDLE || pixel_texture_set == VK_NULL_HANDLE) {
    return;
  }

  VkDescriptorSet sets[4] = {shared_memory_set_, constants_set, vertex_texture_set,
                            pixel_texture_set};
  dfn.vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_entry.pipeline);
  dfn.vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline_entry.layout, 0, 4, sets, 0, nullptr);
  // Shared by both synthesized-index-buffer paths below: uploads `indices`
  // as a fresh transient VK_INDEX_TYPE_UINT32 buffer and issues the indexed
  // draw. Fresh per draw for the same reason the constant buffers are (see
  // the comment above upload_constant_buffer).
  auto draw_with_synthesized_indices = [&](const std::vector<uint32_t>& indices) {
    VkDeviceSize index_buffer_size = indices.size() * sizeof(uint32_t);
    VkBuffer index_buffer;
    VkDeviceMemory index_memory;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, index_buffer_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kUpload, index_buffer, index_memory)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to allocate a synthesized index buffer");
      return;
    }
    void* mapped = nullptr;
    dfn.vkMapMemory(device, index_memory, 0, index_buffer_size, 0, &mapped);
    std::memcpy(mapped, indices.data(), index_buffer_size);
    FlushMapped(vulkan_device, index_memory);
    dfn.vkUnmapMemory(device, index_memory);
    frame_transient_buffers_.push_back({index_buffer, index_memory});

    dfn.vkCmdBindIndexBuffer(command_buffer_, index_buffer, 0, VK_INDEX_TYPE_UINT32);
    dfn.vkCmdDrawIndexed(command_buffer_, uint32_t(indices.size()), 1, 0, 0, 0);
  };

  if (is_tessellated_quad) {
    // One 4-control-point patch per quad, drawn non-indexed: the passthrough
    // VS turns gl_VertexIndex into the per-control-point index, the fixed
    // quad TCS gathers each patch's 4 indices and sets the tessellation
    // levels from the factor pushed here (VGT_HOS_MAX_TESS_LEVEL + 1.0 --
    // "1.0 already added to the factor on the CPU" per the real backend's
    // tessellation_factor_range_max), and the guest shader runs as the
    // tessellation evaluation stage, fetching control point vertex data
    // itself from the shared-memory SSBO.
    float tessellation_factor =
        registers_.Get<float>(rex::graphics::XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    tessellation_factor = std::min(std::max(tessellation_factor, 1.0f), 64.0f);
    dfn.vkCmdPushConstants(command_buffer_, pipeline_entry.layout,
                           VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, 0, sizeof(float),
                           &tessellation_factor);
    dfn.vkCmdDraw(command_buffer_, num_indices, 1, 0, 0);
  } else if (is_quad_list) {
    // Two triangles per quad (0,1,2,0,2,3), indexing the same autoindex
    // vertex stream a non-indexed draw of this quad list would have used --
    // the vertex shader still reads vertex 0..num_indices-1 via the shared
    // memory SSBO, only the host-side assembly into triangles is synthesized.
    std::vector<uint32_t> indices(size_t(quad_count) * 6);
    for (uint32_t quad = 0; quad < quad_count; ++quad) {
      uint32_t base = quad * 4;
      uint32_t* out = &indices[size_t(quad) * 6];
      out[0] = base;
      out[1] = base + 1;
      out[2] = base + 2;
      out[3] = base;
      out[4] = base + 2;
      out[5] = base + 3;
    }
    draw_with_synthesized_indices(indices);
  } else if (is_rect_list) {
    // Two-triangle-strip-per-rectangle synthetic indices, matching
    // PrimitiveProcessor::InitializeCache exactly: per rectangle i, a
    // primitive-restart marker (except before i==0) then 4 synthetic
    // indices (i<<2)+0..3. The translated vertex shader (built with
    // HostVertexShaderType::kRectangleListAsTriangleStrip above) decodes
    // each synthetic index back into (primitive_index, vertex_in_primitive)
    // itself and reconstructs the missing 4th corner -- this step only
    // needs to feed it the right index sequence, not compute geometry.
    std::vector<uint32_t> indices;
    indices.reserve(size_t(rect_count) * 5);
    for (uint32_t rect = 0; rect < rect_count; ++rect) {
      if (rect != 0) {
        indices.push_back(0xFFFFFFFFu);
      }
      uint32_t base = rect << 2;
      indices.push_back(base);
      indices.push_back(base + 1);
      indices.push_back(base + 2);
      indices.push_back(base + 3);
    }
    draw_with_synthesized_indices(indices);
  } else {
    dfn.vkCmdDraw(command_buffer_, num_indices, 1, 0, 0);
  }

  frame_has_draws_ = true;

  static bool logged_first_draw = false;
  if (!logged_first_draw) {
    logged_first_draw = true;
    REXGPU_INFO(
        "NativeCommandProcessor: first real draw issued (num_indices={} is_quad_list={} "
        "is_rect_list={})",
        num_indices, is_quad_list, is_rect_list);
  }
}

void NativeCommandProcessor::DebugDumpColorTarget() {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkBuffer readback_buffer;
  VkDeviceMemory readback_memory;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, color_target_staging_size_, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kReadback, readback_buffer, readback_memory)) {
    REXGPU_ERROR("NativeCommandProcessor: debug dump failed to allocate a readback buffer");
    return;
  }

  VkCommandBuffer cmd;
  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = command_pool_;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &alloc_info, &cmd) == VK_SUCCESS) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(cmd, &begin_info);
    VkBufferCopy copy{0, 0, color_target_staging_size_};
    dfn.vkCmdCopyBuffer(cmd, color_target_staging_buffer_, readback_buffer, 1, &copy);
    dfn.vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (dfn.vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS) {
      VkSubmitInfo submit_info{};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &cmd;
      rex::ui::vulkan::VulkanDevice::Queue::Acquisition queue =
          vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, fence) == VK_SUCCESS) {
        constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull;
        dfn.vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs);
      }
      dfn.vkDestroyFence(device, fence, nullptr);
    }
    // Leaked, same rationale as UploadTexelsAndTransition -- vkFreeCommandBuffers
    // isn't in this SDK's exposed Vulkan function table, and this is temporary
    // debug code capped at 3 dumps for the process lifetime.
  }

  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, readback_memory, 0, color_target_staging_size_, 0, &mapped) ==
      VK_SUCCESS) {
    // CPU reading GPU-written memory needs an invalidate, not a flush (the
    // opposite direction from FlushMapped's writes) -- otherwise the CPU may
    // read a stale cached copy on non-coherent memory types.
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = readback_memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
    char path[256];
    std::snprintf(path, sizeof(path), "logs/debug_color_target_%u_%ux%u_a2b10g10r10.raw",
                 debug_frames_dumped_, color_target_width_, color_target_height_);
    FILE* f = std::fopen(path, "wb");
    if (f) {
      std::fwrite(mapped, 1, color_target_staging_size_, f);
      std::fclose(f);
      REXGPU_INFO("NativeCommandProcessor: debug-dumped color target to {}", path);
    }
    dfn.vkUnmapMemory(device, readback_memory);
  }
  ++debug_frames_dumped_;

  dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);
}

void NativeCommandProcessor::PresentFrame() {
  if (command_buffer_ == VK_NULL_HANDLE) {
    return;
  }

  // Scaled by the guest clock's time scalar (see rex::chrono::Clock::
  // set_guest_time_scalar, used by src/fast_forward.h) so this real-time
  // floor doesn't cap out the guest's own effective speed while
  // fast-forwarding -- without this, scaling guest time to 2.5x would
  // still only be allowed to submit one frame per real 16ms here, visibly
  // capping the game back at 60fps despite the guest logic running faster
  // underneath.
  double guest_time_scalar = rex::chrono::Clock::guest_time_scalar();
  if (!(guest_time_scalar > 0.0)) {
    guest_time_scalar = 1.0;
  }
  auto min_frame_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double, std::milli>(16.0 / guest_time_scalar));
  auto now = std::chrono::steady_clock::now();
  auto elapsed = now - last_present_time_;
  if (elapsed < min_frame_interval) {
    std::this_thread::sleep_for(min_frame_interval - elapsed);
  }
  last_present_time_ = std::chrono::steady_clock::now();

  // Guarantees a valid recording command buffer + cleared color target even
  // if this frame had zero (or all-skipped) draws.
  EnsureFrameBegun();

  // Fast-forward frame-skip: the guest's mode loop calls PresentFrame once
  // per logic iteration, so without this gate, real GPU present work below would run every single
  // call and cap guest FPS at whatever the Vulkan submit/present/fence-wait
  // round trip costs (~95fps measured) regardless of how high
  // guest_time_scalar is set. Gate physical presentation to a fixed,
  // *unscaled* ~60Hz real-time interval instead -- logic keeps iterating
  // (and drawing into the still-open frame below, via EnsureFrameBegun's
  // no-op fast path) between real presents. kMaxFramesBetweenPresents is a
  // safety valve bounding worst-case accumulated-draws growth if the scaled
  // sleep above collapses toward zero at extreme scale values (existing
  // transient descriptor pool / constants arena headroom was sized for 4096
  // draws/frame -- so 64 accumulated logical frames is comfortably inside that
  // margin).
  constexpr auto kPhysicalPresentInterval = std::chrono::milliseconds(16);
  constexpr uint32_t kMaxFramesBetweenPresents = 64;
  ++frames_since_present_;
  bool do_present = (last_present_time_ - last_physical_present_time_ >=
                     kPhysicalPresentInterval) ||
                    frames_since_present_ >= kMaxFramesBetweenPresents;

  if (do_present) {
    frames_since_present_ = 0;
    last_physical_present_time_ = last_present_time_;

    const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
    const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    VkDevice device = vulkan_device->device();

    // Flush all of this frame's constant-buffer writes to the arena in one call
    // (kUpload memory is only guaranteed host-visible, not coherent -- see the
    // FlushMapped comment), replacing the old per-buffer flush. Must happen
    // before the command buffer that reads the arena is submitted below.
    if (constants_arena_offset_ > 0) {
      FlushMapped(vulkan_device, constants_arena_memory_);
    }

    // Render pass's finalLayout is already TRANSFER_SRC_OPTIMAL, so
    // color_target_image_ needs no barrier before the blit below.
    dfn.vkCmdEndRenderPass(command_buffer_);

    bool refreshed = presenter_->RefreshGuestOutput(
      color_target_width_, color_target_height_, color_target_width_, color_target_height_,
      [this, vulkan_device, &dfn, device](
          rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vulkan_context =
            static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(
                context);

        VkImageSubresourceRange subresource_range = rex::ui::vulkan::util::InitializeSubresourceRange();

        // Acquire barrier: UNDEFINED->TRANSFER_DST, contents fully overwritten
        // so the previous contents don't need to be preserved either way.
        VkImageMemoryBarrier acquire_barrier;
        acquire_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire_barrier.pNext = nullptr;
        acquire_barrier.srcAccessMask =
            vulkan_context.image_ever_written_previously()
                ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                : 0;
        acquire_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        acquire_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        acquire_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acquire_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire_barrier.image = vulkan_context.image();
        acquire_barrier.subresourceRange = subresource_range;
        dfn.vkCmdPipelineBarrier(
            command_buffer_,
            vulkan_context.image_ever_written_previously()
                ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &acquire_barrier);

        // Copy the offscreen render target (draws accumulated into it this
        // frame, or just the clear color if there were none) into the guest
        // output image, relayed through a staging buffer: neither
        // vkCmdBlitImage nor vkCmdCopyImage (image-to-image) is in this
        // SDK's exposed Vulkan function table, only buffer<->image copies
        // are. This is exact (no scaling/format conversion needed) since both
        // color_target_image_ and the guest output image are created at
        // color_target_width_ x color_target_height_, in kGuestOutputFormat
        // (VK_FORMAT_A2B10G10R10_UNORM_PACK32).
        VkBufferImageCopy buffer_image_copy{};
        buffer_image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        buffer_image_copy.imageExtent = {color_target_width_, color_target_height_, 1};
        dfn.vkCmdCopyImageToBuffer(command_buffer_, color_target_image_,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   color_target_staging_buffer_, 1, &buffer_image_copy);

        VkBufferMemoryBarrier staging_barrier{};
        staging_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        staging_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        staging_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        staging_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        staging_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        staging_barrier.buffer = color_target_staging_buffer_;
        staging_barrier.size = color_target_staging_size_;
        dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &staging_barrier,
                                 0, nullptr);

        dfn.vkCmdCopyBufferToImage(command_buffer_, color_target_staging_buffer_,
                                   vulkan_context.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &buffer_image_copy);

        // Release barrier: back to the layout/stage/access the presenter
        // expects to take over from (see VulkanPresenter::kGuestOutputInternal*).
        VkImageMemoryBarrier release_barrier;
        release_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        release_barrier.pNext = nullptr;
        release_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        release_barrier.dstAccessMask =
            rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
        release_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        release_barrier.newLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
        release_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        release_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        release_barrier.image = vulkan_context.image();
        release_barrier.subresourceRange = subresource_range;
        dfn.vkCmdPipelineBarrier(
            command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
            rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
            nullptr, 1, &release_barrier);

        dfn.vkEndCommandBuffer(command_buffer_);

        VkSubmitInfo submit_info;
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = nullptr;
        submit_info.waitSemaphoreCount = 0;
        submit_info.pWaitSemaphores = nullptr;
        submit_info.pWaitDstStageMask = nullptr;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        submit_info.signalSemaphoreCount = 0;
        submit_info.pSignalSemaphores = nullptr;

        VkResult submit_result;
        {
          rex::ui::vulkan::VulkanDevice::Queue::Acquisition queue =
              vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
          submit_result = dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, submit_fence_);
        }
        if (submit_result != VK_SUCCESS) {
          REXGPU_ERROR("NativeCommandProcessor: vkQueueSubmit failed ({})",
                       static_cast<int32_t>(submit_result));
          return false;
        }

        // The refresher contract requires all submitted work (including the
        // release barrier's signaling) to complete before returning true;
        // simplest correct option for this milestone is a blocking wait.
        constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull;
        if (dfn.vkWaitForFences(device, 1, &submit_fence_, VK_TRUE, kFenceTimeoutNs) !=
            VK_SUCCESS) {
          REXGPU_ERROR("NativeCommandProcessor: timed out waiting for this frame's fence");
          return false;
        }

        static bool logged_first_present = false;
        if (!logged_first_present) {
          logged_first_present = true;
          REXGPU_INFO("NativeCommandProcessor: first frame presented (had_draws={})",
                      frame_has_draws_);
        }

        if (frame_has_draws_ && debug_frames_dumped_ < 3) {
          DebugDumpColorTarget();
        }

        return true;
      });
    if (!refreshed) {
      static bool logged_refresh_failure = false;
      if (!logged_refresh_failure) {
        logged_refresh_failure = true;
        REXGPU_ERROR("NativeCommandProcessor: RefreshGuestOutput failed");
      }
    }

    // Whether or not the refresh succeeded, this frame's command buffer has
    // either been submitted or is unusable garbage either way -- start fresh
    // next time. frame_transient_buffers_/the transient descriptor pool are
    // reclaimed at the *start* of next frame's EnsureFrameBegun, once its
    // fence wait confirms this frame's GPU work (which references them) is
    // actually done.
    frame_active_ = false;
  }  // do_present

  // See SetOnFramePresented's doc comment -- this is the only "GPU swap"
  // event this renderer has, so it's what drives the F3 overlay's guest-FPS
  // counter (and ModRegistry::DispatchTick for any mod relying on it), which
  // otherwise never fire at all under headless native rendering (no
  // GraphicsSystem exists to call the normal SetHostSwapCallback chain).
  // Fires every logical frame regardless of do_present -- during
  // fast-forward this should reflect true logic throughput (guest FPS), not
  // the throttled physical present rate.
  if (on_frame_presented_) {
    on_frame_presented_();
  }
}

}  // namespace nocturne
