#include "game_data.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <utility>
#include <string_view>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/windowed_app_context.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

#include "game_library.h"
#include "install_pipeline.h"
#include "intro_screen.h"
#include "intro_text.h"

// The storage probes come from the SDK's GameDataSelector
// (src/system/game_data_selector.cpp), which this replaces.

REXCVAR_DEFINE_BOOL(show_start_screen, false, "Game", "Show the start screen on every launch");

namespace nocturne {
namespace {

namespace fs = std::filesystem;

// Read once and cleared, so a later launch does not repeat it.
bool ConsumeStartScreenRequest(const fs::path& flag) {
  std::error_code ec;
  return !flag.empty() && fs::remove(flag, ec);
}

// Same folder the SDK extracted into, so earlier installs are still found.
fs::path WritableBaseDir() {
#if REX_PLATFORM_ANDROID
  if (char* pref = SDL_GetPrefPath("rexglue", "nocturnerecomp")) {
    fs::path p(pref);
    SDL_free(pref);
    return p;
  }
  return "/data/local/tmp";
#else
  return rex::filesystem::GetExecutableFolder();
#endif
}

// The one candidate a player can copy files into, `/sdcard` shortened.
std::string CopyHint() {
#if REX_PLATFORM_ANDROID
  const char* external_path = SDL_GetAndroidExternalStoragePath();
  if (!external_path)
    return {};
  std::string p = (fs::path(external_path) / "assets").string();
  constexpr std::string_view kEmulated = "/storage/emulated/0";
  if (p.starts_with(kEmulated))
    p = "/sdcard" + p.substr(kEmulated.size());
  return p;
#else
  return {};
#endif
}

// Symlinks and junctions, which are never followed or deleted through.
bool IsLink(const fs::path& path) {
  std::error_code ec;
  if (fs::is_symlink(path, ec))
    return true;
#if REX_PLATFORM_WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return false;
#endif
}

// A link cannot be emptied without going through it.
bool IsDeletable(const fs::path& dir) {
  std::error_code ec;
  return !IsLink(dir) && fs::is_directory(dir, ec) && IsGameDirectory(dir);
}

bool SameDir(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  return fs::equivalent(a, b, ec) || a.lexically_normal() == b.lexically_normal();
}

struct DeleteJob {
  std::atomic<uint64_t> done{0};
  std::atomic<bool> finished{false};
  std::string error;
};

// Regular files and folders under `dir`, links skipped, not entered.
uint64_t TreeSize(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!IsLink(it->path()) && it->is_regular_file(entry_ec))
      total += it->file_size(entry_ec);
  }
  return total;
}

void DeleteTree(const fs::path& dir, DeleteJob& job) {
  std::error_code ec;
  std::vector<fs::path> folders{dir};
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    const fs::path path = it->path();
    if (IsLink(path)) {
      it.disable_recursion_pending();
      continue;
    }
    std::error_code entry_ec;
    if (it->is_directory(entry_ec)) {
      folders.push_back(path);
    } else {
      const uint64_t size = it->file_size(entry_ec);
      if (!fs::remove(path, entry_ec) && entry_ec) {
        job.error = path.string() + ": " + entry_ec.message();
        break;
      }
      job.done += size;
    }
  }
  if (job.error.empty() && ec)
    job.error = dir.string() + ": " + ec.message();
  // Deepest first; one holding a skipped link stays.
  for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
    std::error_code remove_ec;
    fs::remove(*folder, remove_ec);
  }
  job.finished = true;
}

std::string FormatSize(uint64_t bytes) {
  char text[32];
  if (bytes >= (1ull << 30))
    std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / double(1ull << 30));
  else
    std::snprintf(text, sizeof(text), "%.0f MB", double(bytes) / double(1ull << 20));
  return text;
}

std::string DeleteQuestion(const std::string& release, const fs::path& dir, uint64_t bytes) {
  std::string message = Tr(IntroText::kConfirmDelete, release);
  if (const size_t at = message.find("{}"); at != std::string::npos)
    message.replace(at, 2, FormatSize(bytes));
  return message + "\n" + dir.string();
}

