// nocturnerecomp - Custom Vulkan drivers on Android.
//
// Many Adreno phones ship a stock driver that cannot run Plume, and Android
// gives users no way to update it. libadrenotools loads a user supplied driver
// (usually Mesa Turnip) in its place. Drivers are installed and picked from a
// dialog in NocturneRecompActivity; the choice lives under the app's internal
// files directory because adrenotools cannot dlopen from shared storage.
#pragma once

namespace nocturne {

// Shows the driver picker and waits for it when the previous renderer start
// never finished, so a crashing driver can be swapped before it runs again.
// Must run before the window handle is read: installing a driver opens the
// system file picker, which takes the game's surface away.
void PromptGpuDriverAfterFailedStart();

// Loads the selected custom driver and returns its vkGetInstanceProcAddr, or
// null for the system driver. Also marks the renderer start as in progress.
void* LoadSelectedVulkanDriver();

// Clears the in progress mark once the renderer is up.
void MarkRendererStarted();

bool CustomVulkanDriverSelected();

}  // namespace nocturne
