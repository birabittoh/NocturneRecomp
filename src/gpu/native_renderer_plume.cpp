// nocturnerecomp - ReXGlue Recompiled Project
//
// See native_renderer_plume.h.

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

#include "native_renderer_plume_internal.h"

namespace plume {
#ifdef _WIN32
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}  // namespace plume

namespace nocturne {
namespace {

using namespace plume;

// B8G8R8A8 rather than R8G8B8A8 because every Windows swap chain supports it
// natively; the other costs a conversion on present.
constexpr RenderFormat kSwapChainFormat = RenderFormat::B8G8R8A8_UNORM;

// Three, not two. With two there is exactly one spare buffer, so a frame cannot
// start until the previous one's flip has retired its buffer, which under vsync
// means waiting for a vblank and quantises the frame rate to 60/30/20.
constexpr uint32_t kSwapChainBuffers = 3;

// How many presents may be outstanding before the frame loop blocks. Also the
// value Plume hands to SetMaximumFrameLatency on D3D12, where 0 is not a legal
// argument.
constexpr uint32_t kMaxFrameLatency = 2;

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

bool InitPlumeBackend(void* window_handle) {
  if (g_ready.load(std::memory_order_acquire))
    return true;
  if (window_handle == nullptr) {
    REXLOG_ERROR("native_renderer: no native window handle, cannot bring up Plume");
    return false;
  }
  const RenderWindow render_window = static_cast<RenderWindow>(window_handle);

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

bool PlumeBackendReady() { return g_ready.load(std::memory_order_acquire); }

void PlumePresentFrame() {
  if (!g_ready.load(std::memory_order_acquire) || !g_backend.swap_chain)
    return;

  // Before the resize check, because on Vulkan a present mode change is exactly
  // what needsResize() reports.
  ApplyVsyncIfChanged();

  // Both the queued resize and the swap chain's own "I need one" answer, since
  // a minimise or a DPI change can invalidate it without an event we hooked.
  if (g_resize_pending.exchange(false, std::memory_order_acq_rel) ||
      g_backend.swap_chain->needsResize()) {
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
  bool presenting = !g_backend.swap_chain->isEmpty() && !g_backend.framebuffers.empty();

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

  if (++g_frames_presented % 600 == 0) {
    REXLOG_DEBUG(
        "native_renderer: Plume presented {} frames ({} acquire failures, {} present failures, "
        "{} resize failures, {} wait(s) owed)",
        g_frames_presented, g_acquire_failures, g_present_failures, g_resize_failures,
        g_present_debt);
  }
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
