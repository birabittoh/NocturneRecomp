// nocturnerecomp - ReXGlue Recompiled Project
//
// See native_renderer.h for what this is and why there is no ring buffer.

#include "native_renderer.h"

#include "guest_shaders.h"

#include <rex/logging.h>
#include <rex/ui/window.h>

#if defined(PLUME_SDL3_VULKAN_ENABLED)
#include <SDL3/SDL.h>
#endif

#include "native_renderer_plume.h"

namespace nocturne {

namespace {
std::function<void()> g_guest_frame_callback;
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
#else
  void* handle = window ? window->GetNativeWindowHandle() : nullptr;
#endif
  REXLOG_DEBUG("native_renderer: native window handle {}.", handle);

  if (!InitPlumeBackend(handle)) {
    REXLOG_WARN(
        "native_renderer: no host backend, so the game will run headless to a black window. "
        "Everything except presentation still works.");
  }
}

}  // namespace nocturne
