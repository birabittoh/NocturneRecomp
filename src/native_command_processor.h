// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv.h>
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

  // Gets (running AnalyzeUcode on first use) the Shader object for the given
  // raw ucode, or nullptr if empty. Cached separately from translation/
  // VkShaderModule (see GetOrTranslateShader) because the interpolator_mask
  // a shader needs to be *translated* with depends on its paired vertex/
  // pixel shader (see TryDraw), but the ucode analysis itself doesn't --
  // this lets TryDraw inspect writes_interpolators()/GetInterpolatorInputMask()
  // on both shaders of a pair before deciding what modification to compile
  // either one with, without re-running AnalyzeUcode every draw.
  rex::graphics::Shader* GetOrAnalyzeShader(rex::graphics::xenos::ShaderType type,
                                            const std::vector<uint32_t>& ucode);
  std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::Shader>> analyzed_shaders_;

  // Gets (translating + creating a VkShaderModule on first use) the shader
  // for the given raw ucode, or nullptr if translation failed. Cached by a
  // hash of the ucode bytes plus every modification input (host vertex
  // shader type, interpolator mask) so repeated draws with the same shader
  // *and* modification (the overwhelmingly common case -- shaders are loaded
  // once, then reused across many draws) don't re-translate, while a shader
  // reused with a genuinely different modification gets its own translation
  // instead of aliasing onto the wrong one. interpolator_mask is the
  // guest-real intersection of what the vertex shader writes and what the
  // pixel shader reads (see TryDraw) -- required, not defaulted, because
  // leaving it 0 (this renderer's original bug) silently drops all
  // vertex-to-pixel interpolated data, including vertex color, producing
  // solid-black output for any shader relying on it. See
  // docs/native-renderer-headless-boot.md.
  struct TranslatedShader {
    rex::graphics::Shader* shader = nullptr;  // Owned by analyzed_shaders_.
    VkShaderModule module = VK_NULL_HANDLE;
  };
  TranslatedShader* GetOrTranslateShader(
      rex::graphics::xenos::ShaderType type, const std::vector<uint32_t>& ucode,
      uint32_t interpolator_mask,
      rex::graphics::Shader::HostVertexShaderType host_vertex_shader_type =
          rex::graphics::Shader::HostVertexShaderType::kVertex);

  // Gets (building on first use) a VkPipeline for this exact vertex/pixel
  // shader pair + primitive topology + blend state, plus the VkPipelineLayout
  // it was created with (see GetOrCreateTextureSetLayout/
  // GetOrCreatePipelineLayout -- the pipeline layout depends on each shader's
  // actual texture/sampler counts, so it isn't a single fixed global anymore,
  // and the caller needs the exact layout the pipeline was built against to
  // bind descriptor sets correctly). blend_control is the raw RB_BLENDCONTROL0
  // value and color_write_mask the RT0 write-mask bits from RB_COLOR_MASK
  // (see the doc comment on GetOrCreatePipeline's blend_attachment setup in
  // native_command_processor.cpp) -- both fold into the cache key since the
  // guest can reuse the same shader pair with different blend state across
  // draws (e.g. a fade animating its blend factors frame to frame with static
  // geometry/shaders).
  struct PipelineEntry {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
  };
  PipelineEntry GetOrCreatePipeline(TranslatedShader* vertex_shader, TranslatedShader* pixel_shader,
                                    VkPrimitiveTopology topology, bool primitive_restart_enable,
                                    uint32_t blend_control, uint32_t color_write_mask);

  // Multi-texture-per-stage support: SpirvShaderTranslator assigns each
  // shader's SAMPLED_IMAGE bindings to [0, texture_bindings().size()) and its
  // SAMPLER bindings to [texture_bindings().size(), + sampler_bindings().size()),
  // both compacted/dedup'd per that specific shader -- there's no fixed
  // binding count that works for every shader, so the descriptor set layout
  // (and therefore the pipeline layout that embeds it) has to be built per
  // distinct (texture_count, sampler_count) shape actually seen, not once
  // globally. Found necessary when a real draw's pixel shader needed two
  // simultaneous texture units (a base image + a glow/overlay layer) and the
  // old single fixed 1-image+1-sampler layout left the second one completely
  // unbound. See docs/native-renderer-headless-boot.md.
  static constexpr uint32_t kMaxTexturesPerStage = 8;
  VkDescriptorSetLayout GetOrCreateTextureSetLayout(uint32_t texture_count, uint32_t sampler_count);
  std::unordered_map<uint64_t, VkDescriptorSetLayout> texture_set_layout_cache_;
  VkPipelineLayout GetOrCreatePipelineLayout(uint32_t vertex_texture_count,
                                             uint32_t vertex_sampler_count,
                                             uint32_t pixel_texture_count,
                                             uint32_t pixel_sampler_count);
  std::unordered_map<uint64_t, VkPipelineLayout> pipeline_layout_cache_;

  // Milestone 3b step 7 (+ DXT4/5+tiling, k_16_16_16_16 follow-ups): gets
  // (uploading on first use) a sampled VkImage for the given texture fetch
  // constant slot, or nullptr if the fetch constant describes something not
  // yet supported (mipped/array/3D, or a format outside the small allow-list
  // -- see native_command_processor.cpp's GetOrUploadTexture) or is simply
  // unbound (base_address == 0). Scoped deliberately narrow: only
  // single-level 2D textures in k_8_8_8_8/k_1_5_5_5/k_4_4_4_4/k_DXT4_5/
  // k_16_16_16_16 (both tiled and linear layouts, via
  // texture_util::GetTiledOffset2D), matching the "skip with a rate-limited
  // log" pattern used elsewhere in this file for anything wider than what's
  // been verified against real draws. No longer owns a descriptor set itself
  // (see the multi-texture-per-stage comment above) -- callers combine
  // however many of these a specific shader needs into a fresh per-draw set.
  struct UploadedTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // Guest physical byte range this upload was decoded from -- used to
    // invalidate the cache entry when a COHER_STATUS_HOST invalidation (see
    // InvalidateTextureCacheRange) or an EDRAM resolve (TryResolveCopy)
    // writes into an overlapping range, since a guest that repopulates a
    // texture's content in place (the gameplay-preview texture case) never
    // touches the fetch constant itself and so would otherwise never miss
    // texture_cache_ again.
    uint32_t base_address_bytes = 0;
    uint32_t size_bytes = 0;
    // Set when this upload matched GetOrUploadTexture's gameplay-preview
    // heuristic (512x256) -- TryDraw uses this to pick
    // gameplay_preview_sampler_ (nearest) instead of default_sampler_
    // (bilinear) for just this texture's binding, so the nearest-vs-bilinear
    // choice stays scoped to this one texture rather than applying to every
    // sampler slot in a draw.
    bool is_gameplay_preview = false;
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

  // Toggleable (scanlines cvar) per-texture
  // equivalent of the SDK's own post_process_shader_path/enabled: runs the
  // same configured shader, but only over the gameplay-preview texture
  // (identified by its known 852x480 size -- see GetOrUploadTexture) rather
  // than the whole composited frame. Renders source_view through the
  // compiled post_process_shader_path fragment shader into a fresh
  // width x height image, returned via the out params (left untouched if
  // this returns false -- caller should fall back to sampling source_image
  // directly, same as when the feature is disabled or the shader fails to
  // compile). See EnsurePostProcessPipeline for the one-time pipeline setup
  // this lazily triggers.
  bool ApplyGameplayPreviewPostProcess(VkImageView source_view, uint32_t width, uint32_t height,
                                       VkImage& out_image, VkDeviceMemory& out_memory,
                                       VkImageView& out_view);
  // Builds the fixed vertex shader (a hand-assembled SPIR-V full-screen
  // triangle -- see the .cpp: this SDK doesn't expose a general-purpose
  // runtime GLSL compiler, only rex::ui::CustomShader::Compile's
  // fragment-only helper tied to the presenter's own preamble, so there's no
  // supported way to compile a matching vertex stage from GLSL here) plus
  // the render pass/descriptor set layout/pipeline layout/sampler this
  // feature needs, once. (Re)compiles the fragment shader from
  // post_process_shader_path whenever that cvar's value changes. Returns
  // false (leaving the pipeline unusable, logged once) if compilation or any
  // resource creation fails.
  bool EnsurePostProcessPipeline();
  void DestroyPostProcessPipeline();
  VkRenderPass post_process_render_pass_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout post_process_descriptor_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout post_process_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline post_process_pipeline_ = VK_NULL_HANDLE;
  VkShaderModule post_process_vertex_shader_ = VK_NULL_HANDLE;
  VkShaderModule post_process_fragment_shader_ = VK_NULL_HANDLE;
  VkSampler post_process_sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool post_process_descriptor_pool_ = VK_NULL_HANDLE;
  // Cache key mirroring VulkanPresenter::custom_shader_compiled_path_ -- only
  // recompile post_process_fragment_shader_ when post_process_shader_path
  // actually changed.
  std::string post_process_shader_compiled_path_;
  bool post_process_shader_compile_failed_ = false;
  bool post_process_pipeline_valid_ = false;

  // Begins accumulating a frame's draws into color_target_image_ if this is
  // the first draw since the last present (render pass begin + clear).
  void EnsureFrameBegun();

  void UpdateSharedMemory(uint32_t guest_address_dwords, uint32_t size_dwords);

  // EDRAM resolve-to-texture (see docs/native-renderer-headless-boot.md, step
  // 17): when the guest sets RB_MODECONTROL.edram_mode to kCopy before what
  // would otherwise be a normal draw, that "draw" is actually a resolve
  // trigger -- real hardware copies the current render target (or a
  // rectangle of it, given by the resolve's own 3 vf0 vertices) into a
  // guest-memory texture instead of rasterizing. Confirmed real and live
  // (fires every frame for a full-screen 1280x720 target), though it turned
  // out to be unrelated to the gameplay-preview texture -- see step 18,
  // whose COHER_STATUS_HOST-based InvalidateTextureCacheRange is what
  // actually fixed that. Narrow support: only k_8_8_8_8 dest format,
  // non-array, no depth-copy, no color/depth-clear-only resolves
  // (all skip-and-log like the rest of this file's format allow-lists).
  void TryResolveCopy();

  // Queues [base, base+size) to have any overlapping texture_cache_ entry
  // destroyed+erased once it's actually safe to do so (see
  // pending_texture_cache_invalidations_'s doc comment). Shared by the
  // COHER_STATUS_HOST handler in OnPacket (see docs/native-renderer-
  // headless-boot.md step 18 -- the real, general, content-driven
  // invalidation signal: the guest's own cache-coherency event marking a
  // range as written by something outside this renderer's normal draw
  // path, e.g. CPU/software rendering) and TryResolveCopy (defense in
  // depth for its own writes, which don't otherwise trigger a coherency
  // event this renderer observes).
  void InvalidateTextureCacheRange(uint32_t base, uint32_t size);
  // OnPacket runs live as PM4 packets stream in, potentially mid-recording
  // of the current frame's command buffer -- a texture invalidated there
  // might already be bound in an earlier draw recorded (but not yet
  // submitted/executed) in that same buffer, so destroying its VkImage
  // immediately would be a use-after-free once the GPU actually runs that
  // draw. Collected here instead and only actually applied in
  // EnsureFrameBegun, right after its fence wait -- the same point
  // FreeTransientBuffers() already relies on for "the GPU is definitely
  // done with anything from the previous frame."
  std::vector<std::pair<uint32_t, uint32_t>> pending_texture_cache_invalidations_;

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
  // Sets 2/3 (vertex/pixel textures) no longer have a single fixed layout --
  // see the multi-texture-per-stage comment on GetOrCreateTextureSetLayout/
  // GetOrCreatePipelineLayout above; both are built per shader-pair shape and
  // cached there instead.
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_set_ = VK_NULL_HANDLE;

  // 1x1 opaque white texture -- used as the per-slot fallback image view
  // wherever a specific texture_bindings() entry doesn't have a real
  // uploaded texture yet (unsupported format, unbound fetch constant), one
  // slot at a time within a (possibly multi-texture) per-draw descriptor set
  // built in TryDraw. A shader with zero texture_bindings() at all just gets
  // an empty (0-binding) set, no fallback needed.
  VkImage default_texture_image_ = VK_NULL_HANDLE;
  VkDeviceMemory default_texture_memory_ = VK_NULL_HANDLE;
  VkImageView default_texture_view_ = VK_NULL_HANDLE;
  VkSampler default_sampler_ = VK_NULL_HANDLE;
  // Nearest, used only for the gameplay-preview texture -- see
  // UploadedTexture::is_gameplay_preview.
  VkSampler gameplay_preview_sampler_ = VK_NULL_HANDLE;

  // Real uploaded textures, cached by a hash of the fetch constant's raw
  // dwords (address+format+dimensions+tiling all fold into that hash, so a
  // guest rewriting a fetch constant to point elsewhere naturally misses the
  // cache). A guest overwriting texture *content* at the same address without
  // changing the fetch constant (the gameplay-preview texture case) does NOT
  // naturally miss this cache on its own -- InvalidateTextureCacheRange
  // (queued from OnPacket's COHER_STATUS_HOST handling and from
  // TryResolveCopy, applied in EnsureFrameBegun) explicitly invalidates any
  // entry whose base_address_bytes/size_bytes range overlaps a just-written
  // range, so that case still gets a fresh upload next time it's sampled.
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

  // Matches the guest's actual configured video mode (video_mode_width/
  // height, or window_width/height, or a --resolution preset -- see
  // xboxkrnl_video.cpp's GetConfiguredVideoModeWidth/Height, replicated in
  // native_command_processor.cpp since those helpers are file-local there).
  // Resolved once at construction: the guest queries its video mode once at
  // boot (VdQueryVideoMode) and sets its viewport registers to match, so a
  // renderer whose actual target size didn't match would scale every 2D
  // draw by (guest's assumed size / this size) and crop to this size's
  // top-left corner -- exactly what happened when this was a hardcoded
  // 1280x720 constant and the game was launched with --resolution 1080p.
  const uint32_t color_target_width_;
  const uint32_t color_target_height_;
  VkImage color_target_image_ = VK_NULL_HANDLE;
  VkDeviceMemory color_target_memory_ = VK_NULL_HANDLE;
  VkImageView color_target_view_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  // Twin of render_pass_ with loadOp=LOAD instead of CLEAR and
  // initialLayout=TRANSFER_SRC_OPTIMAL (matching color_target_image_'s
  // actual layout right after a mid-frame TryResolveCopy readback) --
  // shares framebuffer_ (load op isn't part of a framebuffer). Used only to
  // resume recording draws into color_target_image_ after TryResolveCopy
  // has to end the render pass/command buffer early to read it back; a
  // normal frame only ever uses render_pass_.
  VkRenderPass render_pass_continue_ = VK_NULL_HANDLE;
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
  std::unordered_map<uint64_t, PipelineEntry> pipeline_cache_;

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
