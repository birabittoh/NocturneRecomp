// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>

namespace nocturne {

// Native renderer phase 3, milestone 3a: the SOTN-specific consumer of the
// decoded PM4 stream forwarded by KernelState::SetNativeGpuCommandCallback
// (see docs/native-renderer-headless-boot.md, "Phase 1 revisited" for how
// that decode is produced). This milestone only proves the callback ->
// interpreter -> present loop: it tracks register writes into a RegisterFile
// (state milestone 3b will need) and, on a swap packet, clears the Phase 2
// swapchain's guest-output image to a distinctive color via
// Presenter::RefreshGuestOutput. Real draws/shaders/textures are milestone 3b.
class NativeCommandProcessor {
 public:
  NativeCommandProcessor(rex::ui::vulkan::VulkanProvider* provider,
                          rex::ui::Presenter* presenter);
  ~NativeCommandProcessor();

  NativeCommandProcessor(const NativeCommandProcessor&) = delete;
  NativeCommandProcessor& operator=(const NativeCommandProcessor&) = delete;

  // Bound as KernelState's NativeGpuCommandCallback. Called from the guest's
  // command-processor thread; packet_base is only valid for the duration of
  // the call.
  void OnPacket(const rex::graphics::PacketInfo& info, const uint8_t* packet_base);

 private:
  void PresentFrame();

  // Milestone 3b step 1 (decode-only): parse PM4_IM_LOAD/_IMMEDIATE and
  // PM4_DRAW_INDX/_2 payloads -- which PacketDisassembler doesn't turn into
  // PacketAction entries, unlike plain register writes -- and log a summary
  // of the first few draws (shader addresses/sizes, primitive type, index
  // count, fetch constants referenced) to verify the decode is right before
  // building shader translation/texture upload/real drawing on top of it.
  void OnShaderLoad(const rex::graphics::PacketInfo& info, const uint8_t* packet_base,
                    bool immediate);
  void OnDraw(const rex::graphics::PacketInfo& info, const uint8_t* packet_base);

  // Milestone 3b step 2: try translating whatever vertex+pixel shader pair is
  // active on the first draw, to prove SpirvShaderTranslator integration
  // works (produces nonempty SPIR-V) before building descriptor sets, a
  // pipeline, and real drawing on top of it. One-shot, gated by
  // shaders_translated_once_.
  void TryTranslateActiveShaders();

  rex::ui::vulkan::VulkanProvider* provider_;
  rex::ui::Presenter* presenter_;

  rex::graphics::RegisterFile registers_;

  struct ShaderState {
    uint32_t guest_address = 0;
    // Raw microcode dwords exactly as captured (still guest/big-endian --
    // rex::graphics::Shader's constructor does the byteswap itself given
    // std::endian::big).
    std::vector<uint32_t> ucode;
  };
  ShaderState active_vertex_shader_;
  ShaderState active_pixel_shader_;
  uint32_t draws_logged_ = 0;
  static constexpr uint32_t kMaxDrawsLogged = 10;
  bool shaders_translated_once_ = false;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence submit_fence_ = VK_NULL_HANDLE;

  // HeadlessRingWaitBypass (docs/native-renderer-headless-boot.md) removes
  // all real GPU backpressure from the guest's frame loop, so without this it
  // free-runs at thousands of "frames" per second -- pace it to something
  // resembling real hardware so per-frame debug logging and CPU usage stay
  // sane. Milestone 3b may replace this with real vsync-driven pacing.
  std::chrono::steady_clock::time_point last_present_time_{};
};

}  // namespace nocturne
