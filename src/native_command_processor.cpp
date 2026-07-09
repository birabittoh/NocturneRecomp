// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "native_command_processor.h"

#include <cstring>
#include <thread>

#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

namespace nocturne {

// Intro sequence resolution isn't known until milestone 3b decodes the
// guest's render-target/viewport registers; hardcode a common 360 resolution
// for the 3a clear-color proof (still used as the swapchain's guest-output
// size in milestone 3b step 3 -- the offscreen color target below is what
// actually gets blitted into it).
constexpr uint32_t kPlaceholderWidth = 1280;
constexpr uint32_t kPlaceholderHeight = 720;

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

}  // namespace

NativeCommandProcessor::NativeCommandProcessor(rex::ui::vulkan::VulkanProvider* provider,
                                                rex::ui::Presenter* presenter)
    : provider_(provider), presenter_(presenter) {
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

  REXGPU_INFO("NativeCommandProcessor: ready (pipeline_resources_valid={})",
              pipeline_resources_valid_);
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
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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
    bindings[SpirvShaderTranslator::kConstantBufferSystem].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT;
    bindings[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[SpirvShaderTranslator::kConstantBufferBoolLoop].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[SpirvShaderTranslator::kConstantBufferFetch].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

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

  // Sets 2/3 (vertex/pixel textures): this milestone step doesn't implement
  // texture upload yet, so every shader is treated as sampling nothing --
  // the same empty (0-binding) layout works for both slots.
  {
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 0;
    layout_info.pBindings = nullptr;
    if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &empty_texture_layout_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the empty texture descriptor layout");
      return false;
    }
  }

  {
    VkDescriptorSetLayout set_layouts[4] = {shared_memory_layout_, constants_layout_,
                                            empty_texture_layout_, empty_texture_layout_};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 4;
    layout_info.pSetLayouts = set_layouts;
    if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the pipeline layout");
      return false;
    }
  }

  // Persistent descriptor pool: just the shared-memory set and the one
  // shared empty texture set, allocated once and never freed/reset.
  {
    VkDescriptorPoolSize pool_sizes[1] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the persistent descriptor pool");
      return false;
    }

    VkDescriptorSetLayout alloc_layouts[2] = {shared_memory_layout_, empty_texture_layout_};
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 2;
    alloc_info.pSetLayouts = alloc_layouts;
    VkDescriptorSet allocated_sets[2];
    if (dfn.vkAllocateDescriptorSets(device, &alloc_info, allocated_sets) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to allocate the persistent descriptor sets");
      return false;
    }
    shared_memory_set_ = allocated_sets[0];
    empty_texture_set_ = allocated_sets[1];
  }

  // Transient descriptor pool for per-draw constants sets -- reset (not
  // freed individually) once per frame in PresentFrame. Sized generously:
  // once kQuadList draws stopped being unconditionally skipped, a single
  // frame was observed issuing far more than the original 64-draw budget.
  {
    constexpr uint32_t kMaxDrawsPerFrame = 4096;
    VkDescriptorPoolSize pool_sizes[1] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         kMaxDrawsPerFrame * rex::graphics::SpirvShaderTranslator::kConstantBufferCount}};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kMaxDrawsPerFrame;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &transient_descriptor_pool_) !=
        VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the transient descriptor pool");
      return false;
    }
  }

  // Shared memory buffer: guest physical memory, mirrored 1:1 by byte
  // offset. Host-visible/coherent and persistently mapped so UpdateSharedMemory
  // is a plain memcpy with no explicit flush bookkeeping.
  {
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kSharedMemorySize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kUpload, shared_memory_buffer_,
            shared_memory_memory_)) {
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

  // Offscreen color target draws accumulate into across a frame.
  {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    image_info.extent = {kColorTargetWidth, kColorTargetHeight, 1};
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
    framebuffer_info.width = kColorTargetWidth;
    framebuffer_info.height = kColorTargetHeight;
    framebuffer_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer_) != VK_SUCCESS) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the framebuffer");
      return false;
    }
  }

  // Staging buffer for the image->image copy (see the field comment on
  // color_target_staging_buffer_). A2B10G10R10_UNORM_PACK32 is 4 bytes/texel.
  {
    color_target_staging_size_ =
        VkDeviceSize(kColorTargetWidth) * kColorTargetHeight * 4;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, color_target_staging_size_,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, color_target_staging_buffer_,
            color_target_staging_memory_)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to create the color target staging buffer");
      return false;
    }
  }

  return true;
}

