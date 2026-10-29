// nocturnerecomp - ReXGlue Recompiled Project
//
// See native_renderer_draw.h.

#include "native_renderer_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "guest_shaders.h"
#include "native_renderer_plume.h"
#include "native_renderer_plume_internal.h"
#include "native_renderer_texture.h"
#ifdef _WIN32
#include "shaders/blitVert.hlsl.dxil.h"
#include "shaders/blitFrag.hlsl.dxil.h"
#endif
#include "shaders/blitVert.hlsl.spirv.h"
#include "shaders/blitFrag.hlsl.spirv.h"

namespace nocturne {
namespace {

using namespace plume;

// The guest's render target. The title binds one, 1280x720, for the whole run
// and positions everything in those pixels. The host keeps one, and a second it
// resolves into, at the size the window shows them (PlumeGuestExtent): the
// vertex shaders' screen transform maps guest pixels to clip space, so the
// geometry scales with the host target and only the places that speak guest
// pixels directly (the clear rectangle, the half pixel shift) convert.
constexpr uint32_t kGuestWidth = 1280;
constexpr uint32_t kGuestHeight = 720;
uint32_t g_target_width = kGuestWidth;
uint32_t g_target_height = kGuestHeight;
constexpr RenderFormat kTargetFormat = RenderFormat::R8G8B8A8_UNORM;

// The pipeline layout every guest pipeline shares, matching the bindings
// KryptonRecomp/xenos_hlsl.py emits: textures t0..t15 in space 0 (set 0),
// samplers s0..s15 in space 1 (set 1), and six constant buffers in space 2 as
// root descriptors:
//
//   b0  vertex float constants   float4 c[256]
//   b1  vertex bool/loop constants
//   b2  pixel float constants
//   b3  pixel bool/loop constants
//   b4  XeDrawState: alpha test and texture swizzles
//   b5  XeScreenTransform, the vertex shaders' viewport transform
//       (scripts/gen-guest-shaders.py)
constexpr uint32_t kTextureSlots = 16;
constexpr uint32_t kConstantRegisterSpace = 2;
constexpr uint32_t kTextureDescriptorSet = 0;
constexpr uint32_t kSamplerDescriptorSet = 1;
enum : uint32_t {
  kRootVertexFloats,
  kRootVertexBools,
  kRootPixelFloats,
  kRootPixelBools,
  kRootDrawState,
  kRootScreenTransform,
  kRootCount,
};

// Root constant buffer views have to be 256 byte aligned on D3D12, and nothing
// else here needs more, so every allocation uses it.
constexpr uint64_t kUploadAlignment = 256;
constexpr uint64_t kArenaBlockBytes = 4ull << 20;

// The device object, NR's layout (see HANDOFF / config/nocturnerecomp_d3d9.toml). The
// register shadows are blocks ascending in register order, so a register's
// shadow is base + 4 * (reg - first): RB_DEPTHCONTROL (0x2200) is +10420 and
// RB_ALPHA_REF (0x210E) +10372, both observed.
constexpr uint32_t kVertexFloatShadow = 1792;
constexpr uint32_t kPixelFloatShadow = 5888;
constexpr uint32_t kBoolShadow = 9984;
constexpr uint32_t kLoopShadow = 10016;
constexpr uint32_t kBoundDeclaration = 0x2D14;
constexpr uint32_t kBoundVertexShader = 0x3080;
constexpr uint32_t kBoundPixelShader = 0x307C;
constexpr uint32_t kBoundRenderTarget0 = 0x2F80;
constexpr uint32_t kTextureFetchConstants = 1024;  // 24 bytes per stage
// A texture object's own copy of its fetch constant, which SetTexture and
// Resolve both read.
constexpr uint32_t kTextureObjectFetch = 0x1C;
constexpr uint32_t Reg2200(uint32_t reg) { return 10420 + 4 * (reg - 0x2200); }
constexpr uint32_t Reg2100(uint32_t reg) { return 10316 + 4 * (reg - 0x2100); }

// The declaration object: element count at +24, elements from +52, 12 bytes
// each (stream u16, offset u16, fetch type u32, then usage and usage index at
// +9 and +10).
constexpr uint32_t kDeclCount = 24;
constexpr uint32_t kDeclElements = 52;
constexpr uint32_t kDeclElementBytes = 12;
constexpr uint32_t kMaxDeclElements = 16;

// D3DRESOLVE_CLEARRENDERTARGET.
constexpr uint32_t kResolveClearTarget = 0x200;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// Big endian dwords out of guest memory into host order.
void CopySwapped(uint8_t* dst, const uint8_t* base, uint32_t address, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes; i += 4) {
    const uint32_t value = Load32(base, address + i);
    std::memcpy(dst + i, &value, 4);
  }
}

// ---------------------------------------------------------------------------
// Dropped draws. A dropped draw is missing geometry, so every reason is counted
// and named once.

enum DropReason {
  kDropUnresolvedShader,
  kDropNoDeclaration,
  kDropInput,
  kDropVertexFormat,
  kDropTopology,
  kDropPipeline,
  kDropArena,
  kDropTextures,
  kDropCount,
};
constexpr const char* kDropNames[kDropCount] = {
    "a bound shader is not in the pack",
    "no vertex declaration is bound",
    "the declaration has no element for a vertex shader input",
    "a vertex format the dword swap cannot serve",
    "a primitive type with no host topology",
    "pipeline creation failed",
    "the upload arena could not grow",
    "a texture descriptor set could not be created",
};
uint64_t g_drops[kDropCount] = {};

void Drop(DropReason reason, uint32_t detail) {
  if (g_drops[reason]++ == 0)
    REXLOG_WARN("native_renderer: dropping draws: {} ({:#x})", kDropNames[reason], detail);
}

// ---------------------------------------------------------------------------
// The upload arena: one per frame slot, since the GPU reads a frame's vertices
// and constants straight out of it until that frame retires.

struct ArenaBlock {
  std::unique_ptr<RenderBuffer> buffer;
  uint8_t* cpu = nullptr;
  uint64_t size = 0;
  uint64_t used = 0;
};

struct Allocation {
  RenderBufferReference ref;
  uint8_t* cpu = nullptr;
  explicit operator bool() const { return cpu != nullptr; }
};

std::vector<ArenaBlock> g_arenas[kFramesInFlight];
uint32_t g_arena_slot = 0;

Allocation ArenaAllocate(RenderDevice* device, uint64_t bytes) {
  bytes = (bytes + kUploadAlignment - 1) / kUploadAlignment * kUploadAlignment;
  std::vector<ArenaBlock>& arena = g_arenas[g_arena_slot];
  for (ArenaBlock& block : arena) {
    if (block.used + bytes <= block.size) {
      Allocation allocation{block.buffer->at(block.used), block.cpu + block.used};
      block.used += bytes;
      return allocation;
    }
  }
  ArenaBlock block;
  const uint64_t size = bytes > kArenaBlockBytes ? bytes : kArenaBlockBytes;
  // One buffer serving as vertex, index and constant source. Upload heaps can
  // stay mapped for their lifetime on both backends.
  block.buffer = device->createBuffer(RenderBufferDesc::UploadBuffer(
      size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::CONSTANT));
  if (!block.buffer)
    return {};
  block.cpu = static_cast<uint8_t*>(block.buffer->map());
  if (block.cpu == nullptr)
    return {};
  block.size = size;
  block.used = bytes;
  arena.push_back(std::move(block));
  return {arena.back().buffer->at(0), arena.back().cpu};
}

// ---------------------------------------------------------------------------
// Textures whose layout the draw path tracks itself, since the guest target
// moves between drawing, copying and (later) sampling within a frame.

struct TrackedTexture {
  std::unique_ptr<RenderTexture> texture;
  std::unique_ptr<RenderFramebuffer> framebuffer;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;