// Kept alive by every callback still pending, so a dialog answering after
// the player quit lands nowhere.
struct Flow {
  GameDataOptions options;
  rex::ui::WindowedAppContext* context = nullptr;
  std::function<void(bool)> done;
  GameDataPrompt prompt;
  std::vector<GameVersion> versions;
  // The version Delete applies to, or -1.
  int selected = -1;
  // The identified source Extract installs, and the room it needs.
  std::string picked;
  SourceInfo picked_info;
  uint64_t required_bytes = 0;
  bool finished = false;
};

std::atomic<bool> g_extracting{false};
fs::path g_root;
std::vector<GameVersion> g_versions;
// The screen was asked for, so it waits for Start.
bool g_asked = false;
GameDirectories g_started;

// The library's copy of release `id`, or null.
const GameVersion* Installed(const std::vector<GameVersion>& versions, std::string_view id) {
  for (const GameVersion& version : versions) {
    if (!version.temporary && version.id == id)
      return &version;
  }
  return nullptr;
}

// The version the command line names, or null.
const GameVersion* Override(const std::vector<GameVersion>& versions) {
  const std::string key = GameDirectoryOverride();
  if (key.empty())
    return nullptr;
  for (const GameVersion& version : versions) {
    if (version.temporary || version.key == key)
      return &version;
  }
  return nullptr;
}

// What Start mounts and reads; false with nothing installed.
bool Choose(const std::vector<GameVersion>& versions, GameDirectories& out) {
  const GameVersion* world = Installed(versions, "xbla-world");
  const GameVersion* jp = Installed(versions, "xbla-jp");
  const GameVersion* given = Override(versions);
  const GameVersion* mounted = given ? given : world ? world : jp;
  if (!mounted && !versions.empty())
    mounted = &versions.front();
  if (!mounted)
    return false;
  out.mounted = mounted->dir;
  out.jp = given && given->id == "xbla-jp" ? given->dir : jp ? jp->dir : fs::path();
  return true;
}

bool NeedsPhases(const std::vector<GameVersion>& versions) {
  return std::any_of(versions.begin(), versions.end(),
                     [](const GameVersion& version) { return version.needs_phases; });
}

fs::path LibraryRoot(const GameDataOptions& options) {
  fs::path root = options.game_data_root;
#if REX_PLATFORM_ANDROID
  // Installs stay where earlier builds put them, not in the config folder.
  const auto* entry = rex::cvar::GetFlagInfo("game_data_root");
  if (!entry || entry->source == rex::cvar::Source::kDefault)
    root.clear();
#endif
  if (root.empty())
    root = WritableBaseDir() / "assets";
  return root;
}

InstallHooks MakeHooks() {
  InstallHooks hooks;
  // Without the intro the SDK's progress window shows it.
  if (IntroScreenAvailable()) {
    hooks.progress = ReportIntroProgress;
    hooks.phase = SetIntroPhase;
  }
  return hooks;
}

// Installs on a worker, so the UI thread keeps its event loop: the intro keeps
// drawing and answering input. Updates are handed over under a lock.
struct InstallJob {
  std::mutex mutex;
  std::vector<std::pair<Phase, PhaseState>> phases;
  std::string title, detail;
  float fraction = -1.0f;
  bool seen = false;
  bool posted = false;
  bool quit_requested = false;
  std::thread worker;
  fs::path dir;
  std::string error;
};

void ApplyJobUpdates(const std::shared_ptr<InstallJob>& job) {
  std::vector<std::pair<Phase, PhaseState>> phases;
  std::string title, detail;
  float fraction;
  bool seen;
  {
    std::lock_guard lock(job->mutex);
    job->posted = false;
    phases.swap(job->phases);
    title = job->title;
    detail = job->detail;
    fraction = job->fraction;
    seen = job->seen;
  }
  for (const auto& [phase, state] : phases)
    SetIntroPhase(phase, state);
  if (seen)
    ReportIntroProgress(title, fraction, detail);
}

void PostJobUpdates(const std::shared_ptr<Flow>& flow, const std::shared_ptr<InstallJob>& job) {
  if (job->posted)
    return;
  job->posted = true;
  flow->context->CallInUIThreadDeferred([job] { ApplyJobUpdates(job); });
}

struct DialogRequest {
  std::shared_ptr<Flow> flow;
  std::function<void(std::shared_ptr<Flow>, std::string)> then;
};

