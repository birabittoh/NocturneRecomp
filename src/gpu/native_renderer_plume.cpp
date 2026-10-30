// nocturnerecomp - ReXGlue Recompiled Project
//
// See native_renderer_plume.h.

#include "native_renderer_draw.h"
#include "core/settings.h"
#include "native_renderer_plume.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/ui_drawer.h>

#include <plume_render_interface.h>

#if defined(__ANDROID__)
#include "android_gpu_driver.h"
#endif

#if defined(__APPLE__)
#include <dlfcn.h>
#include <filesystem>

#include <rex/filesystem.h>
#endif

#include "native_renderer_plume_internal.h"
#include "ui/ui_text.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace plume {
#ifdef _WIN32
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
extern void SetVulkanGetInstanceProcAddr(void* getInstanceProcAddr);
}  // namespace plume

namespace nocturne {
namespace {

using namespace plume;

// B8G8R8A8 rather than R8G8B8A8 because every Windows swap chain supports it
// natively; the other costs a conversion on present.
//
// On Android it is the other way round: Adreno's surfaces do not list
// B8G8R8A8_UNORM at all, and Plume reacts by silently creating the swap chain
// with VK_FORMAT_UNDEFINED, which presents a black screen.
#if defined(__ANDROID__)
constexpr RenderFormat kSwapChainFormat = RenderFormat::R8G8B8A8_UNORM;
#else
constexpr RenderFormat kSwapChainFormat = RenderFormat::B8G8R8A8_UNORM;
#endif

// Three, not two. With two there is exactly one spare buffer, so a frame cannot
// start until the previous one's flip has retired its buffer, which under vsync
// means waiting for a vblank and quantises the frame rate to 60/30/20.
constexpr uint32_t kSwapChainBuffers = 3;

// How many presents may be outstanding before the frame loop blocks. Also the
// value Plume hands to SetMaximumFrameLatency on D3D12, where 0 is not a legal
// argument.
constexpr uint32_t kMaxFrameLatency = 2;

#if defined(__APPLE__)
// Point Vulkan at the loader and MoltenVK shipped in the package, so the game
// runs without a Vulkan SDK or Homebrew installed.
//
// The loader is dlopen'd by absolute path because volk dlopens it by leaf name,
// which only searches the DYLD_ variables, and dyld captures those at launch so
// setenv cannot extend them. Preloading works because a leaf dlopen matches an
// already loaded image. The driver manifest is read at instance creation, so
// VK_DRIVER_FILES is in time. Without it vkCreateInstance returns
// VK_ERROR_INCOMPATIBLE_DRIVER.
//
// Both halves defer to an environment that already says otherwise. Probes the
// flat layout then the .app one, matching the SDK's own vulkan_moltenvk.cpp.
void BootstrapAppleVulkanRuntime() {
  namespace fs = std::filesystem;

  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  if (exe_dir.empty())
    return;
  // Contents/MacOS -> Contents, for the bundle layouts below. Empty when the
  // executable is not inside a bundle, in which case those probes are skipped.
  const fs::path bundle = exe_dir.filename() == "MacOS" ? exe_dir.parent_path() : fs::path();

  auto first_existing = [](const std::vector<fs::path>& candidates) -> fs::path {
    for (const fs::path& candidate : candidates) {
      std::error_code ec;
      if (!candidate.empty() && fs::exists(candidate, ec))
        return candidate;
    }
    return {};
  };

  // vulkan/lib first: that is the layout the SDK's CMake stages, so it is the
  // loader the SDK was built against rather than whatever else is lying around.
  std::vector<fs::path> loaders = {
      exe_dir / "vulkan" / "lib" / "libvulkan.1.dylib", exe_dir / "libvulkan.1.dylib",
      exe_dir / "libvulkan.dylib", exe_dir / "lib" / "libvulkan.1.dylib",
      exe_dir / "vulkan" / "lib" / "libMoltenVK.dylib", exe_dir / "libMoltenVK.dylib"};
  if (!bundle.empty()) {
    loaders.push_back(bundle / "Frameworks" / "libvulkan.1.dylib");
    loaders.push_back(bundle / "Frameworks" / "libvulkan.dylib");
    loaders.push_back(bundle / "Frameworks" / "libMoltenVK.dylib");
  }

  const fs::path loader = first_existing(loaders);
  if (!loader.empty()) {
    // RTLD_GLOBAL, not RTLD_LOCAL: volk's later dlopen has to be able to match
    // this image. Never dlclose'd; it is needed for the life of the process.
    if (dlopen(loader.string().c_str(), RTLD_NOW | RTLD_GLOBAL) != nullptr) {
      REXLOG_DEBUG("native_renderer: preloaded the bundled Vulkan loader from {}", loader.string());
    } else {
      REXLOG_WARN("native_renderer: could not load the bundled Vulkan loader at {}: {}",
                  loader.string(), dlerror());
    }
  } else {
    REXLOG_DEBUG(
        "native_renderer: no Vulkan loader shipped with this build, falling back to whatever is "
        "installed on the system");
  }

  const char* existing = std::getenv("VK_DRIVER_FILES");
  const char* existing_legacy = std::getenv("VK_ICD_FILENAMES");
  if ((existing && existing[0]) || (existing_legacy && existing_legacy[0]))
    return;

  std::vector<fs::path> icds = {
      exe_dir / "vulkan" / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json",
      exe_dir / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json",
      exe_dir / "vulkan" / "icd.d" / "MoltenVK_icd.json"};
  if (!bundle.empty()) {
    icds.push_back(bundle / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json");
  }

  const fs::path icd = first_existing(icds);
  if (icd.empty()) {
    REXLOG_DEBUG(
        "native_renderer: no MoltenVK driver manifest shipped with this build; if Vulkan instance "
        "creation fails with ErrorIncompatibleDriver, that is why");
    return;
  }
  // Both names: VK_DRIVER_FILES is the current one, VK_ICD_FILENAMES the
  // deprecated spelling older loaders still read.
  setenv("VK_DRIVER_FILES", icd.string().c_str(), 1);
  setenv("VK_ICD_FILENAMES", icd.string().c_str(), 1);
  REXLOG_DEBUG("native_renderer: using the bundled MoltenVK driver manifest at {}", icd.string());
}
#endif  // __APPLE__

struct PlumeBackend {
  // Not named `interface`: windows.h defines that as a macro, and Plume drags
  // windows.h in behind d3d12.h.
  std::unique_ptr<RenderInterface> render_interface;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  std::unique_ptr<RenderSwapChain> swap_chain;
  std::vector<std::unique_ptr<RenderCommandSemaphore>> release_semaphores;
  std::vector<std::unique_ptr<RenderFramebuffer>> framebuffers;
  std::string api_name;
};

// Everything a frame owns for as long as the GPU is still reading it. One per
// slot, which is the whole of what frames in flight costs.
struct FrameSlot {
  std::unique_ptr<RenderCommandList> command_list;
  std::unique_ptr<RenderCommandFence> fence;
  // Per slot rather than shared: the next frame acquires its image before the
  // previous frame's wait on this has necessarily executed.
  std::unique_ptr<RenderCommandSemaphore> acquire_semaphore;
  // Whether `fence` has work outstanding on it. A slot that has never been
  // submitted must not be waited on.
  bool submitted = false;
};

FrameSlot g_slots[kFramesInFlight];
uint32_t g_slot = 0;

// Whether the current slot's command list has begun. It begins on the first
// guest draw of the frame, or at the present when the frame drew nothing.
bool g_recording = false;
uint64_t g_frame_serial = 0;

PlumeBackend g_backend;
std::atomic<bool> g_ready{false};

// Set from the UI thread when the window resizes, consumed by the next present
// on the guest thread. The swap chain is not thread safe, so the resize has to
// happen where every other swap chain call happens rather than where the event
// arrives.
std::atomic<bool> g_resize_pending{false};

// The window surface going away and coming back (Android backgrounding). Same
// deferral as the resize: recorded on the UI thread, applied by the next
// present. `g_pending_surface` holds the new ANativeWindow to rebuild on.
std::atomic<bool> g_surface_lost{false};
std::atomic<void*> g_pending_surface{nullptr};

// Same shape as the resize: the `vsync` cvar can change from the settings
// overlay at any time, but the swap chain is only safe to touch from the
// thread that presents.
std::atomic<bool> g_vsync_wanted{false};
std::atomic<bool> g_vsync_applied{false};

// Whether the swap chain was created with present wait, i.e. whether the
// per-frame wait() is legal to call.
bool g_present_wait = false;

// The SDK's ImGui drawer, once the app hands it over.
rex::ui::UIDrawer* g_overlay = nullptr;

uint64_t g_frames_presented = 0;

// A window dragged by its corner reports a new size every frame, and each one
// would rebuild the guest's targets. Hold a candidate until it has stopped
// moving, then rebuild once and save the size for the next launch.
uint32_t g_settled_width = 0, g_settled_height = 0;
uint32_t g_pending_width = 0, g_pending_height = 0;
uint64_t g_pending_frame = 0;
constexpr uint64_t kWindowSettleFrames = 30;  // half a second at 60 fps

void WaitForAllSlots();

// Called once per present, after the frame has been submitted. The rebuild
// waits for every slot, so it cannot free an image a queued frame still reads.
void SettleWindowSize() {
  if (g_backend.swap_chain->isEmpty())
    return;
  const uint32_t width = g_backend.swap_chain->getWidth();
  const uint32_t height = g_backend.swap_chain->getHeight();
  if (g_settled_width == 0) {  // first look: whatever the target was built at
    g_settled_width = width;
    g_settled_height = height;
    return;
  }
  if (width == g_settled_width && height == g_settled_height) {
    g_pending_width = g_pending_height = 0;
    // The target is built by the first draw, which can land before the window
    // has reached its final size; catch that up without a debounce.
    uint32_t built_width = 0, built_height = 0;
    GuestTargetExtent(&built_width, &built_height);
    uint32_t want_width = built_width, want_height = built_height;
    PlumeGuestExtent(&want_width, &want_height);
    if (built_width != 0 && (built_width != want_width || built_height != want_height)) {
      WaitForAllSlots();
      GuestResizeTarget(want_width, want_height);
    }
    return;
  }
  if (width != g_pending_width || height != g_pending_height) {
    g_pending_width = width;
    g_pending_height = height;
    g_pending_frame = g_frames_presented;
    return;
  }
  if (g_frames_presented - g_pending_frame < kWindowSettleFrames)
    return;

  g_settled_width = width;
  g_settled_height = height;
  g_pending_width = g_pending_height = 0;
  PersistWindowSize();

  uint32_t extent_width = 0, extent_height = 0;
  GuestTargetExtent(&extent_width, &extent_height);
  if (extent_width == 0)
    return;
  PlumeGuestExtent(&extent_width, &extent_height);
  WaitForAllSlots();
  GuestResizeTarget(extent_width, extent_height);
}
uint64_t g_acquire_failures = 0;
uint64_t g_present_failures = 0;
uint64_t g_resize_failures = 0;

// Waits taken that no present paid back. The frame latency waitable object is a
// semaphore seeded with maxFrameLatency and topped up only by Present, so a
// frame that waits and then bails out spends a token for good; spend
// maxFrameLatency of them and the next wait blocks forever. Skipping that many
// later waits is what puts the count back.
uint32_t g_present_debt = 0;

void OwePresentWait() { g_present_debt = std::min(g_present_debt + 1, kMaxFrameLatency); }

bool ReadVsyncCvar() { return rex::cvar::GetFlagByName("vsync") == "true"; }

void ApplyVsyncIfChanged() {
  const bool wanted = g_vsync_wanted.load(std::memory_order_acquire);
  if (wanted == g_vsync_applied.load(std::memory_order_relaxed))
    return;
  g_backend.swap_chain->setVsyncEnabled(wanted);
  g_vsync_applied.store(wanted, std::memory_order_relaxed);
  REXLOG_DEBUG("native_renderer: vsync {}", wanted ? "on" : "off");
}

// Block until this slot's outstanding work has run. A slot with nothing
// outstanding returns immediately.
void WaitForSlot(FrameSlot& slot) {
  if (!slot.submitted)
    return;
  g_backend.queue->waitForCommandFence(slot.fence.get());
  slot.submitted = false;
}

// Drain the whole ring. For the two things that need the queue genuinely idle:
// a swap chain resize, and shutdown.
void WaitForAllSlots() {
  for (FrameSlot& slot : g_slots)
    WaitForSlot(slot);
}

// One framebuffer per swap chain image. Rebuilt whenever the swap chain is,
// because the textures behind them are replaced by a resize.
void CreateFramebuffers() {
  g_backend.framebuffers.clear();
  const uint32_t count = g_backend.swap_chain->getTextureCount();
  for (uint32_t i = 0; i < count; ++i) {
    const RenderTexture* attachment = g_backend.swap_chain->getTexture(i);
    RenderFramebufferDesc desc;
    desc.colorAttachments = &attachment;
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = nullptr;
    g_backend.framebuffers.push_back(g_backend.device->createFramebuffer(desc));
  }
}

// Grows the per-image release semaphores to match the swap chain. The count is
// not fixed: Vulkan reports however many images the driver actually gave us,
// which can exceed what we asked for and can change across a resize. They are
// indexed by the acquired image index.
void GrowReleaseSemaphores() {
  while (g_backend.release_semaphores.size() < g_backend.swap_chain->getTextureCount())
    g_backend.release_semaphores.push_back(g_backend.device->createCommandSemaphore());
}

#ifdef _WIN32
// DXGI_ERROR_DEVICE_REMOVED / _HUNG / _RESET / _DRIVER_INTERNAL_ERROR, and
// VK_ERROR_DEVICE_LOST.
bool IsDeviceLostCode(uint32_t code) {
  return code == 0x887A0005 || code == 0x887A0006 || code == 0x887A0007 || code == 0x887A0020 ||
         code == 0xFFFFFFFC;
}

bool DeviceLost() {
  if (!g_backend.swap_chain)
    return false;
  if (IsDeviceLostCode(uint32_t(g_backend.swap_chain->getLastError())))
    return true;
  return g_backend.api_name == "D3D12" && uint32_t(g_backend.swap_chain->getDeviceRemovedReason()) != 0;
}

// A lost device never comes back, and nothing here rebuilds one, so the window
// would sit frozen on its last frame with every input dead. Say so, and offer
// the other backend, which is what gets a player past a driver that only
// misbehaves under one API. Asked once; runs on the guest GPU thread, which has
// nothing left to draw anyway.
void PromptDeviceLost() {
  static bool prompted = false;
  if (prompted)
    return;
  prompted = true;

  const bool on_vulkan = g_backend.api_name == "Vulkan";
  const char* other_name = on_vulkan ? "d3d12" : "vulkan";
  const char* other_label = on_vulkan ? "D3D12" : "Vulkan";
  REXLOG_ERROR("native_renderer: the {} device was lost; offering {}", g_backend.api_name,
               other_label);

  const std::string message = TrArgs("renderer", "device_lost",
                                     {g_backend.api_name, other_label});
  const std::u16string wide_message = Utf8ToUtf16(message);
  const std::u16string wide_title = Utf8ToUtf16(Tr("renderer", "device_lost_title"));
  const int answer = MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide_message.c_str()),
                                 reinterpret_cast<const wchar_t*>(wide_title.c_str()),
                                 MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
  if (answer == IDYES) {
    rex::cvar::SetFlagByName("gpu_backend", other_name, /*persist=*/true);
    if (RestartNow())
      return;
  }
  QuitNow();
}
#endif

void ApplyResize() {
  if (!g_backend.swap_chain->resize()) {
    // The back buffers are released by now and there are none to replace them,
    // so the present path has nothing to draw into until a later resize takes.
    // Keep asking: a transient failure (the window mid-drag, a mode change)
    // recovers on its own.
    ++g_resize_failures;
    g_resize_pending.store(true, std::memory_order_release);
    if (g_resize_failures == 1) {
      REXLOG_WARN(
          "native_renderer: Plume swap chain resize failed, 0x{:08X} (device removed reason "
          "0x{:08X}); the window will stay frozen until one succeeds",
          uint32_t(g_backend.swap_chain->getLastError()),
          uint32_t(g_backend.swap_chain->getDeviceRemovedReason()));
    }
    return;
  }
  if (g_resize_failures != 0) {
    REXLOG_DEBUG("native_renderer: Plume swap chain resize recovered after {} failure(s)",
                g_resize_failures);
    g_resize_failures = 0;
  }
  CreateFramebuffers();
  GrowReleaseSemaphores();
}

// Drops the swap chain and everything indexed by its images. Presenting is
// skipped until a new surface arrives; the guest's work is still submitted.
void ReleaseSwapChain() {
  if (!g_backend.swap_chain)
    return;
  WaitForAllSlots();
  g_backend.framebuffers.clear();
  g_backend.swap_chain.reset();
}

bool RebuildSwapChain(void* window_handle) {
  ReleaseSwapChain();
  if (window_handle == nullptr)
    return false;

  g_backend.swap_chain = g_backend.queue->createSwapChain(
      RenderSwapChainDesc(static_cast<RenderWindow>(window_handle), kSwapChainFormat,
                          kSwapChainBuffers, g_present_wait, kMaxFrameLatency));
  if (!g_backend.swap_chain) {
    REXLOG_ERROR("native_renderer: could not rebuild the swap chain on the new window surface");
    return false;
  }
  // Same order as init: vsync before the first resize, since on Vulkan the
  // present mode is what the resize bakes in.
  const bool vsync = g_vsync_wanted.load(std::memory_order_acquire);
  g_backend.swap_chain->setVsyncEnabled(vsync);
  g_vsync_applied.store(vsync, std::memory_order_relaxed);
  g_backend.swap_chain->resize();
  CreateFramebuffers();
  GrowReleaseSemaphores();
  REXLOG_DEBUG("native_renderer: swap chain rebuilt on the new window surface, {}x{}",
               g_backend.swap_chain->getWidth(), g_backend.swap_chain->getHeight());
  return true;
}

// Applies a pending surface loss or replacement. Called first thing in every
// present, since either can leave the swap chain absent.
void ApplySurfaceChanges() {
  if (g_surface_lost.exchange(false, std::memory_order_acq_rel))
    ReleaseSwapChain();
  if (void* surface = g_pending_surface.exchange(nullptr, std::memory_order_acq_rel))
    RebuildSwapChain(surface);
}

}  // namespace

RenderDevice* PlumeDevice() {
  return g_ready.load(std::memory_order_acquire) ? g_backend.device.get() : nullptr;
}

RenderCommandQueue* PlumeQueue() {
  return g_ready.load(std::memory_order_acquire) ? g_backend.queue.get() : nullptr;
}

RenderShaderFormat PlumeShaderFormat() {
  return g_ready.load(std::memory_order_acquire)
             ? g_backend.render_interface->getCapabilities().shaderFormat
             : RenderShaderFormat::UNKNOWN;
}

RenderFormat PlumeSwapChainFormat() { return kSwapChainFormat; }

uint32_t PlumeFrameSlot() { return g_slot; }

RenderCommandList* PlumeFrameCommands() {
  if (!g_ready.load(std::memory_order_acquire))
    return nullptr;
  if (!g_recording) {
    g_slots[g_slot].command_list->begin();
    g_recording = true;
    ++g_frame_serial;
  }
  return g_slots[g_slot].command_list.get();
}

uint64_t PlumeFrameSerial() { return g_frame_serial; }

void PlumeSetOverlayDrawer(rex::ui::UIDrawer* drawer) { g_overlay = drawer; }

bool InitPlumeBackend(void* window_handle, void* window_view) {
  if (g_ready.load(std::memory_order_acquire))
    return true;
  if (window_handle == nullptr) {
    REXLOG_ERROR("native_renderer: no native window handle, cannot bring up Plume");
    return false;
  }
#if defined(__APPLE__)
  // Plume only asserts the layer is non-null, which a release build drops.
  if (window_view == nullptr) {
    REXLOG_ERROR("native_renderer: no CAMetalLayer for the game window, cannot bring up Plume");
    return false;
  }
  const RenderWindow render_window{window_handle, window_view};
  // Before anything touches volk.
  BootstrapAppleVulkanRuntime();
#else
  (void)window_view;
  const RenderWindow render_window = static_cast<RenderWindow>(window_handle);
#endif
#if defined(__ANDROID__)
  SetVulkanGetInstanceProcAddr(LoadSelectedVulkanDriver());
#endif

  // D3D12 on Windows unless gpu_backend asks for Vulkan, Vulkan elsewhere.
  // Plume's Vulkan backend goes through volk and loads the loader at runtime,
  // so this does not add a link time dependency on a Vulkan SDK.
  //
  // NOCTURNE_RENDER_API=vulkan (or d3d12) overrides the cvar without touching
  // the player's settings.
#ifdef _WIN32
  std::string backend = rex::cvar::GetFlagByName("gpu_backend");
  if (const char* api_override = std::getenv("NOCTURNE_RENDER_API"))
    backend = api_override;
  const bool force_vulkan = backend == "vulkan" || backend == "Vulkan" || backend == "vk";
  REXLOG_INFO("native_renderer: gpu_backend is \"{}\"{}", backend,
              std::getenv("NOCTURNE_RENDER_API") ? " (from NOCTURNE_RENDER_API)" : "");
  if (force_vulkan) {
    g_backend.render_interface = CreateVulkanInterface();
    g_backend.api_name = "Vulkan";
    if (!g_backend.render_interface) {
      REXLOG_WARN("native_renderer: no Vulkan interface could be created, falling back to D3D12");
      g_backend.render_interface = CreateD3D12Interface();
      g_backend.api_name = "D3D12";
    }
  } else {
    g_backend.render_interface = CreateD3D12Interface();
    g_backend.api_name = "D3D12";
    if (!g_backend.render_interface) {
      g_backend.render_interface = CreateVulkanInterface();
      g_backend.api_name = "Vulkan";
    }
  }
#else
  g_backend.render_interface = CreateVulkanInterface();
  g_backend.api_name = "Vulkan";
#endif
  if (!g_backend.render_interface) {
    REXLOG_ERROR("native_renderer: could not create any Plume render interface");
    return false;
  }

  // vulkan_device indexes vkEnumeratePhysicalDevices, which is also the order
  // Plume lists its device names in. Plume falls back to its own pick when the
  // name is empty or no longer present.
  std::string preferred_device;
  if (g_backend.api_name == "Vulkan") {
    const std::string index_text = rex::cvar::GetFlagByName("vulkan_device");
    const int index = index_text.empty() ? -1 : std::atoi(index_text.c_str());
    const auto& names = g_backend.render_interface->getDeviceNames();
    if (index >= 0 && size_t(index) < names.size())
      preferred_device = names[size_t(index)];
  }
  g_backend.device = g_backend.render_interface->createDevice(preferred_device);
  if (!g_backend.device) {
    REXLOG_ERROR("native_renderer: Plume {} interface created no device", g_backend.api_name);
    g_backend.render_interface.reset();
    return false;
  }

  g_backend.queue = g_backend.device->createCommandQueue(RenderCommandListType::DIRECT);
  for (FrameSlot& slot : g_slots) {
    slot.command_list = g_backend.queue->createCommandList();
    slot.fence = g_backend.device->createCommandFence();
    slot.acquire_semaphore = g_backend.device->createCommandSemaphore();
  }
  g_slot = 0;

  // Present wait makes vsync usable on a frame-clocked engine. Without it
  // Plume's D3D12 present issues `Present(1, 0)`, which blocks on the vblank
  // and quantises the achieved rate to 60/30/20. Gated on the capability
  // because wait() and present() both assert on it; Vulkan needs
  // VK_KHR_present_wait.
  g_present_wait = g_backend.device->getCapabilities().presentWait;
  g_backend.swap_chain = g_backend.queue->createSwapChain(RenderSwapChainDesc(
      render_window, kSwapChainFormat, kSwapChainBuffers, g_present_wait, kMaxFrameLatency));
  if (!g_backend.swap_chain) {
    REXLOG_ERROR("native_renderer: Plume could not create a swap chain on the game window");
    ShutdownPlumeBackend();
    return false;
  }

  // Plume's swap chains come up with vsync on, so apply the setting. The cvar
  // is hot-reloadable: the callback records the request and the next present
  // applies it.
  const bool vsync = ReadVsyncCvar();
  g_backend.swap_chain->setVsyncEnabled(vsync);
  g_vsync_wanted.store(vsync, std::memory_order_release);
  g_vsync_applied.store(vsync, std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("vsync", [](std::string_view, std::string_view) {
    g_vsync_wanted.store(ReadVsyncCvar(), std::memory_order_release);
  });

  // The swap chain has no textures until the first resize, so this is required
  // rather than defensive.
  g_backend.swap_chain->resize();
  CreateFramebuffers();
  GrowReleaseSemaphores();

  const RenderDeviceDescription& description = g_backend.device->getDescription();
  REXLOG_INFO(
      "native_renderer: Plume up on {} using \"{}\", swap chain {}x{} with {} buffers, vsync {}, "
      "present wait {}.",
      g_backend.api_name, description.name, g_backend.swap_chain->getWidth(),
      g_backend.swap_chain->getHeight(), g_backend.swap_chain->getTextureCount(),
      vsync ? "on" : "off", g_present_wait ? "on" : "off (vsync will quantise the frame rate)");

  g_ready.store(true, std::memory_order_release);
  return true;
}

void PlumeGuestExtent(uint32_t* width, uint32_t* height) {
  if (!g_backend.swap_chain || g_backend.swap_chain->isEmpty())
    return;
  // The largest 16:9 that fits, which is what CompositeGuestImage would
  // letterbox to anyway, so the blit is 1:1.
  const uint64_t window_width = g_backend.swap_chain->getWidth();
  const uint64_t window_height = g_backend.swap_chain->getHeight();
  uint64_t w = window_width, h = window_width * 9 / 16;
  if (h > window_height) {
    h = window_height;
    w = window_height * 16 / 9;
  }
  *width = uint32_t(std::clamp<uint64_t>(w, 16, 8192));
  *height = uint32_t(std::clamp<uint64_t>(h, 9, 8192));
}

bool PlumeBackendReady() { return g_ready.load(std::memory_order_acquire); }

void PlumeSurfaceLost() {
  if (!g_ready.load(std::memory_order_acquire))
    return;
  g_pending_surface.store(nullptr, std::memory_order_release);
  g_surface_lost.store(true, std::memory_order_release);
}

void PlumeSurfaceRestored(void* window_handle) {
  if (!g_ready.load(std::memory_order_acquire) || window_handle == nullptr)
    return;
  g_pending_surface.store(window_handle, std::memory_order_release);
}

void PlumePresentFrame() {
  if (!g_ready.load(std::memory_order_acquire))
    return;

  // Before anything that touches the swap chain, since these can leave it
  // absent (backgrounded on Android); the frame is then submitted unpresented.
  ApplySurfaceChanges();
  const bool have_swap_chain = g_backend.swap_chain != nullptr;

  // Before the resize check, because on Vulkan a present mode change is exactly
  // what needsResize() reports.
  if (have_swap_chain)
    ApplyVsyncIfChanged();

  // Both the queued resize and the swap chain's own "I need one" answer, since
  // a minimise or a DPI change can invalidate it without an event we hooked.
  if (have_swap_chain && (g_resize_pending.exchange(false, std::memory_order_acq_rel) ||
                          g_backend.swap_chain->needsResize())) {
    // A resize replaces the swap chain's textures, and a frame still in flight
    // is rendering into one of them.
    WaitForAllSlots();
    ApplyResize();
  }

  // The guest's draws are already recorded into this slot's command list, so
  // from here on the frame is always submitted, presented or not: its slot's
  // resources are only recycled once its fence has been waited on.
  FrameSlot& slot = g_slots[g_slot];
  RenderCommandList* commands = PlumeFrameCommands();

  // A minimised window has a zero sized swap chain and nothing to present to.
  bool presenting =
      have_swap_chain && !g_backend.swap_chain->isEmpty() && !g_backend.framebuffers.empty();

  // Bounds how far ahead of the display the frame loop may run. `Present(0, 0)`
  // does not block, so without this the queue would fill to the buffer count and
  // every frame would carry that much added input lag.
  if (presenting && g_present_wait) {
    if (g_present_debt > 0)
      --g_present_debt;
    else
      g_backend.swap_chain->wait();
  }

  uint32_t image = 0;
  if (presenting && !g_backend.swap_chain->acquireTexture(slot.acquire_semaphore.get(), &image)) {
    // Usually means the swap chain went out of date between the check above and
    // here; the next frame's resize picks it up.
    if (++g_acquire_failures == 1) {
      REXLOG_WARN(
          "native_renderer: Plume failed to acquire a swap chain texture; frames will be "
          "dropped until it recovers");
    }
    g_resize_pending.store(true, std::memory_order_release);
    OwePresentWait();
    presenting = false;
  } else if (presenting && image >= g_backend.framebuffers.size()) {
    OwePresentWait();
    presenting = false;
  }

  if (presenting) {
    RenderTexture* backbuffer = g_backend.swap_chain->getTexture(image);
    const uint32_t width = g_backend.swap_chain->getWidth();
    const uint32_t height = g_backend.swap_chain->getHeight();

    commands->barriers(RenderBarrierStage::GRAPHICS,
                       RenderTextureBarrier(backbuffer, RenderTextureLayout::COLOR_WRITE));
    CompositeGuestImage(commands, g_backend.framebuffers[image].get(), width, height);
    commands->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
    commands->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));

    // The SDK's overlays record straight into this frame. There is no presenter
    // and no separate UI pass in detached mode: the drawer is handed the live
    // command list through the draw context and draws into the image we are
    // about to present.
    if (g_overlay != nullptr) {
      PlumeUIDrawContext context(width, height, commands, kSwapChainFormat);
      g_overlay->Draw(context);
    }

    commands->barriers(RenderBarrierStage::NONE,
                       RenderTextureBarrier(backbuffer, RenderTextureLayout::PRESENT));
  }
  commands->end();
  g_recording = false;

