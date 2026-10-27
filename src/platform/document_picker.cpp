#include "document_picker.h"

#include <memory>
#include <system_error>
#include <utility>

#include <SDL3/SDL.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app_context.h>

#include "intro_text.h"

#if REX_PLATFORM_ANDROID
#include <jni.h>
#endif

namespace nocturne {
namespace {

namespace fs = std::filesystem;

using ReadFn = std::function<void(std::optional<fs::path>)>;
using WriteFn = std::function<std::string(const fs::path&)>;
using DoneFn = std::function<void(std::optional<std::string>)>;

#if REX_PLATFORM_ANDROID

// The request Java answers; one at a time, so a single slot.
struct Pending {
  rex::ui::WindowedAppContext* context = nullptr;
  ReadFn read;
  WriteFn write;
  DoneFn done;
  fs::path staged;
};
Pending g_pending;

JNIEnv* JniEnv() { return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()); }

void CallActivity(const char* name, const char* signature, jstring arg) {
  JNIEnv* env = JniEnv();
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity)
    return;
  jclass cls = env->GetObjectClass(activity);
  if (jmethodID mid = env->GetMethodID(cls, name, signature)) {
    if (arg)
      env->CallVoidMethod(activity, mid, arg);
    else
      env->CallVoidMethod(activity, mid);
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}

// Runs on the UI thread and clears the request, so a late answer finds nothing.
void Finish(std::optional<std::string> result) {
  Pending pending = std::exchange(g_pending, {});
  if (!pending.context)
    return;
  std::error_code ec;
  if (!pending.staged.empty())
    fs::remove(pending.staged, ec);
  pending.context->CallInUIThreadDeferred([pending, result] {
    if (pending.done)
      pending.done(result);
    else if (pending.read)
      pending.read(std::nullopt);
  });
}

#else

struct DialogRequest {
  rex::ui::WindowedAppContext* context;
  ReadFn read;
  WriteFn write;
  DoneFn done;
};

// May run on the dialog's own thread, so it only posts the answer.
void SDLCALL OnPicked(void* userdata, const char* const* files, int) {
  std::shared_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
  if (!files)
    REXLOG_ERROR("The file dialog failed: {}", SDL_GetError());
  std::optional<fs::path> picked;
  if (files && files[0])
    picked = rex::to_path(std::string(files[0]));
  request->context->CallInUIThreadDeferred([request, picked]() mutable {
    if (request->read) {
      request->read(picked);
      return;
    }
    if (!picked) {
      request->done(std::nullopt);
      return;
    }
    if (!picked->has_extension())
      picked->replace_extension(".zip");
    request->done(request->write(*picked));
  });
}

#endif

}  // namespace

void PickFileToRead(rex::ui::WindowedAppContext& context, ReadFn read) {
#if REX_PLATFORM_ANDROID
  g_pending = {&context, std::move(read), {}, {}, {}};
  CallActivity("requestOpenDocument", "()V", nullptr);
#else
  // No filter: a PS3 SAVEDATA has no extension to match.
  auto* request = new DialogRequest{&context, std::move(read), {}, {}};
  SDL_ShowOpenFileDialog(OnPicked, request, nullptr, nullptr, 0, nullptr, false);
#endif
}

void PickFileToWrite(rex::ui::WindowedAppContext& context, const std::string& suggested_name,
                     const fs::path& staging_dir, WriteFn write, DoneFn done) {
#if REX_PLATFORM_ANDROID
  g_pending = {&context, {}, std::move(write), std::move(done), staging_dir / suggested_name};
  JNIEnv* env = JniEnv();
  if (!env)
    return;
  jstring title = env->NewStringUTF(suggested_name.c_str());
  CallActivity("requestCreateDocument", "(Ljava/lang/String;)V", title);
  env->DeleteLocalRef(title);
#else
  (void)staging_dir;
  static const SDL_DialogFileFilter kFilters[] = {{"Zip archive", "zip"}};
  auto* request = new DialogRequest{&context, {}, std::move(write), std::move(done)};
  SDL_ShowSaveFileDialog(OnPicked, request, nullptr, kFilters, 1, suggested_name.c_str());
#endif
}

}  // namespace nocturne

#if REX_PLATFORM_ANDROID

// These arrive on a Java worker thread, never the SDL one.

// `path` is the cache copy of the picked document, or null when staging it failed.
extern "C" JNIEXPORT void JNICALL
Java_com_birabittoh_nocturnerecomp_NocturneRecompActivity_nativeDocumentPicked(JNIEnv* env, jobject,
                                                                            jstring path) {
  using namespace nocturne;
  std::filesystem::path picked;
  if (path) {
    const char* chars = env->GetStringUTFChars(path, nullptr);
    picked = rex::to_path(std::string(chars ? chars : ""));
    env->ReleaseStringUTFChars(path, chars);
  }
  auto* context = g_pending.context;
  if (!context || !g_pending.read)
    return;
  // Synchronous, since Java deletes the copy once this returns.
  context->CallInUIThreadSynchronous([picked] {
    Pending pending = std::exchange(g_pending, {});
    if (pending.read)
      pending.read(picked);
  });
}

// Fills the staged file and returns its path for Java to copy out, or null.
extern "C" JNIEXPORT jstring JNICALL
Java_com_birabittoh_nocturnerecomp_NocturneRecompActivity_nativeDocumentWrite(JNIEnv* env, jobject) {
  using namespace nocturne;
  auto* context = g_pending.context;
  if (!context || !g_pending.write)
    return nullptr;
  std::string error;
  std::filesystem::path staged;
  context->CallInUIThreadSynchronous([&] {
    staged = g_pending.staged;
    std::error_code ec;
    std::filesystem::create_directories(staged.parent_path(), ec);
    error = g_pending.write(staged);
    if (!error.empty())
      Finish(error);
  });
  return error.empty() ? env->NewStringUTF(staged.string().c_str()) : nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_birabittoh_nocturnerecomp_NocturneRecompActivity_nativeDocumentWritten(JNIEnv*, jobject,
                                                                             jboolean ok) {
  using namespace nocturne;
  if (auto* context = g_pending.context)
    context->CallInUIThreadSynchronous([ok] {
      Finish(ok ? std::string() : std::string(Tr("saves", "err_write_location")));
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_birabittoh_nocturnerecomp_NocturneRecompActivity_nativeDocumentCancelled(JNIEnv*, jobject) {
  using namespace nocturne;
  if (auto* context = g_pending.context)
    context->CallInUIThreadSynchronous([] { Finish(std::nullopt); });
}

#endif  // REX_PLATFORM_ANDROID