// May run on the dialog's own thread (Windows), so it only posts the answer.
void SDLCALL DialogCallback(void* userdata, const char* const* files, int) {
  std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
  if (!files)
    REXLOG_ERROR("The file dialog failed: {}", SDL_GetError());
  std::string picked = files && files[0] ? files[0] : "";
  auto flow = request->flow;
  auto then = std::move(request->then);
  flow->context->CallInUIThreadDeferred(
      [flow, then, picked]() { then(flow, picked); });
}

void ShowDialog(std::shared_ptr<Flow> flow,
                std::function<void(std::shared_ptr<Flow>, std::string)> then) {
  // XBLA packages have no extension, so the filter cannot narrow them down.
  static const SDL_DialogFileFilter kFilters[] = {
      {"Castlevania: Symphony of the Night (XBLA package)", "*"},
  };
  auto* request = new DialogRequest{std::move(flow), std::move(then)};
  SDL_ShowOpenFileDialog(DialogCallback, request, nullptr, kFilters, 1, nullptr, false);
}

bool Library(const GameDataPrompt& prompt) {
  return prompt.confirm.empty() && !prompt.adding && !prompt.versions.empty();
}

// Where no intro screen can draw.
GameDataChoice MessageBoxPrompt(const GameDataPrompt& prompt) {
  SDL_MessageBoxButtonData buttons[4];
  int count = 0;
  std::string message = prompt.error.empty() ? "" : prompt.error + "\n\n";
  if (Library(prompt)) {
    for (const VersionRow& row : prompt.versions)
      message += row.label + "\n";
    buttons[count++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, int(GameDataChoice::kStart),
                        Tr(IntroText::kStart)};
    if (int(prompt.versions.size()) < kMaxVersions)
      buttons[count++] = {0, int(GameDataChoice::kAddVersion), Tr(IntroText::kAddVersion)};
  } else {
    message += Tr(IntroText::kNeedFiles);
    message += std::string(" ") + Tr(IntroText::kSelectIsoOnly);
    if (!prompt.copy_hint.empty())
      message += "\n\n" + Tr(IntroText::kCopyHint, prompt.copy_hint);
    if (prompt.can_extract)
      message = Tr(IntroText::kFoundRelease, prompt.release) + "\n\n" + message;
    if (prompt.can_extract)
      buttons[count++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, int(GameDataChoice::kExtract),
                          Tr(IntroText::kExtract)};
    buttons[count++] = {prompt.can_extract ? 0u : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
                        int(GameDataChoice::kDiscImage), Tr(IntroText::kSelectFile)};
  }
  buttons[count++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, int(GameDataChoice::kQuit), Tr(IntroText::kQuit)};
  const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, nullptr, Tr(IntroText::kTitle),
                                  message.c_str(), count, buttons, nullptr};
  int id = int(GameDataChoice::kQuit);
  if (!SDL_ShowMessageBox(&box, &id))
    return GameDataChoice::kQuit;
  return GameDataChoice(id);
}

void Finish(const std::shared_ptr<Flow>& flow, bool ready) {
  if (flow->finished)
    return;
  flow->finished = true;
  HideIntroScreen();
  flow->done(ready);
}

void Ask(std::shared_ptr<Flow> flow);

// Turns Extract off while the drive the files go to cannot hold them.
void CheckSpace(Flow& flow) {
  if (!flow.required_bytes)
    return;
  std::error_code ec;
  const uint64_t available = fs::space(g_root, ec).available;
  if (ec || flow.required_bytes <= available)
    return;
  std::string message = Tr(IntroText::kNotEnoughSpace, FormatSize(flow.required_bytes));
  if (const size_t at = message.find("{}"); at != std::string::npos)
    message.replace(at, 2, FormatSize(available));
  flow.prompt.can_extract = false;
  flow.prompt.error = message;
}

// Rescans after an install or delete, keeping a command line
// directory listed, and selects `prefer` when given.
void Rescan(Flow& flow, const fs::path& prefer) {
  flow.versions = ScanLibrary(g_root);
  FindVersion(flow.versions, GameDirectoryOverride());
  RefreshRootExecutable(g_root, flow.versions);
  const int count = int(flow.versions.size());
  if (!prefer.empty()) {
    for (int i = 0; i < count; ++i) {
      if (SameDir(flow.versions[size_t(i)].dir, prefer))
        flow.selected = i;
    }
  }
  flow.selected = count ? std::clamp(flow.selected, 0, count - 1) : -1;
}

