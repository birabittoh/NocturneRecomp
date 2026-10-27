#include "host_menu.h"

#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if REX_PLATFORM_ANDROID
#include <jni.h>

#include <SDL3/SDL.h>

#include <cstdarg>
#include <functional>
#include <string>
#include <vector>

#include "settings.h"
#include "ui_text.h"
#endif

namespace nocturne {

namespace {
// Routes the JNI callbacks (see below) back to the instance that opened the
// dialog. There is only ever one HostMenu, owned by the app for its whole
// lifetime, so a raw pointer is fine.
HostMenu* g_instance = nullptr;
}  // namespace

#if REX_PLATFORM_ANDROID

namespace {

// Menu entries, in the order NocturneRecompActivity shows them. The indices
// are what nativeHostMenuSelect switches on, so keep the two in step.
enum MenuItem : int {
  kGpuDriver = 0,
  kRestartLauncher,
  kExit,
};

std::vector<std::string> MenuItems() {
  return {Tr("host_menu", "menu_gpu_driver"), Tr("host_menu", "menu_restart_launcher"),
          Tr("host_menu", "menu_exit")};
}

// The strings NocturneRecompActivity draws itself, in the order of its T_*
// constants. Fetched whenever a dialog opens, so a language change shows.
std::vector<std::string> JavaTexts() {
  return {Tr("host_menu", "driver_invalid"),
          Tr("host_menu", "driver_installed"),
          Tr("host_menu", "driver_system"),
          Tr("host_menu", "driver_title_startup"),
          Tr("host_menu", "driver_title"),
          Tr("host_menu", "start"),
          Tr("host_menu", "ok"),
          Tr("host_menu", "install"),
          Tr("host_menu", "delete"),
          Tr("host_menu", "driver_changed_title"),
          Tr("host_menu", "driver_changed_message"),
          Tr("host_menu", "close_game"),
          Tr("host_menu", "later")};
}

JNIEnv* JniEnv() { return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()); }

// Calls a void method on the activity. `signature` and the varargs are the
// usual JNI ones.
void CallActivityVoid(const char* name, const char* signature, ...) {
  JNIEnv* env = JniEnv();
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = env->GetMethodID(cls, name, signature);
  if (mid) {
    va_list args;
    va_start(args, signature);
    env->CallVoidMethodV(activity, mid, args);
    va_end(args);
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}

jobjectArray ToJavaStringArray(JNIEnv* env, const std::vector<std::string>& items) {
  jclass string_cls = env->FindClass("java/lang/String");
  jobjectArray array = env->NewObjectArray(static_cast<jsize>(items.size()), string_cls, nullptr);
  for (size_t i = 0; i < items.size(); ++i) {
    jstring s = env->NewStringUTF(items[i].c_str());
    env->SetObjectArrayElement(array, static_cast<jsize>(i), s);
    env->DeleteLocalRef(s);
  }
  env->DeleteLocalRef(string_cls);
  return array;
}

void ShowNativeMenu() {
  JNIEnv* env = JniEnv();
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = env->GetMethodID(cls, "showHostMenu", "([Ljava/lang/String;)V");
  if (mid) {
    jobjectArray array = ToJavaStringArray(env, MenuItems());
    env->CallVoidMethod(activity, mid, array);
    env->DeleteLocalRef(array);
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}

void RunOnUiThread(std::function<void()> work) {
  if (!g_instance) {
    return;
  }
  g_instance->window_for_callback()->app_context().CallInUIThread(std::move(work));
}

}  // namespace

// The JNI callbacks below all arrive on an Android thread that is not the one
// owning the SDL window, so anything touching engine state is marshalled
// through CallInUIThread; that is the same hazard and fix as settings.cpp's
// RestartNow().

extern "C" JNIEXPORT void JNICALL
Java_com_birabittoh_nocturnerecomp_NocturneRecompActivity_nativeHostMenuSelect(JNIEnv*, jobject,
                                                                            jint index) {
  if (!g_instance) {
    return;
  }
  rex::ui::Window* window = g_instance->window_for_callback();

  switch (index) {
    case kGpuDriver:
      CallActivityVoid("showGpuDriverDialog", "(Z)V", static_cast<jboolean>(JNI_FALSE));
      break;
    case kRestartLauncher:
      RestartToStartScreen();
      break;
    case kExit:
      RunOnUiThread([window] { window->RequestClose(); });
      break;
    default:  // The dialog was dismissed.
      break;
  }
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_birabittoh_nocturnerecomp_NocturneRecompActivity_nativeHostTexts(JNIEnv* env, jobject) {
  return ToJavaStringArray(env, JavaTexts());
}

extern "C" JNIEXPORT void JNICALL
Java_com_birabittoh_nocturnerecomp_NocturneRecompActivity_nativeCloseGame(JNIEnv*, jobject) {
  if (!g_instance) {
    return;
  }
  rex::ui::Window* window = g_instance->window_for_callback();
  RunOnUiThread([window] { window->RequestClose(); });
}

#endif  // REX_PLATFORM_ANDROID

bool RestartAndroidApp() {
#if REX_PLATFORM_ANDROID
  JNIEnv* env = JniEnv();
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    return false;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = env->GetMethodID(cls, "restartApp", "()V");
  if (mid) {
    env->CallVoidMethod(activity, mid);
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return mid != nullptr;
#else
  return false;
#endif
}

HostMenu::HostMenu(rex::ui::Window* window) : window_(window) {
  g_instance = this;
#if REX_PLATFORM_ANDROID
  // The SDK mod overlay's restart button goes through Relaunch(), which would fork.
  rex::platform::process::SetRelaunchHandler(RestartAndroidApp);
  // "BrowserBack" is the Android back button (trapped by SDL_HINT_
  // ANDROID_TRAP_BACK_BUTTON in window_sdl.cpp so it arrives as a key event
  // instead of backgrounding the app) and doubles as a desktop keyboard's
  // dedicated back key. No gamepad bind: the game uses both Back and Start
  // (see the remap_* defaults in settings.cpp), so the pad has no spare button.
  rex::ui::RegisterBind("bind_host_menu", "BrowserBack", "Open host menu",
                        [] { ShowNativeMenu(); });
#endif
}

HostMenu::~HostMenu() {
#if REX_PLATFORM_ANDROID
  rex::platform::process::SetRelaunchHandler(nullptr);
  rex::ui::UnregisterBind("bind_host_menu");
#endif
  g_instance = nullptr;
}

}  // namespace nocturne
