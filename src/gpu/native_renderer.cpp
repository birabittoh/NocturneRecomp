// nocturnerecomp - ReXGlue Recompiled Project
//
// See native_renderer.h for what this is and why there is no ring buffer.

#include "native_renderer.h"

#include "guest_shaders.h"

#include <rex/logging.h>
#include <rex/ui/window.h>

#if defined(PLUME_SDL3_VULKAN_ENABLED) || defined(__ANDROID__) || defined(__APPLE__)
#include <SDL3/SDL.h>
#endif

#include "native_renderer_plume.h"

#if defined(__ANDROID__)
#include "android_gpu_driver.h"
#endif

namespace nocturne {

namespace {
std::function<void()> g_guest_frame_callback;

#if defined(__ANDROID__)
// Reads the current ANativeWindow back out of SDL. Only valid to call once SDL
// has published a new one, which is what OnSurfaceRestored signals.
void* CurrentAndroidWindowHandle() {
  int window_count = 0;
  SDL_Window** windows = SDL_GetWindows(&window_count);
  void* handle = nullptr;
  if (windows && window_count > 0) {
    handle = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]),
                                    SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
  }
  SDL_free(windows);
  return handle;
}

// Android throws the ANativeWindow away whenever the app leaves the
// foreground; without rebuilding the swap chain on the new one the game keeps
// running behind a black window.
class SurfaceListener final : public rex::ui::WindowListener {
 public:
  void OnSurfaceLost(rex::ui::UIEvent&) override { PlumeSurfaceLost(); }
  void OnSurfaceRestored(rex::ui::UIEvent&) override {
    PlumeSurfaceRestored(CurrentAndroidWindowHandle());
  }
};

// Outlives the window on purpose: removed only at process exit.
SurfaceListener g_surface_listener;
#endif  // __ANDROID__
}  // namespace

void SetGuestFrameCallback(std::function<void()> callback) {
  g_guest_frame_callback = std::move(callback);
}

void NotifyGuestFrame() {
  if (g_guest_frame_callback)
    g_guest_frame_callback();
}

void InitNativeRenderer(rex::ui::Window* window) {
  static bool initialized = false;
  if (initialized)
    return;
  initialized = true;

  if (!LoadGuestShaders())
    REXLOG_ERROR("native renderer: the embedded guest shader pack did not load");

  // The window exists and is ours to render into. The SDK presents nothing in
  // this mode and draws no overlays until it is handed a drawer.
  //
  // Plume's SDL/Vulkan backend takes an SDL_Window* and creates its own
  // VkSurfaceKHR. GetNativeWindowHandle() only returns an HWND on Windows and
  // null elsewhere, and the SDK has no accessor for the SDL_Window behind
  // rex::ui::WindowSDL, so ask SDL: the app opens one window, the first entry.
#if defined(PLUME_SDL3_VULKAN_ENABLED)
  void* handle = nullptr;
  if (window) {
    int window_count = 0;
    SDL_Window** windows = SDL_GetWindows(&window_count);
    if (windows && window_count > 0)
      handle = windows[0];
    // SDL hands back a fresh array; only the SDL_Window pointers inside it are
    // owned by SDL and outlive it.
    SDL_free(windows);
  }
#elif defined(__ANDROID__)
  PromptGpuDriverAfterFailedStart();

  // Plume's Android backend expects an ANativeWindow*, which SDL exposes as a
  // window property.
  void* handle = window ? CurrentAndroidWindowHandle() : nullptr;
#elif defined(__APPLE__)
  // Plume's RenderWindow is a {NSWindow*, CAMetalLayer*} pair here, and the
  // layer is what vkCreateMetalSurfaceEXT needs. SDL's window has no layer of
  // its own, so attach one. Both SDL calls are main-thread only.
  void* handle = nullptr;
  void* view = nullptr;
  if (window) {
    int window_count = 0;
    SDL_Window** windows = SDL_GetWindows(&window_count);
    if (windows && window_count > 0) {
      handle = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]),
                                      SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
      // Leaked at exit on purpose: the swap chain reads the layer until the
      // last present, and SDL wants the view destroyed before its window.
      static SDL_MetalView metal_view = nullptr;
      metal_view = SDL_Metal_CreateView(windows[0]);
      if (metal_view != nullptr)
        view = SDL_Metal_GetLayer(metal_view);
      else
        REXLOG_ERROR("native_renderer: SDL_Metal_CreateView failed: {}", SDL_GetError());
    }
    SDL_free(windows);
  }
#else
  void* handle = window ? window->GetNativeWindowHandle() : nullptr;
#endif
  REXLOG_DEBUG("native_renderer: native window handle {}.", handle);

#if defined(__APPLE__)
  if (!InitPlumeBackend(handle, view)) {
#else
  if (!InitPlumeBackend(handle)) {
#endif
    REXLOG_WARN(
        "native_renderer: no host backend, so the game will run headless to a black window. "
        "Everything except presentation still works.");
  }

#if defined(__ANDROID__)
  if (window)
    window->AddListener(&g_surface_listener);
#endif
}

}  // namespace nocturne