void Begin(const std::shared_ptr<Flow>& flow) {
  GameDirectories dirs;
  if (!Choose(flow->versions, dirs)) {
    Ask(flow);
    return;
  }
  g_started = dirs;
  REXLOG_INFO("Starting the game files in {}", dirs.mounted.string());
  Finish(flow, true);
}

using JobWork = std::function<std::string(const InstallHooks&, fs::path&)>;
using JobDone = std::function<void(std::shared_ptr<Flow>, std::string, fs::path)>;

std::shared_ptr<InstallJob> g_job;

// Runs `work` on a worker while the intro shows the phases, or right here
// where no intro can draw; `then` runs on the UI thread.
void StartJob(std::shared_ptr<Flow> flow, JobWork work, JobDone then) {
  if (!IntroScreenAvailable()) {
    fs::path dir;
    std::string error;
    g_extracting = true;
    try {
      error = work(MakeHooks(), dir);
    } catch (const std::exception& e) {
      error = TrErr("err_prepare_failed", e.what());
    }
    g_extracting = false;
    then(std::move(flow), std::move(error), std::move(dir));
    return;
  }
  auto job = std::make_shared<InstallJob>();
  g_job = job;
  BeginIntroWork();
  g_extracting = true;
  InstallHooks hooks;
  hooks.progress = [flow, job](const std::string& title, float fraction, const std::string& detail) {
    std::lock_guard lock(job->mutex);
    job->title = title;
    job->fraction = fraction;
    job->detail = detail;
    job->seen = true;
    PostJobUpdates(flow, job);
  };
  hooks.phase = [flow, job](Phase phase, PhaseState state) {
    std::lock_guard lock(job->mutex);
    job->phases.emplace_back(phase, state);
    PostJobUpdates(flow, job);
  };
  job->worker = std::thread([flow, job, hooks, work, then] {
    try {
      job->error = work(hooks, job->dir);
    } catch (const std::exception& e) {
      job->error = TrErr("err_prepare_failed", e.what());
    }
    flow->context->CallInUIThreadDeferred([flow, job, then] {
      job->worker.join();
      ApplyJobUpdates(job);
      g_job.reset();
      g_extracting = false;
      EndIntroWork();
      // A quit asked for mid-install is honored now.
      if (job->quit_requested) {
        Finish(flow, false);
        return;
      }
      then(flow, std::move(job->error), std::move(job->dir));
    });
  });
}

// The phases the installed versions still need, then Start when `start` (or
// back to the library).
void RunPhases(std::shared_ptr<Flow> flow, bool start) {
  std::vector<fs::path> dirs;
  for (const GameVersion& version : flow->versions) {
    if (version.needs_phases)
      dirs.push_back(version.dir);
  }
  StartJob(
      std::move(flow),
      [dirs](const InstallHooks& hooks, fs::path& out) {
        for (const fs::path& dir : dirs) {
          out = dir;
          if (std::string error = InstallDirectory(dir, hooks); !error.empty())
            return error;
        }
        return std::string();
      },
      [start](std::shared_ptr<Flow> flow, std::string error, fs::path dir) {
        Rescan(*flow, dir);
        flow->prompt.error = std::move(error);
        if (!flow->prompt.error.empty()) {
          REXLOG_ERROR("{}: {}", dir.string(), flow->prompt.error);
        } else if (start || !IntroScreenAvailable()) {
          Begin(flow);
          return;
        }
        Ask(flow);
      });
}

void OnInstalled(std::shared_ptr<Flow> flow, std::string error, fs::path dir) {
  flow->prompt.error = std::move(error);
  if (!flow->prompt.error.empty()) {
    REXLOG_ERROR("{}: {}", flow->picked, flow->prompt.error);
    // Extract again after fixing what went wrong.
    Rescan(*flow, {});
    flow->prompt.can_extract = true;
    CheckSpace(*flow);
    Ask(flow);
    return;
  }
  const SourceInfo& info = flow->picked_info;
  if (!info.release.empty())
    WriteVersionInfo(dir, info.id, info.release, info.languages);
  Rescan(*flow, dir);
  flow->picked.clear();
  flow->picked_info = {};
  flow->prompt.adding = false;
  flow->prompt.can_extract = false;
  flow->prompt.release.clear();
  flow->prompt.languages = 0;
  if (!IntroScreenAvailable()) {
    Begin(flow);
    return;
  }
  Ask(flow);
}