void NativeCommandProcessor::DestroyPipelineResources() {
  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  for (auto& [key, pipeline] : pipeline_cache_) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
  pipeline_cache_.clear();
  for (auto& [key, entry] : shader_cache_) {
    if (entry.module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, entry.module, nullptr);
    }
  }
  shader_cache_.clear();

  if (color_target_staging_buffer_ != VK_NULL_HANDLE)
    dfn.vkDestroyBuffer(device, color_target_staging_buffer_, nullptr);
  if (color_target_staging_memory_ != VK_NULL_HANDLE)
    dfn.vkFreeMemory(device, color_target_staging_memory_, nullptr);

  if (framebuffer_ != VK_NULL_HANDLE) dfn.vkDestroyFramebuffer(device, framebuffer_, nullptr);
  if (render_pass_ != VK_NULL_HANDLE) dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
  if (color_target_view_ != VK_NULL_HANDLE)
    dfn.vkDestroyImageView(device, color_target_view_, nullptr);
  if (color_target_image_ != VK_NULL_HANDLE) dfn.vkDestroyImage(device, color_target_image_, nullptr);
  if (color_target_memory_ != VK_NULL_HANDLE) dfn.vkFreeMemory(device, color_target_memory_, nullptr);

  if (shared_memory_mapped_) dfn.vkUnmapMemory(device, shared_memory_memory_);
  if (shared_memory_buffer_ != VK_NULL_HANDLE)
    dfn.vkDestroyBuffer(device, shared_memory_buffer_, nullptr);
  if (shared_memory_memory_ != VK_NULL_HANDLE)
    dfn.vkFreeMemory(device, shared_memory_memory_, nullptr);

  if (transient_descriptor_pool_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorPool(device, transient_descriptor_pool_, nullptr);
  if (descriptor_pool_ != VK_NULL_HANDLE) dfn.vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
  if (pipeline_layout_ != VK_NULL_HANDLE) dfn.vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
  if (shared_memory_layout_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorSetLayout(device, shared_memory_layout_, nullptr);
  if (constants_layout_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorSetLayout(device, constants_layout_, nullptr);
  if (empty_texture_layout_ != VK_NULL_HANDLE)
    dfn.vkDestroyDescriptorSetLayout(device, empty_texture_layout_, nullptr);
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
    }
  }

  const char* name = info.type_info ? info.type_info->name : "";
  if (std::strcmp(name, "PM4_IM_LOAD") == 0) {
    OnShaderLoad(info, packet_base, /*immediate=*/false);
  } else if (std::strcmp(name, "PM4_IM_LOAD_IMMEDIATE") == 0) {
    OnShaderLoad(info, packet_base, /*immediate=*/true);
  } else if (info.type_info && info.type_info->category == rex::graphics::PacketCategory::kDraw) {
    OnDraw(info, packet_base);
  }

  if (info.type_info && info.type_info->category == rex::graphics::PacketCategory::kSwap) {
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
  // this stream (docs/native-renderer-headless-boot.md, milestone 3b step
  // 1); a garbage-decoded size here was observed making SpirvShaderTranslator
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

  TryDraw(initiator.prim_type, num_indices);
}

NativeCommandProcessor::TranslatedShader* NativeCommandProcessor::GetOrTranslateShader(
    rex::graphics::xenos::ShaderType type, const std::vector<uint32_t>& ucode,
    rex::graphics::Shader::HostVertexShaderType host_vertex_shader_type) {
  if (ucode.empty()) {
    return nullptr;
  }

  // host_vertex_shader_type only varies translation for vertex shaders (see
  // TryDraw's kRectangleList handling) -- fold it into the cache key so a
  // shader used both as a plain draw and a rectangle-list draw gets separate
  // translations/pipelines instead of aliasing onto the wrong one.
  uint64_t key = HashUcode(ucode) ^
                (type == rex::graphics::xenos::ShaderType::kVertex ? 0x1ull : 0x2ull) ^
                (uint64_t(host_vertex_shader_type) << 8);
  auto existing = shader_cache_.find(key);
  if (existing != shader_cache_.end()) {
    return &existing->second;
  }

  using rex::graphics::Shader;
  using rex::graphics::SpirvShaderTranslator;

  auto shader = std::make_unique<Shader>(type, key, ucode.data(), ucode.size(), std::endian::big);
  rex::string::StringBuffer disasm_buffer;
  shader->AnalyzeUcode(disasm_buffer);

  auto program_cntl = registers_.Get<rex::graphics::reg::SQ_PROGRAM_CNTL>();
  uint32_t num_reg = type == rex::graphics::xenos::ShaderType::kVertex ? program_cntl.vs_num_reg
                                                                       : program_cntl.ps_num_reg;
  uint32_t dynamic_regs = shader->GetDynamicAddressableRegisterCount(num_reg);

  SpirvShaderTranslator::Features features(provider_->vulkan_device());
  SpirvShaderTranslator translator(features, /*native_2x_msaa_with_attachments=*/false,
                                   /*native_2x_msaa_no_attachments=*/false,
                                   /*edram_fragment_shader_interlock=*/false);
  uint64_t modification =
      type == rex::graphics::xenos::ShaderType::kVertex
          ? translator.GetDefaultVertexShaderModification(dynamic_regs, host_vertex_shader_type)
          : translator.GetDefaultPixelShaderModification(dynamic_regs);
  Shader::Translation* translation = shader->GetOrCreateTranslation(modification);
  bool ok = translator.TranslateAnalyzedShader(*translation);

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

  auto [inserted, _] = shader_cache_.emplace(key, TranslatedShader{std::move(shader), module});
  return &inserted->second;
}

VkPipeline NativeCommandProcessor::GetOrCreatePipeline(TranslatedShader* vertex_shader,
                                                        TranslatedShader* pixel_shader,
                                                        VkPrimitiveTopology topology,
                                                        bool primitive_restart_enable) {
  uint64_t key = reinterpret_cast<uintptr_t>(vertex_shader) ^
                (reinterpret_cast<uintptr_t>(pixel_shader) * 3) ^ (uint64_t(topology) << 1) ^
                (primitive_restart_enable ? 0x8000000000000000ull : 0);
  auto existing = pipeline_cache_.find(key);
  if (existing != pipeline_cache_.end()) {
    return existing->second;
  }

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_shader->module;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = pixel_shader->module;

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

  VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state{};
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.dynamicStateCount = 2;
  dynamic_state.pDynamicStates = dynamic_states;

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
  pipeline_info.pDynamicState = &dynamic_state;
  pipeline_info.layout = pipeline_layout_;
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
    REXGPU_INFO("NativeCommandProcessor: created a graphics pipeline (topology={})",
                static_cast<uint32_t>(topology));
  }
  pipeline_cache_.emplace(key, pipeline);
  return pipeline;
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
  FreeTransientBuffers();
  dfn.vkResetDescriptorPool(device, transient_descriptor_pool_, 0);

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
  rp_begin.renderArea = {{0, 0}, {kColorTargetWidth, kColorTargetHeight}};
  rp_begin.clearValueCount = 1;
  rp_begin.pClearValues = &clear_value;
  dfn.vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{0.0f,
                      0.0f,
                      float(kColorTargetWidth),
                      float(kColorTargetHeight),
                      0.0f,
                      1.0f};
  VkRect2D scissor{{0, 0}, {kColorTargetWidth, kColorTargetHeight}};
  dfn.vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
  dfn.vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

  frame_active_ = true;
  frame_has_draws_ = false;
}

