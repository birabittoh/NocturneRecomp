// nocturnerecomp - ReXGlue Recompiled Project
//
// The host side of the native renderer: a Plume device and swap chain on the
// game's own window. native_renderer_d3d.cpp watches what the guest asks for;
// this turns it into draws and presents once per guest swap.
//
// With no GPU plugin the SDK creates no presenter, so nothing draws, including
// the F3/F4 overlays, until this does.
//
// Kept behind a plain C++ interface with no Plume types in the header: Plume
// pulls in d3d12.h and windows.h, which stay confined to the translation units
// that include native_renderer_plume_internal.h.

#pragma once

#include <cstdint>

namespace rex::ui {
class UIDrawer;
}

namespace nocturne {

// Bring up a Plume device and a swap chain on `window_handle` (an HWND on
// Windows, an SDL_Window* on Linux, an ANativeWindow* on Android, an NSWindow*
// on Apple). Safe to call once; returns false and logs if the backend could not
// be created, in which case every other entry point here is a no-op and the
// game runs headless.
//
// `window_view` is only read on Apple, where Plume's RenderWindow is a
// {NSWindow*, CAMetalLayer*} pair and the layer becomes the Vulkan surface.
bool InitPlumeBackend(void* window_handle, void* window_view = nullptr);

// Android hands out a new ANativeWindow every time the app returns to the
// foreground and releases the old one on the way out. Both only record the
// request; the next present drops or rebuilds the swap chain, like a resize.
void PlumeSurfaceLost();
void PlumeSurfaceRestored(void* window_handle);

// True once InitPlumeBackend has succeeded.
bool PlumeBackendReady();

// Hand over the SDK's ImGui drawer so the overlays can record into the frame.
// Until this is called the frame is only the clear. Null disables them again.
void PlumeSetOverlayDrawer(rex::ui::UIDrawer* drawer);

// Draw and present one host frame. Called from the guest's Swap hook, so the
// host frame rate follows the guest's.
void PlumePresentFrame();

// The size the guest's render target should have: the swap chain's size fitted
// to the guest's 16:9. Leaves the arguments alone when there is no swap chain
// to measure, so callers pre-load a fallback.
void PlumeGuestExtent(uint32_t* width, uint32_t* height);

// The window changed size; the swap chain has to follow. Called from the app's
// pixel size hook, i.e. on the UI thread, so it only records the request and
// the next present acts on it.
void PlumeNotifyResize(uint32_t pixel_width, uint32_t pixel_height);

// Release the device and swap chain. Called on shutdown, before the window goes
// away.
void ShutdownPlumeBackend();

}  // namespace nocturne
