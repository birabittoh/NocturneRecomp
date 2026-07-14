// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <filesystem>

#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include <rex/graphics/register_file.h>
#include <rex/ui/overlay/shader_debugger_overlay.h>
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

  // Fired once per guest frame, right after PresentFrame's swap. This class
  // has no GraphicsSystem, so nothing ever calls the normal
  // GraphicsSystem::SetHostSwapCallback -> Runtime -> ModRegistry::DispatchTick
  // chain the F3 overlay's guest-FPS counter (and any mod's RegisterTick)
  // depends on -- see nocturnerecomp_app.h's OnPostSetup, which wires this to
  // that DispatchTick call directly.
  void SetOnFramePresented(std::function<void()> callback) {
    on_frame_presented_ = std::move(callback);
  }

  // F2 shader debugger overlay support (ReXApp::SetShaderDebuggerOverride --
  // this class isn't a rex::graphics::CommandProcessor subclass and has no
  // GraphicsSystem, so it can't use the default GraphicsSystem-based data
  // source the overlay expects). Hash reported/looked-up here is XXH3 over
  // the raw ucode bytes (matching the xenos/Vulkan backend's own
  // Shader::ucode_data_hash() convention), not this file's internal FNV-1a
  // cache key (HashUcode) -- so a hash from the old xenos-plugin renderer's
  // F2 overlay is directly comparable to one reported here for the same
  // ucode. Translation-level detail (per-modification SPIR-V disassembly,
  // binary replace, profiling counters) isn't tracked in a queryable way by
  // this renderer, so ShaderDebuggerDetails::translations is always empty
  // and profiling/binary-replace requests silently no-op (see
  // nocturnerecomp_app.h's SetShaderDebuggerOverride call).
  std::vector<rex::ui::ShaderDebuggerEntry> GetShaderSnapshot() const;
  rex::ui::ShaderDebuggerDetails GetShaderDetails(uint64_t xxh3_ucode_hash) const;
  void SetShaderDisabledByHash(uint64_t xxh3_ucode_hash, bool disabled);
  // Per-shader CPU-time profiling (Total ms/Draws/Avg us columns in the F2
  // overlay). Off by default -- TryDraw only pays the steady_clock overhead
  // while a caller (the overlay's ctor/dtor) has this enabled, matching the
  // real backend's "zero overhead while the debugger is closed" behavior.
  void SetShaderProfilingEnabled(bool enabled) { shader_profiling_enabled_ = enabled; }
  void ResetShaderProfiling() { shader_profile_.clear(); }

  // Wires this renderer's texture uploads (GetOrUploadTexture) into the
  // SDK's own mod texture-replacement pipeline (rex::graphics::
  // TextureReplacement) -- the same content-hash-keyed <mod_root>/<hash16>
  // .dds|.png convention the D3D12/Vulkan xenos backends already use, so
  // existing texture mods work unmodified against this renderer too. Call
  // once after construction, mirroring ReXApp's own
  // IGraphicsSystem::InitializeAssetReplacement call for the normal xenos
  // path (see rex_app.cpp) -- this renderer has no IGraphicsSystem, so
  // nothing does that wiring automatically.
  void InitializeTextureReplacement(std::vector<std::filesystem::path> mod_roots,
                                    std::filesystem::path dump_root);

  // Same idea as InitializeTextureReplacement, but for shaders: this
  // renderer's own SPIR-V-only replacement path (see
  // docs/native-renderer-shader-replacement.md). There's no SDK-side class to
  // reuse here (unlike rex::graphics::TextureReplacement) since the SDK's
  // only shader-replacement implementation, PipelineCache::
  // ApplyDxbcReplacement, is D3D12/DXBC-only -- so mod_roots/dump_root are
  // just kept on this object and consulted directly from
  // GetOrTranslateShader. Gated purely on mod_roots being non-empty, same as
  // texture_replacement_ -- the SDK's shader_load_enabled cvar isn't usable
  // here (it's only defined in the SDK's own TextureCache TU, which this
  // renderer never links in).
  void InitializeShaderReplacement(std::vector<std::filesystem::path> mod_roots,
                                   std::filesystem::path dump_root);

 private:
  void PresentFrame();
  std::function<void()> on_frame_presented_;

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

  // F2 shader debugger support: hashes (XXH3 over raw ucode, see
  // GetShaderSnapshot) the user has toggled off. Checked in TryDraw, which
  // skips the draw entirely if either bound shader's hash is disabled --
  // mirrors the real backend's shader blacklist (CommandProcessor::
  // AddShaderBlacklist) closely enough for the overlay's toggle to visibly
  // do something, without needing this class to plug into that mechanism.
  std::unordered_set<uint64_t> disabled_shader_hashes_;
  // The two shaders bound by the most recent TryDraw call that actually
  // reached pipeline creation -- GetShaderSnapshot's only way to populate
  // ShaderDebuggerEntry::active without this class tracking a full
  // per-shader "is this bound right now" flag across the whole cache.
  uint64_t last_active_vertex_ucode_xxh3_ = 0;
  uint64_t last_active_pixel_ucode_xxh3_ = 0;

  // F2 profiling (see SetShaderProfilingEnabled). Time is attributed to both
  // the vertex and pixel shader hashes a draw bound, matching the real
  // backend's "summed CPU time inside IssueDraw across all draws this shader
  // participated in" semantics (see CommandProcessor::ShaderInfo's doc
  // comment) -- scoped around TryDraw's actual GPU submission work (pipeline
  // bind, descriptor sets, vkCmdDraw*), not shader translation/analysis,
  // which is cached and amortized across many draws already.
  bool shader_profiling_enabled_ = false;
  struct ShaderProfile {
    uint64_t total_ns = 0;
    uint64_t draw_count = 0;
  };
  std::unordered_map<uint64_t, ShaderProfile> shader_profile_;

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
  // tessellation_mode is xenos::TessellationMode as uint32_t and only
  // meaningful when host_vertex_shader_type is a domain type (it selects the
  // TES spacing execution mode -- equal for discrete, fractional-even for
  // continuous); pass 0 otherwise.
  TranslatedShader* GetOrTranslateShader(
      rex::graphics::xenos::ShaderType type, const std::vector<uint32_t>& ucode,
      uint32_t interpolator_mask,
      rex::graphics::Shader::HostVertexShaderType host_vertex_shader_type =
          rex::graphics::Shader::HostVertexShaderType::kVertex,
      uint32_t tessellation_mode = 0);

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
  // tessellated_quad selects the 4-stage tessellation pipeline variant (see
  // CreateTessellationHostShaders): vertex_shader must then be a translation
  // with a quad-domain HostVertexShaderType (its module is a tessellation
  // *evaluation* shader), topology must be VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
  // and the fixed passthrough VS/TCS supply the other two stages.
  PipelineEntry GetOrCreatePipeline(TranslatedShader* vertex_shader, TranslatedShader* pixel_shader,
                                    VkPrimitiveTopology topology, bool primitive_restart_enable,
                                    uint32_t blend_control, uint32_t color_write_mask,
                                    bool tessellated_quad = false);

  // Hardware tessellation support (the title-screen smoke overlay is drawn as
  // a tessellated 4-control-point quad patch -- VGT_OUTPUT_PATH_CNTL selects
  // kTessellationEnable and the "vertex shader" is really a quad-domain
  // *domain* shader reading r0.xy = tess coords, r0.z/r1.xyz = control point
  // indices; running it as a plain vertex shader collapses every vertex onto
  // control point 0 and rasterizes nothing). Built once at init when the
  // device supports tessellationShader: a passthrough vertex shader exporting
  // gl_VertexIndex as the float control-point index the SDK's TES translation
  // expects, and a quad TCS gathering the 4 indices into the per-patch
  // xe_in_patch_control_point_indices input and setting all tess levels from
  // a push constant (VGT_HOS_MAX_TESS_LEVEL + 1, pushed per draw in TryDraw).
  // Hand-assembled with SpirvBuilder, same rationale as the post-process
  // vertex shader above (no runtime GLSL compiler in the SDK).
  bool CreateTessellationHostShaders();
  VkShaderModule tess_control_point_vs_ = VK_NULL_HANDLE;
  VkShaderModule tess_quad_tcs_ = VK_NULL_HANDLE;

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

  // Reuses the SDK's own post_process_shader_path/post_process_shader_enabled
  // cvars, but runs the configured shader only over the gameplay-preview texture
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

  // Real guest-driven sampler state (partial): gets (creating on first use) a
  // sampler whose address modes match the texture fetch constant's
  // clamp_x/y/z. Found necessary for the title-screen smoke overlay, whose U
  // coordinate scrolls past 1.0 every frame and expects kRepeat wrapping --
  // the fixed clamp-to-edge default_sampler_ smeared the texture's last
  // column across the seam instead of looping it. Filter state is still the
  // fixed linear (or nearest for the gameplay preview, same rule as before)
  // rather than fetch-constant-driven -- clamp modes were the only observed
  // wrong behavior. Border-color modes fall back to their edge-clamping
  // cousins, matching the SDK Vulkan backend's own approximations.
  VkSampler GetOrCreateSampler(rex::graphics::xenos::ClampMode clamp_x,
                               rex::graphics::xenos::ClampMode clamp_y,
                               rex::graphics::xenos::ClampMode clamp_z, bool nearest);
  std::unordered_map<uint32_t, VkSampler> sampler_cache_;

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

  // See InitializeTextureReplacement. Null until that's called (or if it's
  // never called, e.g. a standalone debug build) -- GetOrUploadTexture treats
  // that the same as "no replacement found" and falls back to the normal
  // guest-memory decode path.
  std::unique_ptr<rex::graphics::TextureReplacement> texture_replacement_;

  // See InitializeShaderReplacement. Empty (both) until that's called --
  // GetOrTranslateShader then just never finds a replacement and always
  // translates, same "absent means normal path" convention as
  // texture_replacement_ being null. mod_roots is priority-ordered (first
  // root wins per hash), matching the texture pipeline's convention.
  std::vector<std::filesystem::path> shader_mod_roots_;
  std::filesystem::path shader_dump_root_;
  // Looks up a <hash16>.spv replacement for ucode_hash in shader_mod_roots_
  // (first root wins) and, if found and readable, overwrites translation's
  // translated_binary() with its raw bytes in place -- mirroring
  // PipelineCache::ApplyDxbcReplacement's "keep the real translation's
  // metadata (texture/sampler binding layout), only swap the final binary"
  // approach, since this renderer's descriptor set/pipeline layouts are built
  // from that metadata (see GetOrCreatePipelineLayout) and a replacement
  // module has to match it exactly (see docs/native-renderer-shader-
  // replacement.md, "Pipeline layout compatibility"). Returns true if a
  // replacement was applied.
  bool ApplyShaderReplacement(uint64_t ucode_hash, rex::graphics::Shader::Translation& translation);
  // Dumps translation's current translated_binary() (the real, untouched
  // translation -- called before ApplyShaderReplacement, matching the
  // D3D12 backend's dump-then-replace ordering) to
  // <shader_dump_root_>/shaders/<hash16>.<vert|frag>.native.spv, once per
  // hash (skips if the file already exists, same rationale as
  // TextureReplacement::DumpTexture's "write once" rule).
  void DumpShaderTranslation(uint64_t ucode_hash, rex::graphics::xenos::ShaderType type,
                             const std::vector<uint8_t>& spirv_bytes) const;
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

  // Constants arena: one large, persistently-mapped uniform buffer that
  // upload_constant_buffer suballocates from by offset, instead of doing a
  // fresh vkCreateBuffer + dedicated vkAllocateMemory per constant buffer per
  // draw (up to 5/draw x hundreds of draws/frame ~= thousands of
  // vkAllocateMemory calls/frame). That per-draw allocation cost is the
  // dominant reason inline PM4 processing balloons into multi-second stalls
  // that freeze the guest thread (the "skip ahead" -- see
  // docs/native-renderer-pacing-investigation.md). The offset is reset once
  // per frame in EnsureFrameBegun, right after the fence wait guarantees the
  // GPU has finished reading last frame's region, and the used range is
  // flushed once per frame in PresentFrame before submit.
  static constexpr VkDeviceSize kConstantsArenaSize = 64ull * 1024 * 1024;
  VkBuffer constants_arena_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory constants_arena_memory_ = VK_NULL_HANDLE;
  uint8_t* constants_arena_mapped_ = nullptr;
  VkDeviceSize constants_arena_offset_ = 0;
  VkDeviceSize constants_arena_alignment_ = 256;

  // GPU swap counter, mirroring xenos CommandProcessor::counter_: increments
  // once per swap packet; EVENT_WRITE_SHD fences whose initiator selects the
  // counter (bit 31) write this value to guest memory as the GPU-progress
  // signal the game's frame-pacing logic reads (see OnPacket).
  uint32_t swap_counter_ = 0;

  // HeadlessRingWaitBypass (docs/native-renderer-headless-boot.md) removes
  // all real GPU backpressure from the guest's frame loop, so without this it
  // free-runs at thousands of "frames" per second -- pace it to something
  // resembling real hardware so per-frame debug logging and CPU usage stay
  // sane. Milestone 3b may replace this with real vsync-driven pacing.
  std::chrono::steady_clock::time_point last_present_time_{};

  // Fast-forward frame-skip (see docs/native-renderer-pacing-investigation.md
  // and mods_src/fast_forward): last_present_time_ above paces how often this
  // function is even *called* (scaled by the guest clock's time scalar), but
  // the guest's mode loop calls PresentFrame once per logic iteration, so
  // real GPU present work was coupled 1:1 with logic throughput and capped it
  // at the real Vulkan submit/present/fence-wait round-trip cost (~95fps
  // measured). These two track a separate, *unscaled* real-time gate that
  // caps actual presentation at a safe ~60Hz regardless of fast-forward
  // intensity, while logic keeps accumulating draws into a still-open frame
  // in between (see PresentFrame's do_present branch).
  std::chrono::steady_clock::time_point last_physical_present_time_{};
  uint32_t frames_since_present_ = 0;
};

}  // namespace nocturne
