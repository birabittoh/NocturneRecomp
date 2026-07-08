// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "native_command_processor.h"

#include <thread>

#include <rex/logging.h>
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

  if (info.type_info->category == rex::graphics::PacketCategory::kSwap) {
    PresentFrame();
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