  void Transition(RenderCommandList* commands, RenderTextureLayout wanted) {
    if (layout == wanted)
      return;
    const bool copy = wanted == RenderTextureLayout::COPY_SOURCE ||
                      wanted == RenderTextureLayout::COPY_DEST;
    commands->barriers(copy ? RenderBarrierStage::COPY : RenderBarrierStage::GRAPHICS,
                       RenderTextureBarrier(texture.get(), wanted));
    layout = wanted;
  }
};

bool CreateTarget(RenderDevice* device, TrackedTexture& target) {
  target.texture =
      device->createTexture(RenderTextureDesc::ColorTarget(g_target_width, g_target_height, kTargetFormat));
  if (!target.texture)
    return false;
  const RenderTexture* attachment = target.texture.get();
  RenderFramebufferDesc desc;
  desc.colorAttachments = &attachment;
  desc.colorAttachmentsCount = 1;
  target.framebuffer = device->createFramebuffer(desc);
  return target.framebuffer != nullptr;
}

// ---------------------------------------------------------------------------
// Resources shared by every draw.

struct Resources {
  bool attempted = false;
  bool ready = false;

  std::unique_ptr<RenderPipelineLayout> layout;
  std::unique_ptr<RenderTexture> white;
  std::unique_ptr<RenderSampler> sampler;

  // The guest's render target, and the image a Resolve copies it into, which
  // is what the present shows.
  TrackedTexture target;
  TrackedTexture front;

  std::unique_ptr<RenderPipelineLayout> blit_layout;
  std::unique_ptr<RenderPipeline> blit_pipeline;
  std::unique_ptr<RenderSampler> blit_sampler;
  std::unique_ptr<RenderDescriptorSet> blit_set;

  std::unique_ptr<RenderShader> vertex_shaders[256];
  std::unique_ptr<RenderShader> pixel_shaders[256];
} g_res;

// Descriptor sets by what they hold. A set cannot be rewritten while a frame
// that bound it may still be in flight, so each distinct binding gets its own
// and keeps it; the title binds few distinct combinations. The textures and
// samplers they point at live as long as the mirror's cache, i.e. until
// shutdown.
struct TextureBinding {
  RenderTexture* textures[kTextureSlots] = {};
  RenderSampler* samplers[kTextureSlots] = {};
  std::unique_ptr<RenderDescriptorSet> texture_set;
  std::unique_ptr<RenderDescriptorSet> sampler_set;
};
std::vector<std::unique_ptr<TextureBinding>> g_bindings;

// The base address of the texture the last Resolve wrote into. A draw sampling
// it samples the host image the Resolve produced, not guest memory, which the
// host never writes.
uint32_t g_resolve_address = 0;

// Per frame recording state, reset when a new frame's command list begins.
uint64_t g_frame_serial = 0;
bool g_target_bound = false;
// Whether the targets still need their first clear: a fresh texture holds
// whatever the allocator gave it.
bool g_targets_cleared = false;

// Upload one 1x1 texel through a one-shot command list, like the overlay's
// textures: this can run while the frame's own list is recording.
std::unique_ptr<RenderTexture> CreateWhiteTexture(RenderDevice* device) {
  auto texture = device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  if (!texture)
    return nullptr;
  constexpr uint32_t kPitch = 256;
  auto staging = device->createBuffer(RenderBufferDesc::UploadBuffer(kPitch));
  if (!staging)
    return nullptr;
  auto* mapped = static_cast<uint8_t*>(staging->map());
  if (mapped == nullptr)
    return nullptr;
  std::memset(mapped, 0xFF, 4);
  staging->unmap();

  RenderCommandQueue* queue = PlumeQueue();
  auto commands = queue->createCommandList();
  auto fence = device->createCommandFence();
  commands->begin();
  commands->barriers(RenderBarrierStage::COPY,
                     RenderTextureBarrier(texture.get(), RenderTextureLayout::COPY_DEST));
  commands->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(texture.get()),
      RenderTextureCopyLocation::PlacedFootprint(staging.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                                                 kPitch / 4));
  commands->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(texture.get(), RenderTextureLayout::SHADER_READ));
  commands->end();
  const RenderCommandList* submit = commands.get();
  queue->executeCommandLists(&submit, 1, nullptr, 0, nullptr, 0, fence.get());
  queue->waitForCommandFence(fence.get());
  return texture;
}

std::unique_ptr<RenderShader> CreateBuiltinShader(RenderDevice* device, const void* dxil,
                                                  uint64_t dxil_size, const void* spirv,
                                                  uint64_t spirv_size, const char* entry) {
  const RenderShaderFormat format = PlumeShaderFormat();
  if (format == RenderShaderFormat::DXIL && dxil != nullptr)
    return device->createShader(dxil, dxil_size, entry, format);
  if (format == RenderShaderFormat::SPIRV)
    return device->createShader(spirv, spirv_size, entry, format);
  return nullptr;
}

