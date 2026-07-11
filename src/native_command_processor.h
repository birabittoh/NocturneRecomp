// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
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

  // Milestone 3b step 3 (+ quad-list follow-up): attempts the real draw
  // (pipeline bind, descriptor sets, shared-memory-backed vertex fetch) for
  // standard primitive topologies, plus rex::graphics::xenos::PrimitiveType::
  // kQuadList via a host-synthesized triangle-list index buffer (two
  // triangles per quad). kRectangleList isn't a native Vulkan primitive and
  // needs translator-side handling (Shader::HostVertexShaderType::
  // kRectangleListAsTriangleStrip) not implemented yet -- skipped with a
  // rate-limited log instead of guessing at an incorrect mapping.
  void TryDraw(rex::graphics::xenos::PrimitiveType prim_type, uint32_t num_indices);

  // Milestone 3b step 3: build the fixed descriptor set layouts / pipeline
  // layout SpirvShaderTranslator's output expects (shared-memory SSBO at set
  // 0, 5 constant uniform buffers at set 1, empty texture sets at 2/3 for
  // shaders that don't sample -- see command_processor.cpp's Vulkan backend
  // for the layout this replicates), plus an offscreen color target/render
  // pass draws accumulate into across a frame, composited into the Phase 2
  // swapchain on swap instead of milestone 3a's flat clear.
  bool InitializePipelineResources();
  void DestroyPipelineResources();

  // Gets (translating + creating a VkShaderModule on first use) the shader
  // for the given raw ucode, or nullptr if translation failed. Cached by a
  // hash of the ucode bytes so repeated draws with the same shader (the
  // overwhelmingly common case -- shaders are loaded once, then reused
  // across many draws) don't re-translate.
  struct TranslatedShader {
    std::unique_ptr<rex::graphics::Shader> shader;
    VkShaderModule module = VK_NULL_HANDLE;
  };
  TranslatedShader* GetOrTranslateShader(
      rex::graphics::xenos::ShaderType type, const std::vector<uint32_t>& ucode,
      rex::graphics::Shader::HostVertexShaderType host_vertex_shader_type =
          rex::graphics::Shader::HostVertexShaderType::kVertex);

  // Gets (building on first use) a VkPipeline for this exact vertex/pixel
  // shader pair + primitive topology.
  VkPipeline GetOrCreatePipeline(TranslatedShader* vertex_shader, TranslatedShader* pixel_shader,
                                 VkPrimitiveTopology topology, bool primitive_restart_enable);

  // Milestone 3b step 7 (texture upload): gets (uploading on first use) a
  // sampled VkImage + descriptor set for the given texture fetch constant
  // slot, or nullptr if the fetch constant describes something not yet
  // supported (tiled, mipped/array/3D, or a format outside the small
  // allow-list -- see native_command_processor.cpp's GetOrUploadTexture) or
  // is simply unbound (base_address == 0). Scoped deliberately narrow: only
  // untiled, single-level 2D textures in a few common uncompressed formats,
  // matching the "skip with a rate-limited log" pattern used elsewhere in
  // this file for anything wider than what's been verified against the
  // intro's actual draws. Only the first texture_bindings() entry per stage
  // is honored (see texture_descriptor_layout_'s comment for why one
  // binding suffices for now).
  struct UploadedTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  };
  UploadedTexture* GetOrUploadTexture(uint32_t fetch_constant_index);

  // One-shot helper: uploads width*height RGBA8 texels (already host-order,
  // no further byteswap) into image via a staging buffer on a temporary
  // command buffer, and leaves image in SHADER_READ_ONLY_OPTIMAL. Used both
  // for the default 1x1 texture and every real GetOrUploadTexture upload.
  // Synchronous (waits for completion before returning) -- texture uploads
  // are rare relative to per-frame draw traffic, so simplicity here matters
  // more than overlapping this with other GPU work.
  bool UploadTexelsAndTransition(VkImage image, uint32_t width, uint32_t height,
                                 const void* rgba_data);

  // Begins accumulating a frame's draws into color_target_image_ if this is
  // the first draw since the last present (render pass begin + clear).
  void EnsureFrameBegun();

  void UpdateSharedMemory(uint32_t guest_address_dwords, uint32_t size_dwords);

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

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence submit_fence_ = VK_NULL_HANDLE;

  // Milestone 3b step 3 pipeline resources.
  bool pipeline_resources_valid_ = false;
  VkDescriptorSetLayout shared_memory_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout constants_layout_ = VK_NULL_HANDLE;
  // Sets 2/3 (vertex/pixel textures): SpirvShaderTranslator emits separate
  // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE / VK_DESCRIPTOR_TYPE_SAMPLER bindings
  // (not a combined-image-sampler), with binding index a compacted,
  // dedup'd position in the shader's own texture_bindings()/sampler_bindings()
  // list rather than the guest fetch-constant slot directly. This renderer
  // only supports one texture per stage for now (see GetOrUploadTexture), so
  // one fixed 2-binding layout (0=image, 1=sampler) covers every shader that
  // needs it; a shader that doesn't sample at all simply never statically
  // references these bindings, so reusing the same non-empty layout/set for
  // "no texture" draws too (backed by a 1x1 default texture) is harmless.
  VkDescriptorSetLayout texture_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_set_ = VK_NULL_HANDLE;

  // 1x1 opaque white texture bound wherever a draw doesn't have a real
  // uploaded texture yet (unsupported format, unbound fetch constant, or no
  // texture_bindings() at all) -- keeps every draw's descriptor sets valid
  // without needing a second pipeline layout variant.
  VkImage default_texture_image_ = VK_NULL_HANDLE;
  VkDeviceMemory default_texture_memory_ = VK_NULL_HANDLE;
  VkImageView default_texture_view_ = VK_NULL_HANDLE;
  VkDescriptorSet default_texture_set_ = VK_NULL_HANDLE;
  VkSampler default_sampler_ = VK_NULL_HANDLE;

  // Real uploaded textures, cached by a hash of the fetch constant's raw
  // dwords (address+format+dimensions+tiling all fold into that hash, so a
  // guest rewriting a fetch constant to point elsewhere naturally misses the
  // cache; a guest overwriting texture *content* at the same address without
  // changing the fetch constant will incorrectly keep serving the stale
  // upload -- a known limitation, not yet hit by the intro's content).
  // Capped the same way shader_cache_ is, for the same reason (bound a
  // pathological/garbage-decoded fetch constant that would otherwise hash
  // differently every resubmit).
  static constexpr size_t kMaxTextureCacheEntries = 64;
  bool texture_cache_limit_logged_ = false;
  std::unordered_map<uint64_t, UploadedTexture> texture_cache_;
  void DestroyTextureCache();
  // Constants descriptor sets differ per draw (each draw gets fresh constant
  // buffers -- see TransientBuffer) and Vulkan disallows updating a
  // descriptor set already bound within a not-yet-executed command buffer,
  // so each draw needs its own set too; reset (not freed individually) once
  // per frame alongside frame_transient_buffers_.
  VkDescriptorPool transient_descriptor_pool_ = VK_NULL_HANDLE;

  // Guest physical memory mirrored 1:1 by byte offset, so translated
  // shaders' own address computation (guest fetch-constant base + stride)
  // lands on the right bytes without any host-side remapping. Only the
  // byte ranges draws actually reference get memcpy'd in, not the whole
  // buffer (see UpdateSharedMemory).
  static constexpr VkDeviceSize kSharedMemorySize = 0x20000000;  // 512 MB
  VkBuffer shared_memory_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory shared_memory_memory_ = VK_NULL_HANDLE;
  uint32_t shared_memory_memory_type_ = 0;
  uint8_t* shared_memory_mapped_ = nullptr;

  static constexpr uint32_t kColorTargetWidth = 1280;
  static constexpr uint32_t kColorTargetHeight = 720;
  VkImage color_target_image_ = VK_NULL_HANDLE;
  VkDeviceMemory color_target_memory_ = VK_NULL_HANDLE;
  VkImageView color_target_view_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
  bool frame_active_ = false;
  bool frame_has_draws_ = false;

  // Neither vkCmdBlitImage nor vkCmdCopyImage (image-to-image) is in this
  // SDK's exposed Vulkan function table -- only buffer<->image copies are --
  // so PresentFrame routes color_target_image_ -> guest output image through
  // this intermediate staging buffer (vkCmdCopyImageToBuffer, then
  // vkCmdCopyBufferToImage).
  VkDeviceSize color_target_staging_size_ = 0;
  VkBuffer color_target_staging_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory color_target_staging_memory_ = VK_NULL_HANDLE;

  // Temporary debug aid: dumps the first few presented frames' color target
  // to a raw RGBA file under logs/, so pixel content can be inspected
  // without a display (e.g. from a headless CI-like run). Not gated behind
  // a cvar -- intended to be removed once on-screen output is confirmed
  // working, not a permanent feature.
  uint32_t debug_frames_dumped_ = 0;
  void DebugDumpColorTarget();

  // A garbage-decoded draw's shader ucode (see native-renderer-headless-boot.md,
  // Phase 3 "Next" item 1) hashes differently on every resubmit, so it never
  // hits shader_cache_ and instead pays SpirvShaderTranslator's ~20s cost
  // every single time it recurs -- stalling the frame loop for minutes. Real
  // intro content only ever needs a couple of distinct shaders, so once the
  // cache grows well past that, treat further *new* shaders as suspect and
  // skip translating them (draw is dropped, same as any other unsupported
  // case) rather than paying an unbounded number of ~20s translations.
  static constexpr size_t kMaxShaderCacheEntries = 64;
  bool shader_cache_limit_logged_ = false;

  std::unordered_map<uint64_t, TranslatedShader> shader_cache_;
  std::unordered_map<uint64_t, VkPipeline> pipeline_cache_;

  // Transient per-draw uniform buffers (constants differ per draw within a
  // frame, so each draw gets its own buffer objects rather than racing to
  // reuse one -- see native_command_processor.cpp's TryDraw for why).
  // Freed once PresentFrame's synchronous fence wait confirms the frame's
  // GPU work -- and thus the last read of these buffers -- has completed.
  struct TransientBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
  };
  std::vector<TransientBuffer> frame_transient_buffers_;
  void FreeTransientBuffers();

  // HeadlessRingWaitBypass (docs/native-renderer-headless-boot.md) removes
  // all real GPU backpressure from the guest's frame loop, so without this it
  // free-runs at thousands of "frames" per second -- pace it to something
  // resembling real hardware so per-frame debug logging and CPU usage stay
  // sane. Milestone 3b may replace this with real vsync-driven pacing.
  std::chrono::steady_clock::time_point last_present_time_{};
};

}  // namespace nocturne
