# Custom GPU drivers (Android)

Plume needs Vulkan 1.2 and a handful of features many stock Adreno drivers do
not offer, and Android gives users no way to update the driver. On Android the
game can load a user supplied Vulkan driver (usually Mesa Turnip) through
libadrenotools instead of the system one.

## Using it

The driver dialog lists **System driver** and every installed driver:

* **Install...** takes a driver package `.zip` (the format most Android
  emulators use) or a bare driver `.so`, and selects it.
* **Delete** removes the highlighted driver.
* **OK** saves the selection.

The driver is loaded once, when the renderer starts. After a change the game
offers to close itself; Android does not let it relaunch, so start it again
from the launcher.

The dialog is reachable two ways:

* **GPU driver...** in the back button menu, while the game runs.
* Automatically at startup, when the previous start never got 300 frames on
  screen. The game waits on the dialog before creating the Vulkan instance,
  so a crashing driver (stock or custom) can always be replaced.

## Package format

A package is a zip whose root (or single top level folder) holds the driver
library and, optionally, a `meta.json`:

```json
{
  "name": "Turnip",
  "driverVersion": "v25.1.0",
  "libraryName": "vulkan.ad07XX.so"
}
```

`libraryName` names the library to load; without `meta.json` the package must
contain exactly one `.so`. `name` and `driverVersion` become the folder name
shown in the dialog.

## Implementation

* Files live under the app's internal files directory, because adrenotools
  cannot `dlopen` from shared storage:
  * `gpu_drivers/<name>/`: one extracted package.
  * `gpu_drivers/selected`: the active folder name and library name, one per
    line. Missing means the system driver.
  * `gpu_drivers/renderer_starting`: written before the Vulkan instance is
    created, removed after 300 presented frames. Its presence at startup
    triggers the dialog.
* `NocturneRecompActivity.java` owns the dialog, installation and selection.
* `src/platform/android_gpu_driver.cpp` reads the selection, opens the driver
  with `adrenotools_open_libvulkan` and hands its `vkGetInstanceProcAddr` to
  Plume (`plume::SetVulkanGetInstanceProcAddr`, which makes volk skip the
  system loader).
* libadrenotools is a submodule built only for Android. Its four hook
  libraries (`libmain_hook.so`, `libfile_redirect_hook.so`,
  `libgsl_alloc_hook.so`, `libhook_impl.so`) are loaded by name from
  `nativeLibraryDir`, so they have to be staged into `jniLibs` next to
  `libnocturnerecomp.so`, and the APK must keep `useLegacyPackaging = true`.
* With a custom driver selected, the settings overlay skips its Vulkan device
  enumeration, which would load the stock driver into the same process.