void NativeCommandProcessor::TryDraw(rex::graphics::xenos::PrimitiveType prim_type,
                                     uint32_t num_indices) {
  if (!pipeline_resources_valid_ || num_indices == 0) {
    return;
  }

  // Fetch-constant/register decode is known-unreliable for some draws in this
  // stream (see docs/native-renderer-headless-boot.md, milestone 3b step 1) --
  // a garbage-decoded num_indices was observed making the quad-list index
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

  VkPrimitiveTopology topology;
  if (is_quad_list) {
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

  TranslatedShader* vertex_shader = GetOrTranslateShader(
      rex::graphics::xenos::ShaderType::kVertex, active_vertex_shader_.ucode,
      is_rect_list ? rex::graphics::Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
                   : rex::graphics::Shader::HostVertexShaderType::kVertex);
  TranslatedShader* pixel_shader =
      GetOrTranslateShader(rex::graphics::xenos::ShaderType::kPixel, active_pixel_shader_.ucode);
  if (!vertex_shader || !pixel_shader || vertex_shader->module == VK_NULL_HANDLE ||
      pixel_shader->module == VK_NULL_HANDLE) {
    return;
  }

  VkPipeline pipeline =
      GetOrCreatePipeline(vertex_shader, pixel_shader, topology, /*primitive_restart_enable=*/
                         is_rect_list);
  if (pipeline == VK_NULL_HANDLE) {
    return;
  }

  EnsureFrameBegun();

  // Mirror only the vertex fetch constant ranges this shader pair actually
  // reads into shared memory (not a blanket copy) -- see UpdateSharedMemory.
  for (const auto& binding : vertex_shader->shader->vertex_bindings()) {
    rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch =
        registers_.GetVertexFetch(binding.fetch_constant);
    UpdateSharedMemory(fetch.address, fetch.size);
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
    VkBuffer buffer;
    VkDeviceMemory memory;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kUpload, buffer, memory)) {
      REXGPU_ERROR("NativeCommandProcessor: failed to allocate a constant buffer");
      return;
    }
    void* mapped = nullptr;
    dfn.vkMapMemory(device, memory, 0, size, 0, &mapped);
    std::memcpy(mapped, data, size);
    dfn.vkUnmapMemory(device, memory);
    frame_transient_buffers_.push_back({buffer, memory});

    VkDescriptorBufferInfo buffer_info{buffer, 0, size};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = constants_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  };

  // System constants: zeroed except the minimum for a simple opaque draw
  // with no guest viewport remap -- see spirv_translator.h's SystemConstants
  // and command_processor.cpp's UpdateSystemConstantValues for the full
  // real-backend population this approximates.
  SpirvShaderTranslator::SystemConstants system_constants{};
  system_constants.flags = uint32_t(1) << SpirvShaderTranslator::kSysFlag_XYDividedByW_Shift;
  system_constants.ndc_scale[0] = system_constants.ndc_scale[1] = system_constants.ndc_scale[2] =
      1.0f;
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

  VkDescriptorSet sets[4] = {shared_memory_set_, constants_set, empty_texture_set_,
                            empty_texture_set_};
  dfn.vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  dfn.vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                              0, 4, sets, 0, nullptr);
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
    dfn.vkUnmapMemory(device, index_memory);
    frame_transient_buffers_.push_back({index_buffer, index_memory});

    dfn.vkCmdBindIndexBuffer(command_buffer_, index_buffer, 0, VK_INDEX_TYPE_UINT32);
    dfn.vkCmdDrawIndexed(command_buffer_, uint32_t(indices.size()), 1, 0, 0, 0);
  };

  if (is_quad_list) {
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

void NativeCommandProcessor::PresentFrame() {
  if (command_buffer_ == VK_NULL_HANDLE) {
    return;
  }

  constexpr auto kMinFrameInterval = std::chrono::milliseconds(16);
  auto now = std::chrono::steady_clock::now();
  auto elapsed = now - last_present_time_;
  if (elapsed < kMinFrameInterval) {
    std::this_thread::sleep_for(kMinFrameInterval - elapsed);
  }
  last_present_time_ = std::chrono::steady_clock::now();

  // Guarantees a valid recording command buffer + cleared color target even
  // if this frame had zero (or all-skipped) draws.
  EnsureFrameBegun();

  const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
  const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  // Render pass's finalLayout is already TRANSFER_SRC_OPTIMAL, so
  // color_target_image_ needs no barrier before the blit below.
  dfn.vkCmdEndRenderPass(command_buffer_);

  bool refreshed = presenter_->RefreshGuestOutput(
      kPlaceholderWidth, kPlaceholderHeight, kPlaceholderWidth, kPlaceholderHeight,
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
        // are. This is exact (no scaling/format conversion needed) since
        // color_target_image_ is kColorTargetWidth x kColorTargetHeight ==
        // kPlaceholderWidth x kPlaceholderHeight and was deliberately
        // created in kGuestOutputFormat (VK_FORMAT_A2B10G10R10_UNORM_PACK32)
        // to match.
        static_assert(kColorTargetWidth == kPlaceholderWidth &&
                          kColorTargetHeight == kPlaceholderHeight,
                      "color target and guest output sizes must match for this staged copy");
        VkBufferImageCopy buffer_image_copy{};
        buffer_image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        buffer_image_copy.imageExtent = {kColorTargetWidth, kColorTargetHeight, 1};
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
}

}  // namespace nocturne
