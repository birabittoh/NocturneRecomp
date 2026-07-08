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
// for the 3a clear-color proof.
constexpr uint32_t kPlaceholderWidth = 1280;
constexpr uint32_t kPlaceholderHeight = 720;

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

  REXGPU_INFO("NativeCommandProcessor: ready");
}

NativeCommandProcessor::~NativeCommandProcessor() {
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

  uint32_t shader_type_raw;
  uint32_t guest_address;
  uint32_t size_dwords;
  ShaderState state;
  if (immediate) {
    shader_type_raw = payload_dword(1);
    uint32_t start_size = payload_dword(2);
    size_dwords = start_size & 0xFFFF;
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
  if (draws_logged_ >= kMaxDrawsLogged) {
    return;
  }

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
  REXGPU_INFO(
      "NativeCommandProcessor: draw#{} {} prim_type={} source_select={} num_indices={} "
      "vs_addr={:08X} vs_size_dwords={} ps_addr={:08X} ps_size_dwords={} "
      "index_buffer_base={:08X} index_buffer_size_words={}",
      draws_logged_, name, static_cast<uint32_t>(initiator.prim_type),
      static_cast<uint32_t>(initiator.source_select), num_indices,
      active_vertex_shader_.guest_address, active_vertex_shader_.ucode.size(),
      active_pixel_shader_.guest_address, active_pixel_shader_.ucode.size(), index_buffer_base,
      index_buffer_size_words);

  TryTranslateActiveShaders();

  // Fetch constant 0 is overwhelmingly the vertex buffer / primary texture in
  // simple D3D9-style titles; log it to eyeball whether it looks sane
  // (nonzero base address, plausible pitch/format) before building real
  // vertex/texture upload on top of this decode.
  rex::graphics::xenos::xe_gpu_vertex_fetch_t vfetch0 = registers_.GetVertexFetch(0);
  rex::graphics::xenos::xe_gpu_texture_fetch_t tfetch0 = registers_.GetTextureFetch(0);
  uint32_t vfetch0_address = vfetch0.address;
  uint32_t vfetch0_size = vfetch0.size;
  uint32_t tfetch0_base_address = tfetch0.base_address;
  REXGPU_INFO(
      "NativeCommandProcessor:   vfetch[0] address={:08X} size_words={} tfetch[0] "
      "base_address_shifted={:08X}",
      vfetch0_address, vfetch0_size, tfetch0_base_address);
}

void NativeCommandProcessor::TryTranslateActiveShaders() {
  if (shaders_translated_once_) {
    return;
  }
  if (active_vertex_shader_.ucode.empty() || active_pixel_shader_.ucode.empty()) {
    return;
  }
  shaders_translated_once_ = true;

  using rex::graphics::Shader;
  using rex::graphics::SpirvShaderTranslator;

  // ucode_data_hash is only used for translation caching/dumping keys
  // upstream; irrelevant for this one-shot proof.
  Shader vertex_shader(rex::graphics::xenos::ShaderType::kVertex, /*ucode_data_hash=*/0,
                       active_vertex_shader_.ucode.data(), active_vertex_shader_.ucode.size(),
                       std::endian::big);
  Shader pixel_shader(rex::graphics::xenos::ShaderType::kPixel, /*ucode_data_hash=*/0,
                      active_pixel_shader_.ucode.data(), active_pixel_shader_.ucode.size(),
                      std::endian::big);

  rex::string::StringBuffer disasm_buffer;
  vertex_shader.AnalyzeUcode(disasm_buffer);
  pixel_shader.AnalyzeUcode(disasm_buffer);

  auto program_cntl = registers_.Get<rex::graphics::reg::SQ_PROGRAM_CNTL>();
  uint32_t vs_dynamic_regs =
      vertex_shader.GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg);
  uint32_t ps_dynamic_regs =
      pixel_shader.GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg);

  // Features(/*all=*/true): this milestone only needs SPIR-V bytes out, not
  // a device-tailored feature set yet -- pipeline/descriptor set creation
  // (next step) will need the real VulkanDevice-based Features instead.
  SpirvShaderTranslator::Features features(/*all=*/true);
  SpirvShaderTranslator translator(features, /*native_2x_msaa_with_attachments=*/false,
                                   /*native_2x_msaa_no_attachments=*/false,
                                   /*edram_fragment_shader_interlock=*/false);

  uint64_t vs_modification = translator.GetDefaultVertexShaderModification(vs_dynamic_regs);
  uint64_t ps_modification = translator.GetDefaultPixelShaderModification(ps_dynamic_regs);

  Shader::Translation* vs_translation = vertex_shader.GetOrCreateTranslation(vs_modification);
  Shader::Translation* ps_translation = pixel_shader.GetOrCreateTranslation(ps_modification);

  bool vs_ok = translator.TranslateAnalyzedShader(*vs_translation);
  bool ps_ok = translator.TranslateAnalyzedShader(*ps_translation);

  REXGPU_INFO(
      "NativeCommandProcessor: shader translation vs_ok={} vs_spirv_bytes={} ps_ok={} "
      "ps_spirv_bytes={}",
      vs_ok, vs_translation->translated_binary().size(), ps_ok,
      ps_translation->translated_binary().size());
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

  bool refreshed = presenter_->RefreshGuestOutput(
      kPlaceholderWidth, kPlaceholderHeight, kPlaceholderWidth, kPlaceholderHeight,
      [this](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vulkan_context =
            static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(
                context);

        const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
        const rex::ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
        VkDevice device = vulkan_device->device();

        // 5s timeout rather than an unbounded wait: a stuck fence should
        // surface as a loud, diagnosable error, not a silent hang of the
        // guest thread (which also blocks audio, since both ride the same
        // command-processor thread's forward progress).
        constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull;
        if (dfn.vkWaitForFences(device, 1, &submit_fence_, VK_TRUE, kFenceTimeoutNs) !=
            VK_SUCCESS) {
          REXGPU_ERROR("NativeCommandProcessor: timed out waiting for the previous frame's fence");
          return false;
        }
        dfn.vkResetFences(device, 1, &submit_fence_);

        VkCommandBufferBeginInfo begin_info;
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.pNext = nullptr;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_info.pInheritanceInfo = nullptr;
        dfn.vkBeginCommandBuffer(command_buffer_, &begin_info);

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

        // Distinctive deep-blue clear, proof the callback -> present loop
        // works end to end; milestone 3b replaces this with real draws.
        VkClearColorValue clear_color;
        clear_color.float32[0] = 0.05f;
        clear_color.float32[1] = 0.05f;
        clear_color.float32[2] = 0.35f;
        clear_color.float32[3] = 1.0f;
        dfn.vkCmdClearColorImage(command_buffer_, vulkan_context.image(),
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                                  &subresource_range);

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
        if (dfn.vkWaitForFences(device, 1, &submit_fence_, VK_TRUE, kFenceTimeoutNs) !=
            VK_SUCCESS) {
          REXGPU_ERROR("NativeCommandProcessor: timed out waiting for this frame's fence");
          return false;
        }

        static bool logged_first_present = false;
        if (!logged_first_present) {
          logged_first_present = true;
          REXGPU_INFO("NativeCommandProcessor: first frame presented");
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
}

}  // namespace nocturne
