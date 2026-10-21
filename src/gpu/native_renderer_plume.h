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
// Windows, an SDL_Window* on Linux). Safe to call once; returns false and logs
// if the backend could not be created, in which case every other entry point
// here is a no-op and the game runs headless.
bool InitPlumeBackend(void* window_handle);

// True once InitPlumeBackend has succeeded.
bool PlumeBackendReady();

// Hand over the SDK's ImGui drawer so the overlays can record into the frame.
// Until this is called the frame is only the clear. Null disables them again.
void PlumeSetOverlayDrawer(rex::ui::UIDrawer* drawer);

// Draw and present one host frame. Called from the guest's Swap hook, so the
// host frame rate follows the guest's.
void PlumePresentFrame();

// The window changed size; the swap chain has to follow. Called from the app's
// pixel size hook, i.e. on the UI thread, so it only records the request and
// the next present acts on it.
void PlumeNotifyResize(uint32_t pixel_width, uint32_t pixel_height);

// Release the device and swap chain. Called on shutdown, before the window goes
// away.
void ShutdownPlumeBackend();

}  // namespace nocturne