// Runs the phases on the identified source, into its own version folder.
void Extract(std::shared_ptr<Flow> flow) {
  if (flow->finished)
    return;
  flow->prompt.can_extract = false;
  const std::string picked = flow->picked;
  const fs::path target = FreeVersionDir(g_root, flow->picked_info.id);
  StartJob(
      std::move(flow),
      [picked, target](const InstallHooks& hooks, fs::path& dir) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        return Install(picked, target, hooks, dir);
      },
      OnInstalled);
}

// Identifies the pick; extracting waits for the player.
void OnPicked(std::shared_ptr<Flow> flow, std::string picked) {
  if (flow->finished)
    return;
  if (picked.empty()) {
    Ask(flow);
    return;
  }
  REXLOG_INFO("Selected {}", picked);
  const SourceInfo info = IdentifySource(picked);
  flow->prompt.release = info.release;
  flow->prompt.languages = info.languages;
  flow->prompt.error = info.error;
  flow->picked_info = info;
  flow->required_bytes = info.required_bytes;
  if (info.error.empty()) {
    for (const GameVersion& version : flow->versions) {
      if (!version.temporary && version.id == info.id) {
        flow->prompt.error = Tr(IntroText::kAlreadyInstalled, info.release);
        break;
      }
    }
  }
  flow->prompt.can_extract = flow->prompt.error.empty();
  flow->picked = flow->prompt.can_extract ? picked : std::string();
  if (flow->prompt.can_extract)
    CheckSpace(*flow);
  if (!info.release.empty())
    REXLOG_INFO("Identified {}", info.release);
  if (!flow->prompt.error.empty())
    REXLOG_ERROR("{}: {}", picked, flow->prompt.error);
  Ask(flow);
}

const GameVersion* Selected(const Flow& flow) {
  return flow.selected >= 0 && flow.selected < int(flow.versions.size())
             ? &flow.versions[size_t(flow.selected)]
             : nullptr;
}

void AskToDelete(std::shared_ptr<Flow> flow) {
  const GameVersion* version = Selected(*flow);
  if (version && !version->temporary && IsDeletable(version->dir))
    flow->prompt.confirm =
        DeleteQuestion(VersionLabel(*version), version->dir, TreeSize(version->dir));
  Ask(flow);
}

void DeleteSelected(std::shared_ptr<Flow> flow) {
  const GameVersion* version = Selected(*flow);
  if (!version) {
    Ask(flow);
    return;
  }
  const GameVersion removed = *version;
  if (!removed.temporary && IsDeletable(removed.dir)) {
    const uint64_t total = TreeSize(removed.dir);
    REXLOG_INFO("Deleting {}", removed.dir.string());
    BeginIntroWork();
    SetIntroPhase(Phase::kExtract, PhaseState::kSkipped);
    DeleteJob job;
    g_extracting = true;
    std::thread worker(DeleteTree, removed.dir, std::ref(job));
    while (!job.finished) {
      const uint64_t done = job.done;
      ReportIntroProgress("Deleting game files...",
                          total ? float(double(done) / double(total)) : -1.0f,
                          FormatSize(done) + " / " + FormatSize(total));
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    worker.join();
    g_extracting = false;
    EndIntroWork();
    flow->prompt.error = job.error;
  }
  Rescan(*flow, {});
  Ask(flow);
}

void OnChoice(std::shared_ptr<Flow> flow, GameDataChoice choice, int version) {
  if (flow->finished)
    return;
  if (g_job) {
    // The worker owns the files until it ends; only a quit is remembered.
    if (choice == GameDataChoice::kQuit)
      g_job->quit_requested = true;
    return;
  }
  if (version >= 0 && version < int(flow->versions.size()))
    flow->selected = version;
  flow->prompt.error.clear();
  switch (choice) {
    case GameDataChoice::kQuit:
      Finish(flow, false);
      return;
    case GameDataChoice::kStart:
      if (flow->versions.empty()) {
        Ask(flow);
        return;
      }
      if (NeedsPhases(flow->versions))
        RunPhases(flow, true);
      else
        Begin(flow);
      return;
    case GameDataChoice::kAddVersion:
      if (int(flow->prompt.versions.size()) >= kMaxVersions) {
        Ask(flow);
        return;
      }
      flow->prompt.adding = true;
      flow->prompt.can_extract = false;
      flow->prompt.release.clear();
      flow->prompt.languages = 0;
      flow->picked.clear();
      Ask(flow);
      return;
    case GameDataChoice::kCancel:
      if (!flow->prompt.confirm.empty())
        flow->prompt.confirm.clear();
      else
        flow->prompt.adding = false;
      Ask(flow);
      return;
    case GameDataChoice::kDelete:
      AskToDelete(flow);
      return;
    case GameDataChoice::kConfirmDelete:
      flow->prompt.confirm.clear();
      DeleteSelected(flow);
      return;
    case GameDataChoice::kExtract:
      if (flow->prompt.can_extract)
        Extract(flow);
      else
        Ask(flow);
      return;
    case GameDataChoice::kDiscImage:
      SetIntroBusy(true);
      ShowDialog(flow, OnPicked);
      return;
  }
}

// The rows the screen lists, from the scanned versions.
void FillVersionRows(Flow& flow) {
  GameDataPrompt& prompt = flow.prompt;
  prompt.versions.clear();
  for (const GameVersion& version : flow.versions) {
    VersionRow row;
    row.label = VersionLabel(version);
    if (version.temporary)
      row.note = Tr(IntroText::kLinked, version.dir.string());
    if (version.needs_phases)
      row.note = Tr(IntroText::kNeedsUpdate) + (row.note.empty() ? "" : "   " + row.note);
    row.languages = version.languages;
    row.can_delete = !version.temporary && IsDeletable(version.dir);
    prompt.versions.push_back(std::move(row));
  }
  prompt.selected_version = flow.selected;
}

void Ask(std::shared_ptr<Flow> flow) {
  FillVersionRows(*flow);
  if (IntroScreenAvailable()) {
    ShowIntroScreen(flow->prompt,
                    [flow](GameDataChoice choice, int version) { OnChoice(flow, choice, version); });
    return;
  }
  flow->context->CallInUIThreadDeferred(
      [flow]() { OnChoice(flow, MessageBoxPrompt(flow->prompt), flow->selected); });
}

}  // namespace