  const RenderCommandList* submit = commands;
  if (presenting) {
    RenderCommandSemaphore* wait = slot.acquire_semaphore.get();
    RenderCommandSemaphore* signal = g_backend.release_semaphores[image].get();
    g_backend.queue->executeCommandLists(&submit, 1, &wait, 1, &signal, 1, slot.fence.get());

    // A failed Present does not top the frame latency semaphore back up either,
    // so it owes the same debt an early return does.
    if (!g_backend.swap_chain->present(image, &signal, 1)) {
      OwePresentWait();
      if (++g_present_failures == 1) {
        REXLOG_WARN(
            "native_renderer: Plume present failed, 0x{:08X} (device removed reason 0x{:08X}); "
            "the swap chain may be out of date",
            uint32_t(g_backend.swap_chain->getLastError()),
            uint32_t(g_backend.swap_chain->getDeviceRemovedReason()));
      }
      g_resize_pending.store(true, std::memory_order_release);
    }
  } else {
    g_backend.queue->executeCommandLists(&submit, 1, nullptr, 0, nullptr, 0, slot.fence.get());
  }
  slot.submitted = true;

  // Do not wait for the frame just submitted. It runs on the GPU while the CPU
  // records the next one; the only thing waited on is the frame
  // kFramesInFlight back, which by now has had a full frame to finish.
  g_slot = (g_slot + 1) % kFramesInFlight;
  WaitForSlot(g_slots[g_slot]);
  GuestFrameRetired(g_slot);

#if defined(__ANDROID__)
  // A few seconds of real frames, so a driver that dies compiling the first
  // pipelines still gets the picker on the next launch.
  if (presenting && g_frames_presented == 300)
    MarkRendererStarted();
#endif

