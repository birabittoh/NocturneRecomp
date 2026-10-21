// nocturne - ReXGlue Recompiled Project
//
// See native_renderer_overlay.h.

#include "native_renderer_overlay.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>

#include "native_renderer_plume.h"
#include "native_renderer_plume_internal.h"
#ifdef _WIN32
#include "shaders/imguiVert.hlsl.dxil.h"
#include "shaders/imguiFrag.hlsl.dxil.h"
#endif
#include "shaders/imguiVert.hlsl.spirv.h"
#include "shaders/imguiFrag.hlsl.spirv.h"

namespace nocturne {
namespace {

using namespace plume;

// The overlay renders into the swap chain image directly, so this has to agree
// with the backend's swap chain format or pipeline creation is rejected. Asked
// rather than repeated, because the two disagreed on Android and validation
// caught it as an incompatible render pass on every overlay draw:
//
//   VUID-vkCmdDrawIndexed-renderPass-02684: pAttachments[0].format
//   (VK_FORMAT_R8G8B8A8_UNORM) != pAttachments[0].format (VK_FORMAT_B8G8R8A8_UNORM)

// D3D12 requires texture upload rows to be 256 byte aligned. Vulkan does not
// care, so aligning unconditionally is simply the safe common denominator.
constexpr uint32_t kUploadRowAlignment = 256;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

uint64_t AlignUp64(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// Where one batch's vertex and index data starts inside the frame's arena.
// Generous enough to satisfy both backends' view alignment rules without having
// to reason about them per format.
constexpr uint64_t kGeometryAlignment = 256;

// The four sampler combinations ui::ImmediateDrawer can ask for, as a flat
// index so they can live in one array and one descriptor set layout.
uint32_t SamplerIndex(rex::ui::ImmediateTextureFilter filter, bool repeated) {
  const uint32_t filter_bit = filter == rex::ui::ImmediateTextureFilter::kLinear ? 1u : 0u;
  return filter_bit | (repeated ? 2u : 0u);
}

struct PushConstants {
  float scale[2];
  float translate[2];
};

class PlumeImmediateDrawer;

// A texture plus the descriptor set that binds it. One set per texture rather
// than one rewritten per draw, because the overlay binds a handful of textures
// (the font atlas and a few icons) and rewriting a set mid-frame would need
// the frame to be done with the previous contents first.
class PlumeImmediateTexture final : public rex::ui::ImmediateTexture {
 public:
  PlumeImmediateTexture(uint32_t width, uint32_t height)
      : rex::ui::ImmediateTexture(width, height) {}

  std::unique_ptr<RenderTexture> texture;
  std::unique_ptr<RenderTextureView> view;
  std::unique_ptr<RenderDescriptorSet> descriptor_set;
};

class PlumeImmediateDrawer final : public rex::ui::ImmediateDrawer {
 public:
  ~PlumeImmediateDrawer() override = default;

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter filter,
                                                           bool is_repeated,
                                                           const uint8_t* data) override;

  void Begin(rex::ui::UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override;
  void Draw(const rex::ui::ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  // All GPU setup is here rather than in the constructor: the SDK builds the
  // drawer before the backend necessarily exists, and never calls an "enter
  // presenter" hook in this mode, so there is no other honest place for it.
  bool EnsureResources();

  // Sub-allocate one batch out of the frame's upload buffers, copying its
  // geometry in and pointing the two views at it. Returns false if the buffers
  // could not be grown, which is the only failure.
  bool UploadGeometry(const rex::ui::ImmediateDrawBatch& batch);

  RenderDevice* device_ = nullptr;
  bool initialized_ = false;
  bool initialization_failed_ = false;

  std::unique_ptr<RenderShader> vertex_shader_;
  std::unique_ptr<RenderShader> pixel_shader_;
  std::unique_ptr<RenderPipelineLayout> pipeline_layout_;
  std::unique_ptr<RenderPipeline> triangle_pipeline_;
  std::unique_ptr<RenderPipeline> line_pipeline_;
  std::unique_ptr<RenderSampler> samplers_[4];

  // Bound when a draw has no texture of its own, so the shader never has to
  // branch and there is only one pipeline.
  std::unique_ptr<PlumeImmediateTexture> white_texture_;

  // A frame's geometry arena. A batch is appended rather than written at zero:
  // the overlay records into the guest's command list, which does not execute
  // until the present, so every batch of the frame has to still be there when
  // it runs. Writing each one at offset zero left the whole frame reading the
  // last batch's vertices, which is invisible while imgui emits one draw list
  // and breaks the moment a second window (a combo popup, say) adds another.
  struct Arena {
    std::unique_ptr<RenderBuffer> vertex_buffer;
    std::unique_ptr<RenderBuffer> index_buffer;
    uint64_t vertex_capacity = 0;
    uint64_t index_capacity = 0;
    uint64_t vertex_used = 0;
    uint64_t index_used = 0;

    // Buffers replaced by a larger one mid-frame. Earlier batches are already
    // recorded against them, so they have to live until the slot comes round
    // again.
    std::vector<std::unique_ptr<RenderBuffer>> retired_buffers;
  };

  // One per frame slot: the previous frame is still on the GPU while this one
  // records, so sharing one arena let a changed overlay overwrite the vertices
  // that frame was drawing (visible as warping on Android's tilers). A slot's
  // arena is idle again once the present has waited on that slot's fence.
  Arena arenas_[kFramesInFlight];
  Arena* arena_ = &arenas_[0];

  RenderInputSlot input_slot_;
  RenderVertexBufferView vertex_buffer_view_;
  RenderIndexBufferView index_buffer_view_;

  // Per-frame state, valid between Begin and End.
  RenderCommandList* commands_ = nullptr;
  uint32_t target_width_ = 0;
  uint32_t target_height_ = 0;
  bool batch_has_indices_ = false;
  const RenderPipeline* bound_pipeline_ = nullptr;
};

bool PlumeImmediateDrawer::EnsureResources() {
  if (initialized_)
    return true;
  if (initialization_failed_)
    return false;

  device_ = PlumeDevice();
  if (device_ == nullptr)
    return false;  // Backend not up yet; try again next call. Not a failure.

  // Set 0 is the texture plus its sampler. The sampler is a plain descriptor
  // rather than an immutable one so that one layout covers all four filter and
  // wrap combinations.
  RenderDescriptorRange ranges[] = {
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
      RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  RenderDescriptorSetDesc descriptor_set_desc(ranges, 2);

  const RenderPushConstantRange push_constants(
      0, 0, 0, sizeof(PushConstants),
      RenderShaderStageFlag::VERTEX);

  RenderPipelineLayoutDesc layout_desc;
  layout_desc.pushConstantRanges = &push_constants;
  layout_desc.pushConstantRangesCount = 1;
  layout_desc.descriptorSetDescs = &descriptor_set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = true;
  pipeline_layout_ = device_->createPipelineLayout(layout_desc);

  const RenderShaderFormat shader_format = PlumeShaderFormat();
#ifdef _WIN32
  if (shader_format == RenderShaderFormat::DXIL) {
    vertex_shader_ = device_->createShader(imguiVertBlobDXIL, sizeof(imguiVertBlobDXIL), "VSMain",
                                           shader_format);
    pixel_shader_ = device_->createShader(imguiFragBlobDXIL, sizeof(imguiFragBlobDXIL), "PSMain",
                                          shader_format);
  } else
#endif
      if (shader_format == RenderShaderFormat::SPIRV) {
    vertex_shader_ = device_->createShader(imguiVertBlobSPIRV, sizeof(imguiVertBlobSPIRV),
                                           "VSMain", shader_format);
    pixel_shader_ = device_->createShader(imguiFragBlobSPIRV, sizeof(imguiFragBlobSPIRV), "PSMain",
                                          shader_format);
  }
  if (!vertex_shader_ || !pixel_shader_) {
    REXLOG_ERROR(
        "native_renderer: no overlay shader blob for Plume's shader format, so the SDK's "
        "overlays will not draw");
    initialization_failed_ = true;
    return false;
  }

  // ui::ImmediateVertex is float2 position, float2 uv, packed RGBA8 colour --
  // deliberately identical to ImDrawVert, so this describes both.
  static const RenderInputElement kElements[] = {
      RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0,
                         offsetof(rex::ui::ImmediateVertex, x)),
      RenderInputElement("TEXCOORD", 0, 1, RenderFormat::R32G32_FLOAT, 0,
                         offsetof(rex::ui::ImmediateVertex, u)),
      RenderInputElement("COLOR", 0, 2, RenderFormat::R8G8B8A8_UNORM, 0,
                         offsetof(rex::ui::ImmediateVertex, color)),
  };
  input_slot_ = RenderInputSlot(0, sizeof(rex::ui::ImmediateVertex));

  RenderGraphicsPipelineDesc pipeline_desc;
  pipeline_desc.pipelineLayout = pipeline_layout_.get();
  pipeline_desc.vertexShader = vertex_shader_.get();
  pipeline_desc.pixelShader = pixel_shader_.get();
  pipeline_desc.inputSlots = &input_slot_;
  pipeline_desc.inputSlotsCount = 1;
  pipeline_desc.inputElements = kElements;
  pipeline_desc.inputElementsCount = uint32_t(std::size(kElements));
  pipeline_desc.renderTargetFormat[0] = PlumeSwapChainFormat();
  pipeline_desc.renderTargetCount = 1;
  pipeline_desc.cullMode = RenderCullMode::NONE;
  pipeline_desc.depthEnabled = false;
  pipeline_desc.depthWriteEnabled = false;

  // Straight alpha blending, which is what imgui's vertex colours and font
  // atlas assume.
  pipeline_desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();

  pipeline_desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
  triangle_pipeline_ = device_->createGraphicsPipeline(pipeline_desc);
  pipeline_desc.primitiveTopology = RenderPrimitiveTopology::LINE_LIST;
  line_pipeline_ = device_->createGraphicsPipeline(pipeline_desc);
  if (!triangle_pipeline_ || !line_pipeline_) {
    REXLOG_ERROR("native_renderer: could not create the overlay pipelines");
    initialization_failed_ = true;
    return false;
  }

  for (uint32_t i = 0; i < 4; ++i) {
    const bool linear = (i & 1) != 0;
    const bool repeated = (i & 2) != 0;
    RenderSamplerDesc sampler_desc;
    sampler_desc.minFilter = linear ? RenderFilter::LINEAR : RenderFilter::NEAREST;
    sampler_desc.magFilter = sampler_desc.minFilter;
    sampler_desc.mipmapMode = RenderMipmapMode::NEAREST;
    const RenderTextureAddressMode mode =
        repeated ? RenderTextureAddressMode::WRAP : RenderTextureAddressMode::CLAMP;
    sampler_desc.addressU = mode;
    sampler_desc.addressV = mode;
    sampler_desc.addressW = mode;
    samplers_[i] = device_->createSampler(sampler_desc);
  }

  initialized_ = true;

  // Only now, because it goes through CreateTexture, which needs everything
  // above. One opaque white texel, so an untextured draw is just a textured
  // draw that multiplies by one.
  const uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  auto created = CreateTexture(1, 1, rex::ui::ImmediateTextureFilter::kNearest, false, white);
  white_texture_.reset(static_cast<PlumeImmediateTexture*>(created.release()));
  if (!white_texture_) {
    REXLOG_ERROR("native_renderer: could not create the overlay's white fallback texture");
    initialization_failed_ = true;
    initialized_ = false;
    return false;
  }

  REXLOG_DEBUG("native_renderer: overlay drawer up on Plume; the SDK's overlays are back");
  return true;
}

std::unique_ptr<rex::ui::ImmediateTexture> PlumeImmediateDrawer::CreateTexture(
    uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter, bool is_repeated,
    const uint8_t* data) {
  // Required by the SDK's contract for detached mode: the ImGui font atlas is
  // uploaded lazily on the first draw, which can land before the device exists.
  // Returning null is the documented answer; failing is not.
  if (!EnsureResources())
    return nullptr;
  if (width == 0 || height == 0)
    return nullptr;

  auto texture = std::make_unique<PlumeImmediateTexture>(width, height);
  texture->texture = device_->createTexture(
      RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R8G8B8A8_UNORM));
  if (!texture->texture)
    return nullptr;

  if (data != nullptr) {
    const uint32_t row_bytes = width * 4;
    const uint32_t upload_pitch = AlignUp(row_bytes, kUploadRowAlignment);
    auto staging =
        device_->createBuffer(RenderBufferDesc::UploadBuffer(uint64_t(upload_pitch) * height));
    if (!staging)
      return nullptr;

    auto* mapped = static_cast<uint8_t*>(staging->map());
    if (mapped == nullptr)
      return nullptr;
    for (uint32_t y = 0; y < height; ++y)
      std::memcpy(mapped + size_t(y) * upload_pitch, data + size_t(y) * row_bytes, row_bytes);
    staging->unmap();

    // A one-shot list of its own rather than the frame's: this can be called
    // from inside the frame's recording (the lazy font upload again), and
    // recording a copy into a list that is mid-draw would be an ordering
    // hazard that a barrier alone does not fix.
    RenderCommandQueue* queue = PlumeQueue();
    auto upload_commands = queue->createCommandList();
    auto upload_fence = device_->createCommandFence();
    upload_commands->begin();
    upload_commands->barriers(RenderBarrierStage::COPY,
                              RenderTextureBarrier(texture->texture.get(),
                                                   RenderTextureLayout::COPY_DEST));
    upload_commands->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(texture->texture.get()),
        RenderTextureCopyLocation::PlacedFootprint(staging.get(), RenderFormat::R8G8B8A8_UNORM,
                                                   width, height, 1, upload_pitch / 4));
    upload_commands->barriers(RenderBarrierStage::GRAPHICS,
                              RenderTextureBarrier(texture->texture.get(),
                                                   RenderTextureLayout::SHADER_READ));
    upload_commands->end();

    const RenderCommandList* submit = upload_commands.get();
    queue->executeCommandLists(&submit, 1, nullptr, 0, nullptr, 0, upload_fence.get());
    queue->waitForCommandFence(upload_fence.get());
  }

  RenderDescriptorRange ranges[] = {
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
      RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  texture->descriptor_set = device_->createDescriptorSet(RenderDescriptorSetDesc(ranges, 2));
  if (!texture->descriptor_set)
    return nullptr;
  texture->descriptor_set->setTexture(0, texture->texture.get(), RenderTextureLayout::SHADER_READ);
  texture->descriptor_set->setSampler(1, samplers_[SamplerIndex(filter, is_repeated)].get());

  return texture;
}

bool PlumeImmediateDrawer::UploadGeometry(const rex::ui::ImmediateDrawBatch& batch) {
  const uint64_t vertex_bytes = uint64_t(batch.vertex_count) * sizeof(rex::ui::ImmediateVertex);
  const uint64_t index_bytes = uint64_t(std::max(batch.index_count, 0)) * sizeof(uint16_t);

  // A batch is bound by a view of its own, so the only alignment that matters
  // is the one a view start needs. 256 covers both backends and is a multiple
  // of the vertex stride's and the index width's requirements.
  Arena& arena = *arena_;
  const uint64_t vertex_offset = AlignUp64(arena.vertex_used, kGeometryAlignment);
  const uint64_t index_offset = AlignUp64(arena.index_used, kGeometryAlignment);

  if (vertex_offset + vertex_bytes > arena.vertex_capacity) {
    // Round up generously so a frame that grows by one widget does not
    // reallocate; imgui's geometry size is stable after a few frames.
    const uint64_t wanted =
        std::max<uint64_t>(std::max<uint64_t>(arena.vertex_capacity * 2, 64 * 1024),
                           (vertex_offset + vertex_bytes) * 2);
    auto grown = device_->createBuffer(RenderBufferDesc::UploadBuffer(wanted, RenderBufferFlag::VERTEX));
    if (!grown)
      return false;
    if (arena.vertex_buffer)
      arena.retired_buffers.push_back(std::move(arena.vertex_buffer));
    arena.vertex_buffer = std::move(grown);
    arena.vertex_capacity = wanted;
    arena.vertex_used = 0;
    return UploadGeometry(batch);  // Re-run against the new buffer, from zero.
  }
  if (index_bytes != 0 && index_offset + index_bytes > arena.index_capacity) {
    const uint64_t wanted = std::max<uint64_t>(
        std::max<uint64_t>(arena.index_capacity * 2, 32 * 1024), (index_offset + index_bytes) * 2);
    auto grown = device_->createBuffer(RenderBufferDesc::UploadBuffer(wanted, RenderBufferFlag::INDEX));
    if (!grown)
      return false;
    if (arena.index_buffer)
      arena.retired_buffers.push_back(std::move(arena.index_buffer));
    arena.index_buffer = std::move(grown);
    arena.index_capacity = wanted;
    arena.index_used = 0;
    return UploadGeometry(batch);
  }

  if (vertex_bytes != 0) {
    auto* mapped = static_cast<uint8_t*>(arena.vertex_buffer->map());
    if (mapped == nullptr)
      return false;
    std::memcpy(mapped + vertex_offset, batch.vertices, size_t(vertex_bytes));
    arena.vertex_buffer->unmap();
  }
  arena.vertex_used = vertex_offset + vertex_bytes;
  vertex_buffer_view_ =
      RenderVertexBufferView(arena.vertex_buffer->at(vertex_offset), uint32_t(vertex_bytes));

  if (index_bytes != 0) {
    auto* mapped = static_cast<uint8_t*>(arena.index_buffer->map());
    if (mapped == nullptr)
      return false;
    std::memcpy(mapped + index_offset, batch.indices, size_t(index_bytes));
    arena.index_buffer->unmap();
    arena.index_used = index_offset + index_bytes;
    index_buffer_view_ = RenderIndexBufferView(arena.index_buffer->at(index_offset),
                                               uint32_t(index_bytes), RenderFormat::R16_UINT);
  }
  return true;
}

void PlumeImmediateDrawer::Begin(rex::ui::UIDrawContext& ui_draw_context,
                                 float coordinate_space_width, float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  commands_ = nullptr;
  bound_pipeline_ = nullptr;

  // The frame that last recorded into this slot has been fence waited, so its
  // arena, including anything replaced mid-frame, is unreferenced.
  arena_ = &arenas_[PlumeFrameSlot()];
  arena_->retired_buffers.clear();
  arena_->vertex_used = 0;
  arena_->index_used = 0;

  if (!EnsureResources())
    return;

  auto& context = static_cast<PlumeUIDrawContext&>(ui_draw_context);
  commands_ = context.commands();
  target_width_ = uint32_t(this->coordinate_space_width());
  target_height_ = uint32_t(this->coordinate_space_height());
  if (commands_ == nullptr || target_width_ == 0 || target_height_ == 0) {
    commands_ = nullptr;
    return;
  }

  commands_->setGraphicsPipelineLayout(pipeline_layout_.get());

  // Pixels to clip space. Y is negated because the overlay's origin is top
  // left; the Vulkan half of that flip is handled by -fvk-invert-y at compile
  // time, so this is written once for both backends.
  PushConstants push;
  push.scale[0] = 2.0f / float(target_width_);
  push.scale[1] = -2.0f / float(target_height_);
  push.translate[0] = -1.0f;
  push.translate[1] = 1.0f;
  commands_->setGraphicsPushConstants(0, &push, 0, sizeof(push));
}

void PlumeImmediateDrawer::BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) {
  batch_has_indices_ = false;
  if (commands_ == nullptr || batch.vertex_count <= 0)
    return;

  if (!UploadGeometry(batch)) {
    commands_ = nullptr;
    return;
  }

  // Both bindings are baked into the command list as it records, so each batch
  // keeps the views it was recorded with. That is what makes appending safe.
  commands_->setVertexBuffers(0, &vertex_buffer_view_, 1, &input_slot_);
  if (batch.indices != nullptr && batch.index_count > 0) {
    commands_->setIndexBuffer(&index_buffer_view_);
    batch_has_indices_ = true;
  }
}

void PlumeImmediateDrawer::Draw(const rex::ui::ImmediateDraw& draw) {
  if (commands_ == nullptr || draw.count <= 0)
    return;

  const RenderPipeline* pipeline =
      draw.primitive_type == rex::ui::ImmediatePrimitiveType::kLines ? line_pipeline_.get()
                                                                     : triangle_pipeline_.get();
  if (pipeline != bound_pipeline_) {
    commands_->setPipeline(pipeline);
    bound_pipeline_ = pipeline;
  }

  // The base class does the clamping into render target coordinates and tells
  // us when the result is empty, which also covers the inverted-rect case its
  // header documents as "not drawing".
  uint32_t left = 0, top = 0, width = target_width_, height = target_height_;
  if (draw.scissor) {
    if (!ScissorToRenderTarget(draw, left, top, width, height))
      return;
  }
  commands_->setScissors(RenderRect(int32_t(left), int32_t(top), int32_t(left + width),
                                    int32_t(top + height)));

  auto* texture = static_cast<PlumeImmediateTexture*>(draw.texture);
  RenderDescriptorSet* set =
      texture && texture->descriptor_set ? texture->descriptor_set.get()
                                         : white_texture_->descriptor_set.get();
  commands_->setGraphicsDescriptorSet(set, 0);

  if (batch_has_indices_) {
    commands_->drawIndexedInstanced(uint32_t(draw.count), 1, uint32_t(draw.index_offset),
                                    draw.base_vertex, 0);
  } else {
    commands_->drawInstanced(uint32_t(draw.count), 1, uint32_t(draw.base_vertex), 0);
  }
}

void PlumeImmediateDrawer::EndDrawBatch() { batch_has_indices_ = false; }

void PlumeImmediateDrawer::End() {
  commands_ = nullptr;
  bound_pipeline_ = nullptr;
  ImmediateDrawer::End();
}

}  // namespace

std::unique_ptr<rex::ui::ImmediateDrawer> CreatePlumeImmediateDrawer() {
  // Constructed presenter-less and with no GPU work done, per the SDK's
  // contract for this mode: the device may not exist yet, and the first
  // EnsureResources call that finds one wins.
  return std::make_unique<PlumeImmediateDrawer>();
}

}  // namespace nocturne
