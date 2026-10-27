#include "android_gpu_driver.h"

#if defined(__ANDROID__)

#include <dlfcn.h>
#include <jni.h>

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include <SDL3/SDL.h>
#include <adrenotools/driver.h>

#include <rex/logging.h>

namespace nocturne {
namespace {

namespace fs = std::filesystem;

// Layout shared with NocturneRecompActivity: gpu_drivers/<name>/ holds one
// driver, "selected" names the active one and its library on two lines.
fs::path DriversRoot() {
  const char* files = SDL_GetAndroidInternalStoragePath();
  return files ? fs::path(files) / "gpu_drivers" : fs::path();
}

fs::path StartingMarker() {
  const fs::path root = DriversRoot();
  return root.empty() ? root : root / "renderer_starting";
}

struct Selection {
  fs::path dir;
  std::string library;
};

bool ReadSelection(Selection& out) {
  const fs::path root = DriversRoot();
  if (root.empty())
    return false;
  std::ifstream file(root / "selected");
  std::string name;
  if (!std::getline(file, name) || !std::getline(file, out.library) || name.empty() ||
      out.library.empty())
    return false;
  out.dir = root / name;
  std::error_code ec;
  return fs::is_regular_file(out.dir / out.library, ec);
}

// adrenotools needs the hook libraries' directory, which is nativeLibraryDir:
// the one this library was extracted into.
std::string NativeLibraryDir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&NativeLibraryDir), &info) || !info.dli_fname)
    return {};
  return fs::path(info.dli_fname).parent_path().string() + "/";
}

std::mutex g_dialog_mutex;
std::condition_variable g_dialog_cv;
bool g_dialog_open = false;

}  // namespace

void PromptGpuDriverAfterFailedStart() {
  const fs::path marker = StartingMarker();
  std::error_code ec;
  if (marker.empty() || !fs::exists(marker, ec))
    return;
  fs::remove(marker, ec);

  REXLOG_WARN("gpu_driver: the renderer did not finish starting last time, asking for a driver");
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity)
    return;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = env->GetMethodID(cls, "showGpuDriverDialog", "(Z)V");
  if (mid) {
    {
      std::lock_guard lock(g_dialog_mutex);
      g_dialog_open = true;
    }
    env->CallVoidMethod(activity, mid, JNI_TRUE);
    std::unique_lock lock(g_dialog_mutex);
    g_dialog_cv.wait(lock, [] { return !g_dialog_open; });
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}

void* LoadSelectedVulkanDriver() {
  const fs::path marker = StartingMarker();
  if (!marker.empty()) {
    std::error_code ec;
    fs::create_directories(marker.parent_path(), ec);
    std::ofstream(marker).put('\n');
  }

  Selection selection;
  if (!ReadSelection(selection))
    return nullptr;

  const std::string hook_dir = NativeLibraryDir();
  const std::string driver_dir = selection.dir.string() + "/";
  // Only read below API 29, which has no memfd to patch libraries in.
  const char* cache = SDL_GetAndroidCachePath();
  const std::string tmp_dir = cache ? std::string(cache) + "/" : std::string();
  void* handle = adrenotools_open_libvulkan(
      RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, tmp_dir.empty() ? nullptr : tmp_dir.c_str(),
      hook_dir.c_str(), driver_dir.c_str(), selection.library.c_str(), nullptr, nullptr);
  if (!handle) {
    const char* error = dlerror();
    REXLOG_ERROR("gpu_driver: could not load {} from {}, using the system driver: {}",
                 selection.library, driver_dir, error ? error : "unknown error");
    return nullptr;
  }
  void* proc = dlsym(handle, "vkGetInstanceProcAddr");
  if (!proc) {
    REXLOG_ERROR("gpu_driver: {} has no vkGetInstanceProcAddr, using the system driver",
                 selection.library);
    return nullptr;
  }
  REXLOG_INFO("gpu_driver: using custom driver {} ({})", selection.dir.filename().string(),
              selection.library);
  return proc;
}

void MarkRendererStarted() {
  const fs::path marker = StartingMarker();
  std::error_code ec;
  if (!marker.empty())
    fs::remove(marker, ec);
}

bool CustomVulkanDriverSelected() {
  Selection selection;
  return ReadSelection(selection);
}

extern "C" JNIEXPORT void JNICALL
Java_com_birabittoh_nocturnerecomp_NocturneRecompActivity_nativeGpuDriverDialogClosed(JNIEnv*,
                                                                                   jobject) {
  {
    std::lock_guard lock(g_dialog_mutex);
    g_dialog_open = false;
  }
  g_dialog_cv.notify_all();
}

}  // namespace nocturne

#endif  // __ANDROID__