bool CreateBlit(RenderDevice* device) {
  const RenderDescriptorRange ranges[] = {
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
      RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  const RenderDescriptorSetDesc set_desc(ranges, 2);
  RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  g_res.blit_layout = device->createPipelineLayout(layout_desc);
  if (!g_res.blit_layout)
    return false;

#ifdef _WIN32
  auto vertex = CreateBuiltinShader(device, blitVertBlobDXIL, sizeof(blitVertBlobDXIL),
                                    blitVertBlobSPIRV, sizeof(blitVertBlobSPIRV), "VSMain");
  auto pixel = CreateBuiltinShader(device, blitFragBlobDXIL, sizeof(blitFragBlobDXIL),
                                   blitFragBlobSPIRV, sizeof(blitFragBlobSPIRV), "PSMain");
#else
  auto vertex = CreateBuiltinShader(device, nullptr, 0, blitVertBlobSPIRV,
                                    sizeof(blitVertBlobSPIRV), "VSMain");
  auto pixel = CreateBuiltinShader(device, nullptr, 0, blitFragBlobSPIRV,
                                   sizeof(blitFragBlobSPIRV), "PSMain");
#endif
  if (!vertex || !pixel)
    return false;

  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_res.blit_layout.get();
  desc.vertexShader = vertex.get();
  desc.pixelShader = pixel.get();
  desc.renderTargetFormat[0] = PlumeSwapChainFormat();
  desc.renderTargetCount = 1;
  desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
  desc.cullMode = RenderCullMode::NONE;
  desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
  g_res.blit_pipeline = device->createGraphicsPipeline(desc);
  if (!g_res.blit_pipeline)
    return false;

  RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = RenderFilter::LINEAR;
  sampler_desc.magFilter = RenderFilter::LINEAR;
  sampler_desc.addressU = RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = RenderTextureAddressMode::CLAMP;
  g_res.blit_sampler = device->createSampler(sampler_desc);
  g_res.blit_set = device->createDescriptorSet(set_desc);
  if (!g_res.blit_sampler || !g_res.blit_set)
    return false;
  g_res.blit_set->setTexture(0, g_res.front.texture.get(), RenderTextureLayout::SHADER_READ);
  g_res.blit_set->setSampler(1, g_res.blit_sampler.get());
  return true;
}

// The texture and sampler set layouts. One range per slot rather than one
// sixteen wide: Plume turns a range into a single Vulkan binding with an array
// count, and the HLSL declares sixteen separate objects at bindings 0..15 (see
// Eternal Sonata's layout).
RenderDescriptorSetDesc SlotSetDesc(RenderDescriptorRangeType type) {
  static RenderDescriptorRange texture_ranges[kTextureSlots];
  static RenderDescriptorRange sampler_ranges[kTextureSlots];
  RenderDescriptorRange* ranges =
      type == RenderDescriptorRangeType::TEXTURE ? texture_ranges : sampler_ranges;
  for (uint32_t slot = 0; slot < kTextureSlots; ++slot)
    ranges[slot] = RenderDescriptorRange(type, slot, 1);
  return RenderDescriptorSetDesc(ranges, kTextureSlots);
}

bool EnsureResources(RenderDevice* device) {
  if (g_res.attempted)
    return g_res.ready;
  g_res.attempted = true;

  const RenderDescriptorSetDesc set_descs[] = {
      SlotSetDesc(RenderDescriptorRangeType::TEXTURE),
      SlotSetDesc(RenderDescriptorRangeType::SAMPLER),
  };
  RenderRootDescriptorDesc root_descriptors[kRootCount];
  for (uint32_t i = 0; i < kRootCount; ++i) {
    root_descriptors[i] = RenderRootDescriptorDesc(i, kConstantRegisterSpace,
                                                   RenderRootDescriptorType::CONSTANT_BUFFER);
  }
  RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = set_descs;
  desc.descriptorSetDescsCount = uint32_t(std::size(set_descs));
  desc.rootDescriptorDescs = root_descriptors;
  desc.rootDescriptorDescsCount = kRootCount;
  desc.allowInputLayout = true;
  g_res.layout = device->createPipelineLayout(desc);
  if (!g_res.layout) {
    REXLOG_ERROR("native_renderer: could not create the guest pipeline layout; nothing will draw");
    return false;
  }

  g_res.white = CreateWhiteTexture(device);
  RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = RenderFilter::LINEAR;
  sampler_desc.magFilter = RenderFilter::LINEAR;
  sampler_desc.addressU = RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = RenderTextureAddressMode::CLAMP;
  g_res.sampler = device->createSampler(sampler_desc);
  if (!g_res.white || !g_res.sampler) {
    REXLOG_ERROR("native_renderer: could not create the placeholder texture bindings");
    return false;
  }

  PlumeGuestExtent(&g_target_width, &g_target_height);
  if (!CreateTarget(device, g_res.target) || !CreateTarget(device, g_res.front)) {
    REXLOG_ERROR("native_renderer: could not create the {}x{} guest render target",
                 g_target_width, g_target_height);
    return false;
  }
  if (!CreateBlit(device)) {
    REXLOG_ERROR("native_renderer: could not create the present blit");
    return false;
  }

  g_res.ready = true;
  return true;
}

// Called before recording anything into the frame: a new frame has a new
// command list, so nothing recorded into the last one is bound any more, and
// its slot's arena is free again (the backend waited on it before handing the
// slot back).
RenderCommandList* BeginRecording() {
  RenderCommandList* commands = PlumeFrameCommands();
  if (commands == nullptr)
    return nullptr;
  RenderDevice* device = PlumeDevice();
  if (!EnsureResources(device))
    return nullptr;
  if (PlumeFrameSerial() != g_frame_serial) {
    g_frame_serial = PlumeFrameSerial();
    g_arena_slot = PlumeFrameSlot();
    for (ArenaBlock& block : g_arenas[g_arena_slot])
      block.used = 0;
    TextureMirrorBeginFrame(g_arena_slot);
    g_target_bound = false;
  }
  if (!g_targets_cleared) {
    g_targets_cleared = true;
    for (TrackedTexture* target : {&g_res.target, &g_res.front}) {
      target->Transition(commands, RenderTextureLayout::COLOR_WRITE);
      commands->setFramebuffer(target->framebuffer.get());
      commands->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    }
    g_target_bound = false;
  }
  return commands;
}

void BindTarget(RenderCommandList* commands) {
  if (g_target_bound && g_res.target.layout == RenderTextureLayout::COLOR_WRITE)
    return;
  g_res.target.Transition(commands, RenderTextureLayout::COLOR_WRITE);
  commands->setFramebuffer(g_res.target.framebuffer.get());
  // Shifted half a pixel down and right: the guest puts a pixel's centre on
  // the integer coordinate (D3D9 convention) and the host at the half, so the
  // same geometry lands half a pixel further along without it. See Eternal
  // Sonata's draw path for the derivation.
  commands->setViewports(RenderViewport(0.5f * float(g_target_width) / kGuestWidth,
                                        0.5f * float(g_target_height) / kGuestHeight,
                                        float(g_target_width), float(g_target_height), 0.0f, 1.0f));
  commands->setScissors(RenderRect(0, 0, int32_t(g_target_width), int32_t(g_target_height)));
  g_target_bound = true;
}

// ---------------------------------------------------------------------------
// Vertex declarations and formats.

struct VertexElement {
  uint16_t stream = 0;
  uint16_t offset = 0;
  uint32_t type = 0;
  uint8_t usage = 0;
  uint8_t usage_index = 0;
};

struct VertexDeclaration {
  VertexElement elements[kMaxDeclElements];
  uint32_t count = 0;
};

bool ReadDeclaration(const uint8_t* base, uint32_t decl, VertexDeclaration& out) {
  if (decl == 0)
    return false;
  out.count = Load32(base, decl + kDeclCount);
  if (out.count == 0 || out.count > kMaxDeclElements)
    return false;
  for (uint32_t i = 0; i < out.count; ++i) {
    const uint32_t e = decl + kDeclElements + kDeclElementBytes * i;
    out.elements[i].stream = uint16_t(Load32(base, e) >> 16);
    out.elements[i].offset = uint16_t(Load32(base, e) & 0xFFFFu);
    out.elements[i].type = Load32(base, e + 4);
    out.elements[i].usage = *rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), e + 9);
    out.elements[i].usage_index = *rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), e + 10);
  }
  return true;
}