  ++g_frames_presented;
  if (have_swap_chain)
    SettleWindowSize();
#ifdef _WIN32
  if (have_swap_chain && (g_present_failures != 0 || g_resize_failures != 0) && DeviceLost())
    PromptDeviceLost();
#endif
  if (g_frames_presented % 600 == 0) {
    REXLOG_DEBUG(
        "native_renderer: Plume presented {} frames ({} acquire failures, {} present failures, "
        "{} resize failures, {} wait(s) owed)",
        g_frames_presented, g_acquire_failures, g_present_failures, g_resize_failures,
        g_present_debt);
  }
}

void PlumePresentOverlayOnly() {
  if (!g_ready.load(std::memory_order_acquire) || g_recording)
    return;

  ApplySurfaceChanges();
  if (!g_backend.swap_chain)
    return;

  ApplyVsyncIfChanged();
  if (g_resize_pending.exchange(false, std::memory_order_acq_rel) ||
      g_backend.swap_chain->needsResize()) {
    WaitForAllSlots();
    ApplyResize();
  }
  if (g_backend.swap_chain->isEmpty() || g_backend.framebuffers.empty())
    return;

  if (g_present_wait) {
    if (g_present_debt > 0)
      --g_present_debt;
    else
      g_backend.swap_chain->wait();
  }

  FrameSlot& slot = g_slots[g_slot];
  uint32_t image = 0;
  if (!g_backend.swap_chain->acquireTexture(slot.acquire_semaphore.get(), &image)) {
    g_resize_pending.store(true, std::memory_order_release);
    OwePresentWait();
    return;
  }
  if (image >= g_backend.framebuffers.size()) {
    OwePresentWait();
    return;
  }

  RenderTexture* backbuffer = g_backend.swap_chain->getTexture(image);
  RenderCommandList* commands = slot.command_list.get();
  const uint32_t width = g_backend.swap_chain->getWidth();
  const uint32_t height = g_backend.swap_chain->getHeight();
  commands->begin();
  commands->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(backbuffer, RenderTextureLayout::COLOR_WRITE));
  commands->setFramebuffer(g_backend.framebuffers[image].get());
  commands->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
  commands->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));
  commands->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  if (g_overlay != nullptr) {
    PlumeUIDrawContext context(width, height, commands, kSwapChainFormat);
    g_overlay->Draw(context);
  }
  commands->barriers(RenderBarrierStage::NONE,
                     RenderTextureBarrier(backbuffer, RenderTextureLayout::PRESENT));
  commands->end();

  const RenderCommandList* submit = commands;
  RenderCommandSemaphore* wait = slot.acquire_semaphore.get();
  RenderCommandSemaphore* signal = g_backend.release_semaphores[image].get();
  g_backend.queue->executeCommandLists(&submit, 1, &wait, 1, &signal, 1, slot.fence.get());
  if (!g_backend.swap_chain->present(image, &signal, 1)) {
    OwePresentWait();
    g_resize_pending.store(true, std::memory_order_release);
  }
  // Not marked submitted: waiting here keeps this out of the guest's frame
  // accounting, which would otherwise retire a frame index it never recorded.
  g_backend.queue->waitForCommandFence(slot.fence.get());
}

void PlumeNotifyResize(uint32_t pixel_width, uint32_t pixel_height) {
  (void)pixel_width;
  (void)pixel_height;
  g_resize_pending.store(true, std::memory_order_release);
}

void ShutdownPlumeBackend() {
  g_ready.store(false, std::memory_order_release);

  // Order matters: the queue has to be idle before anything it referenced is
  // destroyed, and the swap chain has to go before the queue that made it.
  if (g_backend.queue)
    WaitForAllSlots();
  if (g_backend.swap_chain && g_present_wait)
    g_backend.swap_chain->wait();
  ShutdownGuestDraw();

  g_backend.framebuffers.clear();
  g_backend.release_semaphores.clear();
  g_backend.swap_chain.reset();
  for (FrameSlot& slot : g_slots) {
    slot.acquire_semaphore.reset();
    slot.fence.reset();
    slot.command_list.reset();
    slot.submitted = false;
  }
  g_backend.queue.reset();
  g_backend.device.reset();
  g_backend.render_interface.reset();
}

}  // namespace nocturne