fs::path StartScreenFlagPath(const fs::path& user_settings_path) {
  return user_settings_path.parent_path() / "start_screen.flag";
}

void RequestStartScreenOnNextLaunch(const fs::path& flag, bool request) {
  std::error_code ec;
  if (request) {
    std::ofstream{flag};
  } else {
    fs::remove(flag, ec);
  }
}

bool UsePreparedGameData(const GameDataOptions& options) {
  g_root = LibraryRoot(options);
  REXLOG_INFO("Game library: {}", g_root.string());
  MigrateLibrary(g_root);
  g_versions = ScanLibrary(g_root);
  FindVersion(g_versions, GameDirectoryOverride());
  RefreshRootExecutable(g_root, g_versions);
  g_asked = ConsumeStartScreenRequest(options.start_screen_flag) || REXCVAR_GET(show_start_screen);
  for (const GameVersion& version : g_versions)
    REXLOG_INFO("Game version {} at {}", version.key, version.dir.string());
  GameDirectories dirs;
  if (!Choose(g_versions, dirs) || g_asked || NeedsPhases(g_versions))
    return false;
  g_started = dirs;
  return true;
}

const GameDirectories& StartedGameDirectories() {
  return g_started;
}

void AskForGameData(const GameDataOptions& options, rex::ui::WindowedAppContext& context,
                    std::function<void(bool ready)> done) {
  auto flow = std::make_shared<Flow>();
  flow->options = options;
  flow->context = &context;
  flow->done = std::move(done);
  flow->prompt.copy_hint = CopyHint();
  flow->versions = std::move(g_versions);
  flow->selected = flow->versions.empty() ? -1 : 0;
  if (!NeedsPhases(flow->versions)) {
    Ask(flow);
    return;
  }
  // A version is behind this build: the intro shows its phases running, then
  // starts the library unless the screen was asked for.
  if (IntroScreenAvailable()) {
    FillVersionRows(*flow);
    ShowIntroScreen(flow->prompt,
                    [flow](GameDataChoice choice, int version) { OnChoice(flow, choice, version); });
    SetIntroBusy(true);
  }
  const bool start = !g_asked;
  context.CallInUIThreadDeferred([flow, start]() { RunPhases(flow, start); });
}

bool IsExtractingGameData() {
  return g_extracting;
}

}  // namespace nocturne