// D3DDECLUSAGE -> the HLSL semantic xenos_hlsl.py declares inputs with.
constexpr const char* kUsageSemantics[] = {
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
    "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",   "DEPTH",    "SAMPLE",
};

// A declaration element's `type` is the hardware fetch word: data format in
// bits 0..5, signed at bit 8, unnormalised at bit 9, destination swizzle in
// bits 10..21. Only formats made of 32 bit units are served, because the
// vertices are swapped as plain dwords; the title's 2D draws use nothing else.
RenderFormat MapVertexFormat(uint32_t type) {
  const uint32_t format = type & 0x3Fu;
  const bool is_signed = ((type >> 8) & 1u) != 0;
  const bool is_integer = ((type >> 9) & 1u) != 0;
  switch (format) {
    case 6:  // k_8_8_8_8
      if (is_integer)
        return is_signed ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT;
      // D3DCOLOR declares the zyxw swizzle, because a k_8_8_8_8 fetch delivers
      // the low byte in x and D3DCOLOR stores blue there; the host format
      // whose components are already in that order is BGRA.
      if (!is_signed && ((type >> 10) & 0xFFFu) == 0x60Au)
        return RenderFormat::B8G8R8A8_UNORM;
      return is_signed ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case 36:
      return RenderFormat::R32_FLOAT;
    case 37:
      return RenderFormat::R32G32_FLOAT;
    case 57:
      return RenderFormat::R32G32B32_FLOAT;
    case 38:
      return RenderFormat::R32G32B32A32_FLOAT;
    default:
      return RenderFormat::UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// Primitive types. These are Xenos values, not D3D9 ones.

enum : uint32_t {
  kPrimLineList = 2,
  kPrimLineStrip = 3,
  kPrimTriangleList = 4,
  kPrimTriangleFan = 5,
  kPrimTriangleStrip = 6,
  kPrimQuadList = 13,
};

RenderPrimitiveTopology MapTopology(uint32_t primitive) {
  switch (primitive) {
    case kPrimLineList:
      return RenderPrimitiveTopology::LINE_LIST;
    case kPrimLineStrip:
      return RenderPrimitiveTopology::LINE_STRIP;
    case kPrimTriangleList:
    // Fans and quads have no host topology everywhere Plume runs, so both are
    // expanded into triangle lists through an index buffer.
    case kPrimTriangleFan:
    case kPrimQuadList:
      return RenderPrimitiveTopology::TRIANGLE_LIST;
    case kPrimTriangleStrip:
      return RenderPrimitiveTopology::TRIANGLE_STRIP;
    default:
      return RenderPrimitiveTopology::UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// Render state and blending.

// xenos::BlendFactor. The colour factors in the alpha half mean their alpha
// component, as on the console.
RenderBlend MapBlendFactor(uint32_t factor, bool alpha_half) {
  switch (factor) {
    case 0:
      return RenderBlend::ZERO;
    case 1:
      return RenderBlend::ONE;
    case 4:
      return alpha_half ? RenderBlend::SRC_ALPHA : RenderBlend::SRC_COLOR;
    case 5:
      return alpha_half ? RenderBlend::INV_SRC_ALPHA : RenderBlend::INV_SRC_COLOR;
    case 6:
      return RenderBlend::SRC_ALPHA;
    case 7:
      return RenderBlend::INV_SRC_ALPHA;
    case 8:
      return alpha_half ? RenderBlend::DEST_ALPHA : RenderBlend::DEST_COLOR;
    case 9:
      return alpha_half ? RenderBlend::INV_DEST_ALPHA : RenderBlend::INV_DEST_COLOR;
    case 10:
      return RenderBlend::DEST_ALPHA;
    case 11:
      return RenderBlend::INV_DEST_ALPHA;
    case 12:
    case 14:
      return RenderBlend::BLEND_FACTOR;
    case 13:
    case 15:
      return RenderBlend::INV_BLEND_FACTOR;
    case 16:
      return RenderBlend::SRC_ALPHA_SAT;
    default:
      return RenderBlend::ONE;
  }
}

RenderBlendOperation MapBlendOp(uint32_t op) {
  switch (op) {
    case 1:
      return RenderBlendOperation::SUBTRACT;
    case 2:
      return RenderBlendOperation::MIN;
    case 3:
      return RenderBlendOperation::MAX;
    case 4:
      return RenderBlendOperation::REV_SUBTRACT;
    default:
      return RenderBlendOperation::ADD;
  }
}

// RB_BLENDCONTROL0 has no enable bit: the hardware always blends, and "off" is
// ONE * src + ZERO * dst with ADD on both halves.
RenderBlendDesc MapBlend(uint32_t blend_control, uint32_t color_mask) {
  RenderBlendDesc blend = RenderBlendDesc::Copy();
  blend.renderTargetWriteMask = uint8_t(color_mask & 0xFu);
  const uint32_t src = blend_control & 0x1Fu, op = (blend_control >> 5) & 7u,
                 dst = (blend_control >> 8) & 0x1Fu;
  const uint32_t src_a = (blend_control >> 16) & 0x1Fu, op_a = (blend_control >> 21) & 7u,
                 dst_a = (blend_control >> 24) & 0x1Fu;
  if (src == 1 && dst == 0 && op == 0 && src_a == 1 && dst_a == 0 && op_a == 0)
    return blend;
  blend.blendEnabled = true;
  blend.blendOp = MapBlendOp(op);
  blend.blendOpAlpha = MapBlendOp(op_a);
  blend.srcBlend = MapBlendFactor(src, false);
  blend.dstBlend = MapBlendFactor(dst, false);
  blend.srcBlendAlpha = MapBlendFactor(src_a, true);
  blend.dstBlendAlpha = MapBlendFactor(dst_a, true);
  // MIN and MAX ignore the factors on the console.
  if (blend.blendOp == RenderBlendOperation::MIN || blend.blendOp == RenderBlendOperation::MAX)
    blend.srcBlend = blend.dstBlend = RenderBlend::ONE;
  if (blend.blendOpAlpha == RenderBlendOperation::MIN ||
      blend.blendOpAlpha == RenderBlendOperation::MAX)
    blend.srcBlendAlpha = blend.dstBlendAlpha = RenderBlend::ONE;
  return blend;
}

// ---------------------------------------------------------------------------
// The pipeline cache.

struct PipelineKey {
  int vertex_slot = 0;
  int pixel_slot = 0;
  uint32_t topology = 0;
  uint32_t stride = 0;
  uint32_t blend_control = 0;
  uint32_t color_mask = 0;
  uint32_t mode_cntl = 0;
  uint32_t element_count = 0;
  VertexElement elements[kMaxDeclElements];

  bool operator==(const PipelineKey& other) const {
    if (vertex_slot != other.vertex_slot || pixel_slot != other.pixel_slot ||
        topology != other.topology || stride != other.stride ||
        blend_control != other.blend_control || color_mask != other.color_mask ||
        mode_cntl != other.mode_cntl || element_count != other.element_count)
      return false;
    for (uint32_t i = 0; i < element_count; ++i) {
      const VertexElement& a = elements[i];
      const VertexElement& b = other.elements[i];
      if (a.stream != b.stream || a.offset != b.offset || a.type != b.type ||
          a.usage != b.usage || a.usage_index != b.usage_index)
        return false;
    }
    return true;
  }
};

struct GuestPipeline {
  PipelineKey key;
  std::unique_ptr<RenderPipeline> pipeline;
  const GuestShader* vertex = nullptr;
  const GuestShader* pixel = nullptr;
};

std::vector<std::unique_ptr<GuestPipeline>> g_pipelines;

RenderShader* EnsureShader(RenderDevice* device, const GuestShader& shader,
                           std::unique_ptr<RenderShader>& cached) {
  if (cached)
    return cached.get();
  const RenderShaderFormat format = PlumeShaderFormat();
  if (format == RenderShaderFormat::DXIL && shader.dxil_size != 0)
    cached = device->createShader(shader.dxil, shader.dxil_size, "main", format);
  else if (format == RenderShaderFormat::SPIRV && shader.spirv_size != 0)
    cached = device->createShader(shader.spirv, shader.spirv_size, "main", format);
  return cached.get();
}

const GuestPipeline* AcquirePipeline(RenderDevice* device, const PipelineKey& key,
                                     const VertexDeclaration& decl) {
  for (const auto& candidate : g_pipelines) {
    if (candidate->key == key)
      return candidate.get();
  }

  const GuestShader& vertex = GuestVertexShader(uint32_t(key.vertex_slot));
  const GuestShader& pixel = GuestPixelShader(uint32_t(key.pixel_slot));
  RenderShader* vertex_shader = EnsureShader(device, vertex, g_res.vertex_shaders[key.vertex_slot]);
  RenderShader* pixel_shader = EnsureShader(device, pixel, g_res.pixel_shaders[key.pixel_slot]);
  if (vertex_shader == nullptr || pixel_shader == nullptr) {
    Drop(kDropPipeline, uint32_t(key.vertex_slot) << 8 | uint32_t(key.pixel_slot));
    return nullptr;
  }

  // The input layout: the match the guest performs when it patches vfetch
  // from the bound declaration (shader input against declaration element, by
  // usage and usage index), except that the result is a host input layout.
  // Every element reads the one inline stream.
  std::vector<RenderInputElement> elements;
  for (uint32_t i = 0; i < vertex.input_count; ++i) {
    const GuestVertexInput& input = vertex.inputs[i];
    const VertexElement* match = nullptr;
    for (uint32_t e = 0; e < decl.count; ++e) {
      if (decl.elements[e].usage == input.usage &&
          decl.elements[e].usage_index == input.usage_index) {
        match = &decl.elements[e];
        break;
      }
    }
    if (match == nullptr || input.usage >= std::size(kUsageSemantics)) {
      Drop(kDropInput, uint32_t(input.usage) << 8 | input.usage_index);
      return nullptr;
    }
    const RenderFormat format = MapVertexFormat(match->type);
    if (format == RenderFormat::UNKNOWN) {
      Drop(kDropVertexFormat, match->type);
      return nullptr;
    }
    elements.emplace_back(kUsageSemantics[input.usage], input.usage_index, i, format, 0,
                          match->offset);
  }

  auto pipeline = std::make_unique<GuestPipeline>();
  pipeline->key = key;
  pipeline->vertex = &vertex;
  pipeline->pixel = &pixel;

  const RenderInputSlot slot(0, key.stride);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_res.layout.get();
  desc.vertexShader = vertex_shader;
  desc.pixelShader = pixel_shader;
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements.data();
  desc.inputElementsCount = uint32_t(elements.size());
  desc.primitiveTopology = RenderPrimitiveTopology(key.topology);
  desc.renderTargetFormat[0] = kTargetFormat;
  desc.renderTargetCount = 1;
  desc.renderTargetBlend[0] = MapBlend(key.blend_control, key.color_mask);
  // PA_SU_SC_MODE_CNTL: cull_front +0, cull_back +1, face +2 (1 = clockwise
  // front). Both culled draws nothing on the console; the host cannot say
  // that, and none of this title's 2D draws ask for it.
  const bool cull_front = (key.mode_cntl & 1u) != 0, cull_back = (key.mode_cntl & 2u) != 0;
  desc.cullMode = cull_front && !cull_back   ? RenderCullMode::FRONT
                  : cull_back && !cull_front ? RenderCullMode::BACK
                                             : RenderCullMode::NONE;
  desc.frontFace =
      (key.mode_cntl & 4u) != 0 ? RenderFrontFace::CLOCKWISE : RenderFrontFace::COUNTER_CLOCKWISE;
  pipeline->pipeline = device->createGraphicsPipeline(desc);
  if (!pipeline->pipeline) {
    Drop(kDropPipeline, uint32_t(key.vertex_slot) << 8 | uint32_t(key.pixel_slot));
    return nullptr;
  }
  REXLOG_DEBUG(
      "native_renderer: pipeline #{} vs={} ps={} prim={} stride={} blend={:08X} mask={:X} "
      "mode={:08X}",
      g_pipelines.size(), key.vertex_slot, key.pixel_slot, key.topology, key.stride,
      key.blend_control, key.color_mask, key.mode_cntl);
  g_pipelines.push_back(std::move(pipeline));
  return g_pipelines.back().get();
}

// ---------------------------------------------------------------------------
// Constants.

// A float bank out of the device's shadow, with the shader's literal pool
// overlaid on the top registers: the guest writes those through neither
// constant setter, so the shadow does not have them.
Allocation UploadFloatBank(RenderDevice* device, const uint8_t* base, uint32_t shadow,
                           const GuestShader& shader) {
  constexpr uint32_t kBankBytes = 256 * 16;
  const Allocation allocation = ArenaAllocate(device, kBankBytes);
  if (!allocation)
    return allocation;
  CopySwapped(allocation.cpu, base, shadow, kBankBytes);
  if (shader.has_literals && shader.literal_bytes != 0)
    std::memcpy(allocation.cpu + kBankBytes - shader.literal_bytes, shader.literals,
                shader.literal_bytes);
  return allocation;
}

// The guest's viewport transform folded with the pixel to clip mapping; see
// SCREEN_TRANSFORM in scripts/gen-guest-shaders.py. PA_CL_VTE_CNTL says which
// of the PA_CL_VPORT_* scales and offsets the hardware applies; a disabled one
// is 1 or 0. The render target's size comes from the bound surface, whose info
// word at +36 holds width - 1 from bit 18 and height - 1 in bits 3..17.
//
// The w handling is the 2D subset: with VTX_XY_FMT and VTX_W0_FMT as the 2D
// paths set them, w arrives as 1. A projected draw will want both honoured.
void ScreenTransform(const uint8_t* base, uint32_t device, float out[4]) {
  const uint32_t vte = Load32(base, device + Reg2200(0x2206));
  const float scale_x = (vte & 1u) ? LoadFloat(base, device + Reg2100(0x210F)) : 1.0f;
  const float offset_x = (vte & 2u) ? LoadFloat(base, device + Reg2100(0x2110)) : 0.0f;
  const float scale_y = (vte & 4u) ? LoadFloat(base, device + Reg2100(0x2111)) : 1.0f;
  const float offset_y = (vte & 8u) ? LoadFloat(base, device + Reg2100(0x2112)) : 0.0f;

  float width = float(kGuestWidth), height = float(kGuestHeight);
  if (const uint32_t surface = Load32(base, device + kBoundRenderTarget0)) {
    const uint32_t info = Load32(base, surface + 36);
    width = float((info >> 18) + 1);
    height = float(((info >> 3) & 0x7FFFu) + 1);
  }
  // ndc.x = screen.x * 2 / width - 1 and ndc.y = 1 - screen.y * 2 / height
  // (D3D clip space; the Vulkan flip is -fvk-invert-y's).
  out[0] = 2.0f * scale_x / width;
  out[1] = -2.0f * scale_y / height;
  out[2] = 2.0f * offset_x / width - 1.0f;
  out[3] = 1.0f - 2.0f * offset_y / height;

  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO(
        "native_renderer: first draw's screen transform: VTE {:08X}, viewport scale ({}, {}) "
        "offset ({}, {}), target {}x{}",
        vte, scale_x, scale_y, offset_x, offset_y, width, height);
  }
}

// XeDrawState, words in the order xenos_hlsl.py declares them.
constexpr uint32_t kDrawStateWords = 32;

void BuildDrawState(const uint8_t* base, uint32_t device, const uint32_t swizzles[kTextureSlots],
                    uint32_t words[kDrawStateWords]) {
  std::memset(words, 0, kDrawStateWords * 4);
  // RB_COLORCONTROL: alpha_func +0 (3 bits), alpha_test_enable +3. A disabled
  // test is sent as ALWAYS.
  const uint32_t color_control = Load32(base, device + Reg2200(0x2202));
  words[0] = (color_control & 8u) != 0 ? (color_control & 7u) : 7u;
  words[1] = Load32(base, device + Reg2100(0x210E));  // RB_ALPHA_REF, a float
  const float one = 1.0f;
  for (uint32_t i : {12u, 13u, 14u, 15u})
    std::memcpy(&words[i], &one, 4);  // xe_clip_scale, xe_texture0_scale
  // The fetch constants' swizzles, applied by xe_texture_fetch_post: the
  // mirror keeps every texel in Xenos component order (see
  // native_renderer_texture.h).
  for (uint32_t stage = 0; stage < kTextureSlots; ++stage)
    words[16 + stage] = swizzles[stage];
}

// The descriptor sets for the textures the pixel shader declares, out of the
// device's fetch constants. Uploads anything new through `commands`, so this
// runs before the target is bound. A slot the shader does not declare, or one
// the mirror cannot produce, samples the white placeholder.
const TextureBinding* BindTextures(RenderCommandList* commands, RenderDevice* device,
                                   uint8_t* base, uint32_t dev, uint32_t texture_mask,
                                   uint32_t swizzles[kTextureSlots]) {
  TextureBinding wanted;
  for (uint32_t stage = 0; stage < kTextureSlots; ++stage) {
    wanted.textures[stage] = g_res.white.get();
    wanted.samplers[stage] = g_res.sampler.get();
    swizzles[stage] = 0x688u;
    TextureFetch fetch;
    if ((texture_mask & (1u << stage)) == 0 ||
        !ReadTextureFetch(base, dev + kTextureFetchConstants + 24 * stage, fetch))
      continue;
    RenderTexture* texture = nullptr;
    if (fetch.base_address == g_resolve_address) {
      g_res.front.Transition(commands, RenderTextureLayout::SHADER_READ);
      texture = g_res.front.texture.get();
    } else {
      texture = MirrorTexture(commands, base, fetch);
    }
    if (texture == nullptr)
      continue;
    wanted.textures[stage] = texture;
    wanted.samplers[stage] = MirrorSampler(fetch);
    swizzles[stage] = fetch.swizzle;
  }

  for (const auto& binding : g_bindings) {
    if (std::equal(std::begin(wanted.textures), std::end(wanted.textures),
                   std::begin(binding->textures)) &&
        std::equal(std::begin(wanted.samplers), std::end(wanted.samplers),
                   std::begin(binding->samplers)))
      return binding.get();
  }
  auto binding = std::make_unique<TextureBinding>();
  std::copy(std::begin(wanted.textures), std::end(wanted.textures), binding->textures);
  std::copy(std::begin(wanted.samplers), std::end(wanted.samplers), binding->samplers);
  binding->texture_set = device->createDescriptorSet(SlotSetDesc(RenderDescriptorRangeType::TEXTURE));
  binding->sampler_set = device->createDescriptorSet(SlotSetDesc(RenderDescriptorRangeType::SAMPLER));
  if (!binding->texture_set || !binding->sampler_set)
    return nullptr;
  for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
    binding->texture_set->setTexture(slot, binding->textures[slot], RenderTextureLayout::SHADER_READ);
    binding->sampler_set->setSampler(slot, binding->samplers[slot]);
  }
  if (g_bindings.size() % 256 == 255)
    REXLOG_WARN("native_renderer: {} distinct texture bindings", g_bindings.size() + 1);
  g_bindings.push_back(std::move(binding));
  return g_bindings.back().get();
}

// ---------------------------------------------------------------------------
// The frame dump: NOCTURNE_DUMP_FRAMES=a,b,c writes the image a Resolve
// produced on those frames to nocturne_frame_<n>.ppm in the working directory.

struct FrameDump {
  std::vector<uint64_t> frames;
  std::unique_ptr<RenderBuffer> buffer;
  uint64_t pending_frame = 0;
  uint32_t pending_slot = 0;
  bool pending = false;
  bool parsed = false;
} g_dump;

bool WantDump(uint64_t frame) {
  if (!g_dump.parsed) {
    g_dump.parsed = true;
    if (const char* list = std::getenv("NOCTURNE_DUMP_FRAMES")) {
      for (const char* p = list; *p;) {
        char* end = nullptr;
        const unsigned long long value = std::strtoull(p, &end, 10);
        if (end == p)
          break;
        g_dump.frames.push_back(value);
        p = *end == ',' ? end + 1 : end;
      }
    }
  }
  for (uint64_t wanted : g_dump.frames) {
    if (wanted == frame)
      return true;
  }
  return false;
}

void RecordDump(RenderCommandList* commands, RenderDevice* device, uint64_t frame) {
  if (g_dump.pending || !WantDump(frame))
    return;
  const uint64_t kBytes = uint64_t(g_target_width) * g_target_height * 4;
  if (!g_dump.buffer)
    g_dump.buffer = device->createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));
  if (!g_dump.buffer)
    return;
  g_res.front.Transition(commands, RenderTextureLayout::COPY_SOURCE);
  commands->copyTextureRegion(
      RenderTextureCopyLocation::PlacedFootprint(g_dump.buffer.get(), kTargetFormat, g_target_width,
                                                 g_target_height, 1, g_target_width),
      RenderTextureCopyLocation::Subresource(g_res.front.texture.get()));
  g_dump.pending = true;
  g_dump.pending_frame = frame;
  g_dump.pending_slot = PlumeFrameSlot();
}

}  // namespace

GuestVertexFill GuestDrawInline(uint8_t* base, const GuestInlineDraw& draw) {
  if (draw.count == 0 || draw.stride == 0 || (draw.stride & 3u) != 0)
    return {};
  RenderCommandList* commands = BeginRecording();
  if (commands == nullptr)
    return {};
  RenderDevice* device = PlumeDevice();

  const uint32_t dev = draw.device;
  const int vertex_slot = draw.vertex_slot >= 0
                              ? draw.vertex_slot
                              : FindGuestVertexShader(base, Load32(base, dev + kBoundVertexShader));
  const int pixel_slot = FindGuestPixelShader(base, Load32(base, dev + kBoundPixelShader));
  if (vertex_slot < 0 || pixel_slot < 0) {
    Drop(kDropUnresolvedShader, Load32(base, dev + (vertex_slot < 0 ? kBoundVertexShader
                                                                     : kBoundPixelShader)));
    return {};
  }
  VertexDeclaration decl;
  if (!ReadDeclaration(base, Load32(base, dev + kBoundDeclaration), decl)) {
    Drop(kDropNoDeclaration, Load32(base, dev + kBoundDeclaration));
    return {};
  }
  const RenderPrimitiveTopology topology = MapTopology(draw.primitive);
  if (topology == RenderPrimitiveTopology::UNKNOWN) {
    Drop(kDropTopology, draw.primitive);
    return {};
  }

  PipelineKey key;
  key.vertex_slot = vertex_slot;
  key.pixel_slot = pixel_slot;
  key.topology = uint32_t(topology);
  key.stride = draw.stride;
  key.blend_control = Load32(base, dev + Reg2200(0x2201));  // RB_BLENDCONTROL0
  key.color_mask = Load32(base, dev + Reg2100(0x2104));     // RB_COLOR_MASK
  key.mode_cntl = Load32(base, dev + Reg2200(0x2205));      // PA_SU_SC_MODE_CNTL
  key.element_count = decl.count;
  std::copy(decl.elements, decl.elements + decl.count, key.elements);
  const GuestPipeline* pipeline = AcquirePipeline(device, key, decl);
  if (pipeline == nullptr)
    return {};

  // The vertices, out of the command buffer. Every element is made of 32 bit
  // units (MapVertexFormat refuses anything else), so the block swaps as
  // dwords.
  const uint32_t vertex_bytes = draw.count * draw.stride;
  const Allocation vertices = ArenaAllocate(device, vertex_bytes);
  if (!vertices) {
    Drop(kDropArena, vertex_bytes);
    return {};
  }
  GuestVertexFill fill;
  if (draw.host_vertices != nullptr)
    std::memcpy(vertices.cpu, draw.host_vertices, vertex_bytes);
  else
    fill = {vertices.cpu, draw.data, vertex_bytes};

  // Quads and fans, as triangle lists.
  uint32_t index_count = 0;
  Allocation indices;
  if (draw.primitive == kPrimQuadList || draw.primitive == kPrimTriangleFan) {
    index_count = draw.primitive == kPrimQuadList ? draw.count / 4 * 6
                                                  : (draw.count >= 3 ? (draw.count - 2) * 3 : 0);
    if (index_count == 0)
      return {};
    indices = ArenaAllocate(device, uint64_t(index_count) * 4);
    if (!indices) {
      Drop(kDropArena, index_count);
      return {};
    }
    auto* out = reinterpret_cast<uint32_t*>(indices.cpu);
    if (draw.primitive == kPrimQuadList) {
      for (uint32_t q = 0; q < draw.count / 4; ++q) {
        const uint32_t v = q * 4;
        const uint32_t quad[6] = {v, v + 1, v + 2, v, v + 2, v + 3};
        std::memcpy(out + q * 6, quad, sizeof(quad));
      }
    } else {
      for (uint32_t t = 0; t + 2 < draw.count; ++t) {
        out[t * 3] = 0;
        out[t * 3 + 1] = t + 1;
        out[t * 3 + 2] = t + 2;
      }
    }
  }

  // Constants. The bool and loop banks are shared by both stages (the pixel
  // shaders index bools from 128), and are one buffer.
  const Allocation vertex_floats = UploadFloatBank(device, base, dev + kVertexFloatShadow,
                                                   *pipeline->vertex);
  const Allocation pixel_floats = UploadFloatBank(device, base, dev + kPixelFloatShadow,
                                                  *pipeline->pixel);
  const Allocation bools = ArenaAllocate(device, 160);
  const Allocation draw_state = ArenaAllocate(device, kDrawStateWords * 4);
  const Allocation screen = ArenaAllocate(device, 16);
  if (!vertex_floats || !pixel_floats || !bools || !draw_state || !screen) {
    Drop(kDropArena, 0);
    return {};
  }
  CopySwapped(bools.cpu, base, dev + kBoolShadow, 32);
  CopySwapped(bools.cpu + 32, base, dev + kLoopShadow, 128);
  uint32_t swizzles[kTextureSlots];
  const TextureBinding* textures =
      BindTextures(commands, device, base, dev, pipeline->pixel->texture_mask, swizzles);
  if (textures == nullptr) {
    Drop(kDropTextures, 0);
    return {};
  }
  BuildDrawState(base, dev, swizzles, reinterpret_cast<uint32_t*>(draw_state.cpu));
  ScreenTransform(base, dev, reinterpret_cast<float*>(screen.cpu));

  BindTarget(commands);
  commands->setGraphicsPipelineLayout(g_res.layout.get());
  commands->setPipeline(pipeline->pipeline.get());
  commands->setGraphicsRootDescriptor(vertex_floats.ref, kRootVertexFloats);
  commands->setGraphicsRootDescriptor(bools.ref, kRootVertexBools);
  commands->setGraphicsRootDescriptor(pixel_floats.ref, kRootPixelFloats);
  commands->setGraphicsRootDescriptor(bools.ref, kRootPixelBools);
  commands->setGraphicsRootDescriptor(draw_state.ref, kRootDrawState);
  commands->setGraphicsRootDescriptor(screen.ref, kRootScreenTransform);
  commands->setGraphicsDescriptorSet(textures->texture_set.get(), kTextureDescriptorSet);
  commands->setGraphicsDescriptorSet(textures->sampler_set.get(), kSamplerDescriptorSet);

  const RenderInputSlot slot(0, draw.stride);
  const RenderVertexBufferView vertex_view(vertices.ref, vertex_bytes);
  commands->setVertexBuffers(0, &vertex_view, 1, &slot);
  if (index_count != 0) {
    const RenderIndexBufferView index_view(indices.ref, index_count * 4, RenderFormat::R32_UINT);
    commands->setIndexBuffer(&index_view);
    commands->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  } else {
    commands->drawInstanced(draw.count, 1, 0, 0);
  }
  return fill;
}

void GuestFillVertices(uint8_t* base, const GuestVertexFill& fill) {
  if (fill.destination != nullptr)
    CopySwapped(fill.destination, base, fill.source, fill.bytes);
}

void GuestResolve(uint8_t* base, uint32_t device, uint32_t flags, uint32_t destination,
                  uint32_t clear_color) {
  (void)device;
  RenderCommandList* commands = BeginRecording();
  if (commands == nullptr)
    return;

  TextureFetch fetch;
  if (destination != 0 && ReadTextureFetch(base, destination + kTextureObjectFetch, fetch) &&
      fetch.base_address != g_resolve_address) {
    g_resolve_address = fetch.base_address;
    REXLOG_INFO("native_renderer: resolving into a {}x{} format {} texture at {:08X}", fetch.width,
                fetch.height, fetch.format, fetch.base_address);
  }

  g_res.target.Transition(commands, RenderTextureLayout::COPY_SOURCE);
  g_res.front.Transition(commands, RenderTextureLayout::COPY_DEST);
  commands->copyTexture(g_res.front.texture.get(), g_res.target.texture.get());
  RecordDump(commands, PlumeDevice(), PlumeFrameSerial());
  g_target_bound = false;

  if ((flags & kResolveClearTarget) != 0) {
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (clear_color != 0) {
      for (uint32_t i = 0; i < 4; ++i)
        color[i] = LoadFloat(base, clear_color + 4 * i);
    }
    BindTarget(commands);
    commands->clearColor(0, RenderColor(color[0], color[1], color[2], color[3]));
  }
}

void GuestClear(uint8_t* base, uint32_t device, uint32_t flags, uint32_t rect, uint32_t color) {
  (void)device;
  RenderCommandList* commands = BeginRecording();
  if (commands == nullptr)
    return;
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (color != 0) {
    for (uint32_t i = 0; i < 4; ++i)
      rgba[i] = LoadFloat(base, color + 4 * i);
  }
  RenderRect host_rect(0, 0, int32_t(g_target_width), int32_t(g_target_height));
  if (rect != 0) {
    // Guest pixels to host pixels.
    const double sx = double(g_target_width) / kGuestWidth, sy = double(g_target_height) / kGuestHeight;
    host_rect = RenderRect(int32_t(Load32(base, rect) * sx), int32_t(Load32(base, rect + 4) * sy),
                           int32_t(std::ceil(Load32(base, rect + 8) * sx)),
                           int32_t(std::ceil(Load32(base, rect + 12) * sy)));
  }
  static uint32_t logged = 0;
  if (logged < 8) {
    ++logged;
    REXLOG_INFO("native_renderer: clear flags {:X} rect ({}, {}, {}, {}) colour ({}, {}, {}, {})",
                flags, host_rect.left, host_rect.top, host_rect.right, host_rect.bottom, rgba[0],
                rgba[1], rgba[2], rgba[3]);
  }
  BindTarget(commands);
  commands->clearColor(0, RenderColor(rgba[0], rgba[1], rgba[2], rgba[3]), &host_rect, 1);
}

void CompositeGuestImage(RenderCommandList* commands, const RenderFramebuffer* framebuffer,
                         uint32_t width, uint32_t height) {
  if (BeginRecording() != commands) {
    commands->setFramebuffer(framebuffer);
    commands->clearColor(0, RenderColor(0.05f, 0.06f, 0.10f, 1.0f));
    return;
  }
  g_res.front.Transition(commands, RenderTextureLayout::SHADER_READ);
  g_target_bound = false;
  commands->setFramebuffer(framebuffer);
  commands->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
  commands->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));
  commands->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

  // Letterboxed to the guest's aspect.
  float w = float(width), h = float(width) * kGuestHeight / kGuestWidth;
  if (h > float(height)) {
    h = float(height);
    w = float(height) * kGuestWidth / kGuestHeight;
  }
  commands->setViewports(RenderViewport((float(width) - w) * 0.5f, (float(height) - h) * 0.5f, w, h));
  commands->setGraphicsPipelineLayout(g_res.blit_layout.get());
  commands->setPipeline(g_res.blit_pipeline.get());
  commands->setGraphicsDescriptorSet(g_res.blit_set.get(), 0);
  commands->drawInstanced(3, 1, 0, 0);
  commands->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
}

void GuestFrameRetired(uint32_t slot) {
  if (!g_dump.pending || g_dump.pending_slot != slot)
    return;
  g_dump.pending = false;
  const auto* pixels = static_cast<const uint8_t*>(g_dump.buffer->map());
  if (pixels == nullptr)
    return;
  const std::string path = "nocturne_frame_" + std::to_string(g_dump.pending_frame) + ".ppm";
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fprintf(file, "P6\n%u %u\n255\n", g_target_width, g_target_height);
    std::vector<uint8_t> row(g_target_width * 3);
    for (uint32_t y = 0; y < g_target_height; ++y) {
      const uint8_t* src = pixels + size_t(y) * g_target_width * 4;
      for (uint32_t x = 0; x < g_target_width; ++x) {
        row[x * 3] = src[x * 4];
        row[x * 3 + 1] = src[x * 4 + 1];
        row[x * 3 + 2] = src[x * 4 + 2];
      }
      std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    REXLOG_INFO("native_renderer: wrote {}", path);
  }
  g_dump.buffer->unmap();
}

void ShutdownGuestDraw() {
  for (uint64_t i = 0; i < kDropCount; ++i) {
    if (g_drops[i] != 0)
      REXLOG_INFO("native_renderer: dropped {} draw(s): {}", g_drops[i], kDropNames[i]);
  }
  g_bindings.clear();
  ShutdownTextureMirror();
  g_resolve_address = 0;
  g_pipelines.clear();
  for (auto& arena : g_arenas)
    arena.clear();
  g_dump.buffer.reset();
  g_res = Resources();
  g_targets_cleared = false;
}

void GuestTargetExtent(uint32_t* width, uint32_t* height) {
  *width = g_res.ready ? g_target_width : 0;
  *height = g_res.ready ? g_target_height : 0;
}

void GuestResizeTarget(uint32_t width, uint32_t height) {
  if (!g_res.ready || (width == g_target_width && height == g_target_height))
    return;
  RenderDevice* device = PlumeDevice();
  // Bindings name the old front image by pointer, and the allocator is free to
  // hand its address to the new one, which the lookup would read as a hit.
  g_bindings.clear();
  g_res.target = TrackedTexture{};
  g_res.front = TrackedTexture{};
  g_target_width = width;
  g_target_height = height;
  if (!CreateTarget(device, g_res.target) || !CreateTarget(device, g_res.front)) {
    REXLOG_ERROR("native_renderer: could not rebuild the guest target at {}x{}", width, height);
    g_res.ready = false;
    return;
  }
  g_res.blit_set->setTexture(0, g_res.front.texture.get(), RenderTextureLayout::SHADER_READ);
  g_dump.buffer.reset();
  g_dump.pending = false;
  g_targets_cleared = false;
  g_target_bound = false;
  REXLOG_INFO("native_renderer: guest target is now {}x{}", width, height);
}

}  // namespace nocturne
