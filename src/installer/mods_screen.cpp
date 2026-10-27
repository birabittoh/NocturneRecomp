#include "mods_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <functional>
#include <utility>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/system/mod_version.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "document_picker.h"

#include "intro_music.h"
#include "settings.h"
#include "target.h"
#include "intro_text.h"
#include "version.generated.h"

namespace nocturne {
namespace {

constexpr ImVec4 kAccent(0xD4 / 255.0f, 0xA8 / 255.0f, 0x4A / 255.0f, 1.0f);
constexpr ImVec4 kText(0.96f, 0.93f, 0.88f, 1.0f);
constexpr ImVec4 kMuted(0.60f, 0.62f, 0.66f, 1.0f);
constexpr ImVec4 kHeader(0.83f, 0.66f, 0.29f, 1.0f);
constexpr ImVec4 kCode(1.00f, 0.82f, 0.30f, 1.0f);
constexpr ImVec4 kError(1.00f, 0.35f, 0.35f, 1.0f);
constexpr ImVec4 kWarn(1.00f, 0.75f, 0.30f, 1.0f);
constexpr ImVec4 kQuitRed(0.90f, 0.22f, 0.22f, 1.0f);
constexpr ImVec4 kGood(0.45f, 0.85f, 0.55f, 1.0f);
constexpr float kIconSize = 40.0f;

// The overlay's own strings, already translated in res/lang's [sdk_ui].
const char* T(const char* key, const char* fallback) {
  const char* text = SdkUiText(key);
  return text ? text : fallback;
}

std::string Fmt(const char* key, const char* fallback, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fallback);
  std::vsnprintf(buffer, sizeof(buffer), T(key, fallback), args);
  va_end(args);
  return buffer;
}

std::string Join(const std::vector<std::string>& names) {
  std::string joined;
  for (size_t i = 0; i < names.size(); ++i)
    joined += (i ? ", " : "") + names[i];
  return joined;
}

std::string JoinRequirements(const std::vector<rex::system::ModRequirement>& reqs) {
  std::string joined;
  for (size_t i = 0; i < reqs.size(); ++i) {
    joined += (i ? ", " : "") + reqs[i].name;
    if (!reqs[i].min_version.empty())
      joined += " >= " + reqs[i].min_version;
  }
  return joined;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return {};
  const std::streamsize length = file.tellg();
  if (length <= 0)
    return {};
  file.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  if (!file.read(reinterpret_cast<char*>(bytes.data()), length))
    return {};
  return bytes;
}

std::unique_ptr<rex::ui::ImmediateTexture> Upload(rex::ui::ImmediateDrawer* drawer,
                                                  const std::vector<uint8_t>& bytes) {
  if (!drawer || bytes.empty())
    return nullptr;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
  if (rgba.empty() || w <= 0 || h <= 0)
    return nullptr;
  return drawer->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear,
                               false, rgba.data());
}

}  // namespace

ModsScreen::ModsScreen(rex::ui::ImmediateDrawer* drawer, rex::ui::WindowedAppContext* context)
    : drawer_(drawer), context_(context) {}

ModsScreen::~ModsScreen() {
  *alive_ = false;

  // The frame that last drew them may still be in flight, so they are never freed.
  static std::vector<std::unique_ptr<rex::ui::ImmediateTexture>> graveyard;
  for (auto& [url, texture] : icons_) {
    if (texture)
      graveyard.push_back(std::move(texture));
  }
  cancel_.Cancel();
  for (auto& [url, thread] : icon_threads_) {
    if (thread.joinable())
      thread.join();
  }
}

std::string ModsScreen::BlockingIssue() {
  // The runtime refuses to start only on a version mismatch; other problems are logged.
  using rex::system::CompareVersions;
  using rex::system::ParseVersionComponents;
  const auto root = rex::system::ModState::ResolveModsRoot();
  const auto entries = rex::system::ModState::LoadReconciled(root);
  std::unordered_map<std::string, rex::system::ModInfo> infos;
  std::vector<std::string> enabled;
  for (const auto& entry : entries) {
    if (!entry.enabled)
      continue;
    enabled.push_back(entry.id);
    infos.emplace(entry.id, rex::system::ParseModInfo(root / entry.id));
  }
  std::vector<int> have, want;
  for (const auto& id : enabled) {
    const auto& info = infos.at(id);
    if (!info.min_game_version.empty() && ParseVersionComponents(info.min_game_version, want) &&
        ParseVersionComponents(kVersionString, have) && CompareVersions(have, want) < 0)
      return id + ": " + Fmt("incompatible_version", "Requires game version >= %s (running %s)",
                             info.min_game_version.c_str(), kVersionString);
    for (const auto& req : info.requires_mods) {
      const auto dep = infos.find(req.name);
      if (req.name == id || dep == infos.end() || req.min_version.empty())
        continue;
      if (ParseVersionComponents(req.min_version, want) &&
          ParseVersionComponents(dep->second.version, have) && CompareVersions(have, want) < 0)
        return id + " requires " + req.name + " >= " + req.min_version + " (found " +
               dep->second.version + ")";
    }
  }
  return {};
}

void ModsScreen::Reload() {
  mods_root_ = rex::system::ModState::ResolveModsRoot();
  entries_ = rex::system::ModState::LoadReconciled(mods_root_);
  manifests_.clear();
  other_target_.clear();
  const auto game_targets = GameTargets();
  for (const auto& entry : entries_) {
    auto info = rex::system::ParseModInfo(mods_root_ / entry.id);
    if (!rex::system::ModTargetsMatch(info.targets, game_targets))
      other_target_.emplace(entry.id, Join(info.targets));
    manifests_.emplace(entry.id, std::move(info));
  }
  pending_removals_ = rex::system::ModState::PendingRemovals(mods_root_);
  loaded_ = true;
  issues_ = rex::system::ModState::Validate(entries_, manifests_, kVersionString,
                                            rex::system::ModState::HostPlatformId());
}

void ModsScreen::Commit() {
  if (mods_root_.empty())
    return;
  rex::system::ModState::ApplyPendingRemovals(mods_root_);
  if (!installer_.InstallSnapshot().in_progress)
    rex::system::ModState::ApplyPendingUpdates(mods_root_);
  loaded_ = false;
}

void ModsScreen::Persist() {
  rex::system::ModState::Save(mods_root_, entries_);
  issues_ = rex::system::ModState::Validate(entries_, manifests_, kVersionString,
                                            rex::system::ModState::HostPlatformId());
}

void ModsScreen::StartCatalogFetch() {
  installer_.Refresh("nocturnerecomp");
}

ModsScreen::CatalogLoad ModsScreen::CatalogStatus() const {
  switch (installer_.state()) {
    case rex::system::CatalogState::kLoading:
      return CatalogLoad::kLoading;
    case rex::system::CatalogState::kReady:
      return CatalogLoad::kReady;
    case rex::system::CatalogState::kFailed:
      return CatalogLoad::kFailed;
    default:
      return CatalogLoad::kIdle;
  }
}

rex::ui::ImmediateTexture* ModsScreen::LocalIcon(const rex::system::ModInfo& mod) {
  if (mod.icon_path.empty())
    return nullptr;
  const std::string key = mod.icon_path.string();
  auto it = icons_.find(key);
  if (it == icons_.end())
    it = icons_.emplace(key, Upload(drawer_, ReadFile(mod.icon_path))).first;
  return it->second.get();
}

rex::ui::ImmediateTexture* ModsScreen::RemoteIcon(const std::string& url) {
  if (url.empty())
    return nullptr;
  if (auto it = icons_.find(url); it != icons_.end())
    return it->second.get();
  std::vector<uint8_t> bytes;
  {
    std::lock_guard<std::mutex> lock(icon_mutex_);
    if (auto it = icon_bytes_.find(url); it != icon_bytes_.end()) {
      bytes = std::move(it->second);
      icon_bytes_.erase(it);
    } else {
      if (!icon_threads_.contains(url)) {
        icon_threads_.emplace(url, std::thread([this, url] {
                                rex::net::HttpResponse response;
                                for (int attempt = 0; attempt < 3; ++attempt) {
                                  response = rex::net::HttpGet(url, {}, &cancel_);
                                  if (response.ok() || cancel_.cancelled())
                                    break;
                                  REXLOG_WARN("Mods: icon {} failed (status {}): {}", url,
                                              response.status, response.error);
                                }
                                if (!response.ok())
                                  return;
                                std::lock_guard<std::mutex> lock2(icon_mutex_);
                                icon_bytes_[url] = std::vector<uint8_t>(response.body.begin(),
                                                                        response.body.end());
                              }));
      }
      return nullptr;
    }
  }
  auto texture = Upload(drawer_, bytes);
  if (!texture)
    REXLOG_WARN("Mods: icon {} did not decode ({} bytes)", url, bytes.size());
  auto* raw = texture.get();
  icons_.emplace(url, std::move(texture));
  return raw;
}

namespace {

ImU32 Col(const ImVec4& c, float a = 1.0f) {
  return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a));
}

struct Seg {
  std::string text;
  ImVec4 color;
};

// Segments on one line; wider than `avail` they slide to show the rest,
// pausing at both ends (as the main page's long labels do).
void Rich(ImDrawList* draw, ImFont* font, float size, float x, float y, float avail, float unit,
          float t, float a, const std::vector<Seg>& segs) {
  float width = 0.0f;
  for (const Seg& seg : segs)
    width += font->CalcTextSizeA(size, FLT_MAX, 0.0f, seg.text.c_str()).x;
  float offset = 0.0f;
  if (width > avail && t >= 0.0f) {
    constexpr float kPause = 1.0f;
    const float travel = (width - avail) / (50.0f * unit);
    const float phase = std::fmod(t, 2.0f * (travel + kPause));
    float along = 0.0f;
    if (phase < kPause)
      along = 0.0f;
    else if (phase < kPause + travel)
      along = (phase - kPause) / travel;
    else if (phase < 2.0f * kPause + travel)
      along = 1.0f;
    else
      along = 1.0f - (phase - 2.0f * kPause - travel) / travel;
    offset = (width - avail) * along;
  }
  float at = x - offset;
  for (const Seg& seg : segs) {
    draw->AddText(font, size, ImVec2(std::floor(at), y), Col(seg.color, a), seg.text.c_str());
    at += font->CalcTextSizeA(size, FLT_MAX, 0.0f, seg.text.c_str()).x;
  }
}

bool Pressed(std::initializer_list<ImGuiKey> keys) {
  // Alt+Enter toggles fullscreen.
  if (ImGui::GetIO().KeyAlt)
    return false;
  return std::any_of(keys.begin(), keys.end(), [](ImGuiKey k) { return ImGui::IsKeyPressed(k); });
}

}  // namespace

int ModsScreen::TabCount() const {
  return 2;
}

void ModsScreen::BuildLists() {
  available_.clear();
  enabled_.clear();
  for (size_t i = 0; i < entries_.size(); ++i)
    (entries_[i].enabled ? enabled_ : available_).push_back(i);
  view_.clear();
  if (CatalogStatus() == CatalogLoad::kReady)
    view_ = installer_.Snapshot();
  if (tab_ >= TabCount())
    tab_ = 0;
}

const rex::system::ModInfo& ModsScreen::Info(const std::string& id) const {
  static const rex::system::ModInfo kEmpty;
  const auto it = manifests_.find(id);
  return it != manifests_.end() ? it->second : kEmpty;
}

const rex::system::CatalogMod* ModsScreen::Listed(const std::string& id) const {
  for (const auto& mod : view_) {
    if (mod.mod_id == id)
      return &mod;
  }
  return nullptr;
}

bool ModsScreen::UpdateAvailable(const std::string& id) const {
  const auto* listed = Listed(id);
  const std::string& version = Info(id).version;
  return listed && !version.empty() &&
         rex::system::CompareVersionStrings(listed->version, version) > 0;
}

std::string ModsScreen::Incompatible(const rex::system::CatalogMod& mod) const {
  if (!mod.game_version.empty() &&
      rex::system::CompareVersionStrings(kVersionString, mod.game_version) < 0)
    return Fmt("incompatible_version", "Requires game version >= %s (running %s)",
               mod.game_version.c_str(), kVersionString);
  const std::string platform = rex::system::ModState::HostPlatformId();
  if (!mod.platforms.empty() &&
      std::find(mod.platforms.begin(), mod.platforms.end(), platform) == mod.platforms.end())
    return Fmt("incompatible_platform", "No binary for this platform (ships: %s)",
               Join(mod.platforms).c_str());
  return {};
}

void ModsScreen::BuildTools() {
  tools_.clear();
  if (tab_ == 1) {
    tools_.push_back({ToolId::kRefreshCatalog, T("refresh", "Refresh"),
                      CatalogStatus() != CatalogLoad::kLoading});
    return;
  }
  const bool has = !current_.empty() &&
                   std::any_of(entries_.begin(), entries_.end(),
                               [&](const auto& e) { return e.id == current_; });
  tools_.push_back({ToolId::kAutoSort, T("auto_sort", "Auto-sort"), true});
  tools_.push_back({ToolId::kRefreshDisk, T("refresh_disk", "Refresh from disk"), true});
#if !REX_PLATFORM_ANDROID
  // Android has no file manager to hand the folder to.
  tools_.push_back({ToolId::kOpenFolder, T("open_folder", "Open Mods Folder"), true});
#endif
  if (context_) {
    tools_.push_back({ToolId::kInstallFile, Tr("mods_page", "install_file"), !picking_});
  }
  tools_.push_back({ToolId::kUpdate, T("update", "Update"), has && UpdateAvailable(current_)});
}

int ModsScreen::Count(Zone zone) const {
  switch (zone) {
    case Zone::kAvailable:
      return int(available_.size());
    case Zone::kEnabled:
      return int(enabled_.size());
    case Zone::kCatalog:
      return int(view_.size());
    default:
      return 0;
  }
}

bool ModsScreen::Valid(const Focus& focus) const {
  switch (focus.zone) {
    case Zone::kBack:
    case Zone::kMusic:
    case Zone::kPicker:
      return true;
    case Zone::kTab:
      return focus.index >= 0 && focus.index < TabCount();
    case Zone::kTool:
      return focus.index >= 0 && focus.index < int(tools_.size()) &&
             tools_[size_t(focus.index)].enabled;
    case Zone::kAvailable:
    case Zone::kEnabled:
      return tab_ == 0 && focus.index >= 0 && focus.index < Count(focus.zone);
    case Zone::kCatalog:
      return tab_ == 1 && focus.index >= 0 && focus.index < Count(focus.zone);
  }
  return false;
}

void ModsScreen::Settle() {
  if (Valid(focus_))
    return;
  for (const Focus& candidate :
       {Focus{Zone::kEnabled, 0}, Focus{Zone::kAvailable, 0}, Focus{Zone::kCatalog, 0},
        Focus{Zone::kTool, 0}, Focus{Zone::kTab, tab_}}) {
    if (Valid(candidate)) {
      focus_ = candidate;
      return;
    }
  }
  focus_ = {Zone::kBack, 0};
}

const std::string& ModsScreen::IdAt(const Focus& focus) const {
  static const std::string kNone;
  if (!Valid(focus))
    return kNone;
  if (focus.zone == Zone::kAvailable)
    return entries_[available_[size_t(focus.index)]].id;
  if (focus.zone == Zone::kEnabled)
    return entries_[enabled_[size_t(focus.index)]].id;
  if (focus.zone == Zone::kCatalog)
    return view_[size_t(focus.index)].mod_id;
  return kNone;
}

void ModsScreen::FocusMod(const std::string& id) {
  BuildLists();
  for (size_t i = 0; i < enabled_.size(); ++i) {
    if (entries_[enabled_[i]].id == id) {
      focus_ = {Zone::kEnabled, int(i)};
      current_ = id;
      return;
    }
  }
  for (size_t i = 0; i < available_.size(); ++i) {
    if (entries_[available_[i]].id == id) {
      focus_ = {Zone::kAvailable, int(i)};
      current_ = id;
      return;
    }
  }
}

// Up from the bottom row: back to where the cursor was, else the first thing there is.
void ModsScreen::LeaveBottom() {
  int tool = -1;
  for (size_t i = 0; i < tools_.size() && tool < 0; ++i) {
    if (tools_[i].enabled)
      tool = int(i);
  }
  for (const Focus& candidate :
       {before_music_, Focus{Zone::kEnabled, 0}, Focus{Zone::kAvailable, 0},
        Focus{Zone::kCatalog, 0}, Focus{Zone::kTool, tool}}) {
    if (Valid(candidate) && candidate.zone != Zone::kMusic && candidate.zone != Zone::kPicker) {
      focus_ = candidate;
      return;
    }
  }
  focus_ = {Zone::kTab, tab_};
}

void ModsScreen::Move(int dx, int dy) {
  Focus& f = focus_;
  auto first_tool = [&] {
    for (size_t i = 0; i < tools_.size(); ++i) {
      if (tools_[i].enabled)
        return int(i);
    }
    return -1;
  };
  switch (f.zone) {
    case Zone::kMusic:
      if (dy < 0)
        LeaveBottom();
      else if (dx > 0 && IntroLanguageCount() >= 2)
        f = {Zone::kPicker, 0};
      return;
    case Zone::kPicker:
      return;
    case Zone::kBack:
      if (dy > 0)
        f = {Zone::kTab, tab_};
      return;
    case Zone::kTab:
      if (dx)
        f.index = std::clamp(f.index + dx, 0, TabCount() - 1);
      else if (dy < 0)
        f = {Zone::kBack, 0};
      else if (dy > 0 && first_tool() >= 0)
        f = {Zone::kTool, first_tool()};
      return;
    case Zone::kTool: {
      if (dx) {
        for (int i = f.index + dx; i >= 0 && i < int(tools_.size()); i += dx) {
          if (tools_[size_t(i)].enabled) {
            f.index = i;
            break;
          }
        }
      } else if (dy < 0) {
        f = {Zone::kTab, tab_};
      } else if (dy > 0) {
        // The left tools sit over Available, the rest over Enabled.
        const Zone near_zone = f.index < 2 ? Zone::kAvailable : Zone::kEnabled;
        const Zone far_zone = near_zone == Zone::kAvailable ? Zone::kEnabled : Zone::kAvailable;
        for (const Focus& candidate :
             {Focus{Zone::kCatalog, 0}, Focus{near_zone, 0}, Focus{far_zone, 0}}) {
          if (Valid(candidate)) {
            f = candidate;
            return;
          }
        }
        // No music toggle until a track ships; the language picker sits there.
        if (IntroMusicAvailable())
          f = {Zone::kMusic, 0};
        else if (IntroLanguageCount() >= 2)
          f = {Zone::kPicker, 0};
      }
      return;
    }
    case Zone::kAvailable:
    case Zone::kEnabled:
    case Zone::kCatalog: {
      if (!grabbed_.empty() && f.zone == Zone::kEnabled) {
        if (dy)
          MoveEnabled(grabbed_, dy);
        return;
      }
      if (trash_) {
        // Left returns to the row; right goes on to the Enabled pane.
        if (dx < 0) {
          trash_ = false;
          return;
        }
        if (dx > 0) {
          if (f.zone == Zone::kAvailable && Count(Zone::kEnabled) > 0) {
            trash_ = false;
            f = {Zone::kEnabled, std::min(f.index, Count(Zone::kEnabled) - 1)};
          }
          return;
        }
        trash_ = false;
      }
      if (dy < 0) {
        if (f.index > 0)
          --f.index;
        else if (first_tool() >= 0)
          f = {Zone::kTool, first_tool()};
      } else if (dy > 0) {
        if (f.index + 1 < Count(f.zone)) {
          ++f.index;
        } else {
          before_music_ = f;
          // No music toggle until a track ships; the language picker sits there.
        if (IntroMusicAvailable())
          f = {Zone::kMusic, 0};
        else if (IntroLanguageCount() >= 2)
          f = {Zone::kPicker, 0};
        }
      } else if (dx > 0 && f.zone != Zone::kCatalog) {
        trash_ = true;
      } else if (dx < 0 && f.zone == Zone::kEnabled && Count(Zone::kAvailable) > 0) {
        f = {Zone::kAvailable, std::min(f.index, Count(Zone::kAvailable) - 1)};
      }
      return;
    }
  }
}

void ModsScreen::ToggleMod(const std::string& id) {
  if (pending_removals_.contains(id)) {
    ToggleRemoval(id);
    return;
  }
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const auto& e) { return e.id == id; });
  if (it == entries_.end())
    return;
  std::unordered_set<std::string> visited;
  if (!it->enabled) {
    // Enabling pulls in what the mod requires.
    std::function<void(const std::string&)> enable = [&](const std::string& mod_id) {
      if (!visited.insert(mod_id).second)
        return;
      const auto e = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const auto& x) { return x.id == mod_id; });
      if (e == entries_.end())
        return;
      e->enabled = true;
      for (const auto& req : Info(mod_id).requires_mods)
        enable(req.name);
    };
    enable(id);
  } else {
    // Disabling takes the mods that require it along.
    std::function<void(const std::string&)> disable = [&](const std::string& mod_id) {
      if (!visited.insert(mod_id).second)
        return;
      const auto e = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const auto& x) { return x.id == mod_id; });
      if (e == entries_.end())
        return;
      e->enabled = false;
      for (auto& other : entries_) {
        if (!other.enabled)
          continue;
        for (const auto& req : Info(other.id).requires_mods) {
          if (req.name == mod_id) {
            disable(other.id);
            break;
          }
        }
      }
    };
    disable(id);
  }
  Persist();
  FocusMod(id);
}

// Only marks the folder so it can be restored; Commit() deletes it.
void ModsScreen::ToggleRemoval(const std::string& id) {
  if (pending_removals_.contains(id))
    rex::system::ModState::UnmarkPendingRemoval(mods_root_, id);
  else
    rex::system::ModState::MarkPendingRemoval(mods_root_, id);
  Reload();
  FocusMod(id);
}

void ModsScreen::MoveEnabled(const std::string& id, int delta) {
  const auto rank = std::find_if(enabled_.begin(), enabled_.end(),
                                 [&](size_t i) { return entries_[i].id == id; });
  if (rank == enabled_.end())
    return;
  const auto other = rank + delta;
  if (other < enabled_.begin() || other >= enabled_.end())
    return;
  std::swap(entries_[*rank], entries_[*other]);
  Persist();
  FocusMod(id);
}

void ModsScreen::ActivateTool(int index) {
  if (index < 0 || index >= int(tools_.size()))
    return;
  switch (tools_[size_t(index)].id) {
    case ToolId::kRefreshCatalog:
      StartCatalogFetch();
      return;
    case ToolId::kAutoSort:
      entries_ = rex::system::ModState::AutoSort(entries_, manifests_);
      Persist();
      break;
    case ToolId::kRefreshDisk:
      Reload();
      break;
    case ToolId::kOpenFolder: {
      std::error_code ignored;
      std::filesystem::create_directories(mods_root_, ignored);
      rex::platform::process::OpenFolder(mods_root_);
      break;
    }
    case ToolId::kInstallFile:
      InstallFromFile();
      return;
    case ToolId::kUpdate:
      if (const auto* listed = Listed(current_))
        installer_.InstallAsync(*listed, mods_root_);
      break;
  }
  BuildLists();
}

void ModsScreen::InstallFromFile() {
  if (!context_ || picking_)
    return;
  picking_ = true;
  status_.clear();
  std::weak_ptr<bool> alive = alive_;
  PickFileToRead(*context_, [this, alive](std::optional<std::filesystem::path> picked) {
    if (alive.expired())
      return;
    picking_ = false;
    if (!picked)
      return;
    std::string error;
    const auto result = rex::system::ModState::InstallLocalArchive(mods_root_, *picked, error);
    status_error_ = !result;
    if (!result)
      status_ = Tr("mods_page", "install_failed", error);
    else
      status_ = Tr("mods_page", result->staged ? "updated" : "installed", result->id);
    Reload();
    if (result)
      FocusMod(result->id);
  });
}

void ModsScreen::Press() {
  switch (focus_.zone) {
    case Zone::kBack:
      back_ = true;
      break;
    case Zone::kMusic:
      SetIntroMusicEnabled(!IntroMusicEnabled());
      break;
    case Zone::kPicker:
      break;
    case Zone::kTab:
      tab_ = focus_.index;
      break;
    case Zone::kTool:
      ActivateTool(focus_.index);
      break;
    case Zone::kAvailable:
    case Zone::kEnabled: {
      if (!grabbed_.empty()) {
        grabbed_.clear();
        break;
      }
      const std::string id = IdAt(focus_);
      if (id.empty())
        break;
      if (trash_)
        ToggleRemoval(id);
      else
        ToggleMod(id);
      break;
    }
    case Zone::kCatalog: {
      const auto& mod = view_[size_t(focus_.index)];
      const bool installed = manifests_.contains(mod.mod_id);
      if (Incompatible(mod).empty() && (!installed || UpdateAvailable(mod.mod_id)))
        installer_.InstallAsync(mod, mods_root_);
      break;
    }
  }
}

void ModsScreen::Input() {
  // The intro's picker has the keys while the cursor is on it.
  if (!ready_ || focus_.zone == Zone::kPicker)
    return;
  for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; ++k) {
    if (ImGui::IsKeyPressed(ImGuiKey(k), false))
      gamepad_ = true;
  }
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
    if (ImGui::IsKeyPressed(ImGuiKey(k), false))
      gamepad_ = false;
  }
  if (moved_ || ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    gamepad_ = false;

  if (Pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown})) {
    Move(0, 1);
  } else if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp})) {
    Move(0, -1);
  } else if (Pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight})) {
    Move(1, 0);
  } else if (Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft})) {
    Move(-1, 0);
  } else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                      ImGuiKey_GamepadFaceDown})) {
    press_ = true;
  } else if (Pressed({ImGuiKey_Delete, ImGuiKey_GamepadFaceLeft})) {
    remove_ = true;
  } else if (Pressed({ImGuiKey_G, ImGuiKey_GamepadFaceUp})) {
    // Reorder: up and down move the mod until the next press.
    if (!grabbed_.empty())
      grabbed_.clear();
    else if (focus_.zone == Zone::kEnabled)
      grabbed_ = IdAt(focus_);
  } else if (Pressed({ImGuiKey_Q, ImGuiKey_GamepadL1})) {
    tab_ = std::max(0, tab_ - 1);
  } else if (Pressed({ImGuiKey_E, ImGuiKey_GamepadR1})) {
    tab_ = std::min(TabCount() - 1, tab_ + 1);
  } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
    if (!grabbed_.empty())
      grabbed_.clear();
    else
      back_ = true;
  }
}

void ModsScreen::TouchBegin() {
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    touch_ = io.MouseSource == ImGuiMouseSource_TouchScreen;
  else if (moved_ && io.MouseSource != ImGuiMouseSource_TouchScreen)
    touch_ = false;
  for (int k = ImGuiKey_NamedKey_BEGIN; k <= ImGuiKey_GamepadRStickDown; ++k) {
    if (ImGui::IsKeyPressed(ImGuiKey(k), false))
      touch_ = false;
  }
  // The rows drawn this frame claim the new finger.
  if (touch_ && ready_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    gesture_ = {};
    gesture_.down = true;
    gesture_.start = io.MousePos;
    gesture_.last_y = io.MousePos.y;
    gesture_.since = Clock::now();
    std::fill(std::begin(fling_), std::end(fling_), 0.0f);
  }
}

void ModsScreen::TouchTarget(Focus self, Part part) {
  // The buttons beside a row win over the row under them.
  if (!gesture_.down || (gesture_.has_target && part == Part::kBody))
    return;
  gesture_.has_target = true;
  gesture_.target = self;
  gesture_.part = part;
}

void ModsScreen::TouchUpdate() {
  Gesture& g = gesture_;
  if (!g.down)
    return;
  if (!ready_) {
    if (g.held)
      grabbed_.clear();
    g = {};
    return;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const bool row = g.has_target && g.part == Part::kBody &&
                   (g.target.zone == Zone::kAvailable || g.target.zone == Zone::kEnabled ||
                    g.target.zone == Zone::kCatalog);
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 pos = io.MousePos;
    const float dt = std::max(io.DeltaTime, 1e-4f);
    const float dx = pos.x - g.start.x;
    const float dy = pos.y - g.start.y;
    const float slop = 12.0f * unit_;
    if (!g.dragging && !g.held) {
      if (dx * dx + dy * dy > slop * slop) {
        g.dragging = true;
      } else if (Clock::now() - g.since >= std::chrono::milliseconds(450)) {
        // A hold previews the mod, and on Enabled picks it up.
        g.held = true;
        if (g.has_target && g.part == Part::kBody && row && Valid(g.target)) {
          focus_ = g.target;
          trash_ = false;
          if (g.target.zone == Zone::kEnabled)
            grabbed_ = IdAt(g.target);
        }
      }
    }
    if (g.dragging && g.list >= 0) {
      scroll_[g.list] = g.scroll_start - dy;
      g.velocity += ((g.last_y - pos.y) / dt - g.velocity) * 0.5f;
    }
    if (g.held && !grabbed_.empty()) {
      // The mod follows the finger, and the list scrolls near its ends.
      const ListGeometry& geo = geometry_[1];
      const float edge = geo.pitch * 0.6f;
      if (pos.y < geo.top + edge)
        scroll_[1] -= 600.0f * unit_ * dt;
      else if (pos.y > geo.bottom - edge)
        scroll_[1] += 600.0f * unit_ * dt;
      const int want = std::clamp(int(std::floor((pos.y - geo.top + scroll_[1]) / geo.pitch)), 0,
                                  Count(Zone::kEnabled) - 1);
      const std::string id = grabbed_;
      for (int guard = 0; guard < Count(Zone::kEnabled); ++guard) {
        const auto at = std::find_if(enabled_.begin(), enabled_.end(),
                                     [&](size_t i) { return entries_[i].id == id; });
        const int rank = int(at - enabled_.begin());
        if (at == enabled_.end() || rank == want)
          break;
        MoveEnabled(id, rank < want ? 1 : -1);
      }
    }
    g.last_y = pos.y;
    return;
  }

  if (g.held && row) {
    grabbed_.clear();
  } else if (g.dragging) {
    if (g.list >= 0)
      fling_[g.list] = g.velocity;
  } else if (g.has_target && Valid(g.target)) {
    // A tap on a row only selects it; acting is left to the buttons beside it.
    focus_ = g.target;
    trash_ = false;
    if (g.part == Part::kTrash)
      remove_ = true;
    else if (g.part == Part::kAction || !row)
      press_ = true;
  }
  g = {};
}

bool ModsScreen::Hit(ImVec2 min, ImVec2 max, Focus self, bool usable) {
  ImGui::SetCursorScreenPos(min);
  ImGui::PushID(int(self.zone) * 10000 + self.index);
  ImGui::InvisibleButton("##hit", ImVec2(max.x - min.x, max.y - min.y),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  const bool left = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  const bool right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  ImGui::PopID();
  if (!ready_ || !usable)
    return false;
  if (touch_) {
    if (left)
      TouchTarget(self, Part::kBody);
    return hovered;
  }
  // Only a moving mouse selects, so a resting cursor does not undo the keys.
  if (hovered && moved_ && grabbed_.empty()) {
    if (!(focus_ == self))
      trash_ = false;
    focus_ = self;
  }
  if (left || right) {
    focus_ = self;
    (left ? press_ : remove_) = true;
  }
  return hovered;
}

void ModsScreen::Plate(ImVec2 min, ImVec2 max, bool focused, bool picked, float a) {
  const float rounding = 6.0f * unit_;
  draw_->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((focused ? 150 : 100) * a)), rounding);
  if (picked)
    draw_->AddRectFilled(min, max, Col(kAccent, 0.25f * a), rounding);
  draw_->AddRect(min, max, focused ? Col(kAccent, a) : IM_COL32(255, 255, 255, int(60 * a)),
                 rounding, 0, std::max(1.0f, (focused ? 2.0f : 1.0f) * unit_));
}

void ModsScreen::Text(ImVec2 min, ImVec2 max, const char* text, float size, float a, bool focused) {
  const ImVec2 extent = font_->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  draw_->AddText(font_, size,
                 ImVec2(std::floor(min.x + (max.x - min.x - extent.x) * 0.5f),
                        std::floor(min.y + (max.y - min.y - extent.y) * 0.5f)),
                 Col(kText, a * (focused ? 1.0f : 0.8f)), text);
}

template <class RowFn>
void ModsScreen::List(Zone zone, int count, ImVec2 min, ImVec2 max, float row_height,
                      RowFn&& row) {
  const float gap = 8.0f * unit_;
  const float view = max.y - min.y;
  const float content = float(count) * (row_height + gap) - gap;
  const int slot = zone == Zone::kAvailable ? 0 : zone == Zone::kEnabled ? 1 : 2;
  float& scroll = scroll_[slot];

  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool inside =
      mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y;
  if (ready_ && inside)
    scroll -= ImGui::GetIO().MouseWheel * 1.5f * (row_height + gap);
  if (focus_changed_ && focus_.zone == zone) {
    const float top = float(focus_.index) * (row_height + gap);
    if (top < scroll)
      scroll = top;
    else if (top + row_height > scroll + view)
      scroll = top + row_height - view;
  }
  // A swipe that let go keeps the list coasting.
  float& fling = fling_[slot];
  if (fling != 0.0f) {
    const float dt = ImGui::GetIO().DeltaTime;
    scroll += fling * dt;
    fling *= std::exp(-4.0f * dt);
    if (std::fabs(fling) < 20.0f * unit_)
      fling = 0.0f;
  }
  const float limit = std::max(0.0f, content - view);
  if (scroll <= 0.0f || scroll >= limit)
    fling = 0.0f;
  scroll = std::clamp(scroll, 0.0f, limit);
  geometry_[slot] = {min.y, max.y, row_height + gap};
  if (ready_ && inside && gesture_.down && gesture_.list < 0 &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    gesture_.list = slot;
    gesture_.scroll_start = scroll;
  }

  draw_->AddRectFilled(min, max, IM_COL32(0, 0, 0, int(60 * alpha_)), 6.0f * unit_);
  ImGui::PushClipRect(min, max, true);
  for (int i = 0; i < count; ++i) {
    const float y = min.y - scroll + float(i) * (row_height + gap);
    if (y + row_height < min.y || y > max.y)
      continue;
    row(i, ImVec2(min.x, y), ImVec2(max.x - 8.0f * unit_, y + row_height));
  }
  ImGui::PopClipRect();
  if (content > view) {
    const float bar_h = std::max(24.0f * unit_, view * view / content);
    const float bar_y = min.y + (view - bar_h) * (scroll / (content - view));
    draw_->AddRectFilled(ImVec2(max.x - 5.0f * unit_, bar_y),
                         ImVec2(max.x - 2.0f * unit_, bar_y + bar_h),
                         IM_COL32(255, 255, 255, int(70 * alpha_)), 2.0f * unit_);
  }
}

// A button beside a row: the trash can (Remove) or, on touch, the row's own action.
bool ModsScreen::SideButton(ImVec2 min, ImVec2 max, Focus row, Part part, bool selected,
                            const ImVec4& glow) {
  ImGui::SetCursorScreenPos(min);
  ImGui::PushID(int(row.zone) * 10000 + row.index);
  ImGui::InvisibleButton(part == Part::kTrash ? "##trash" : "##action",
                         ImVec2(max.x - min.x, max.y - min.y));
  const bool hot = ready_ && (ImGui::IsItemHovered() || selected);
  if (ready_ && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    if (touch_) {
      TouchTarget(row, part);
    } else {
      focus_ = row;
      (part == Part::kTrash ? remove_ : press_) = true;
    }
  }
  ImGui::PopID();
  const float rounding = 6.0f * unit_;
  const float a = alpha_;
  draw_->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((hot ? 150 : 100) * a)), rounding);
  if (hot)
    draw_->AddRectFilled(min, max, Col(glow, 0.3f * a), rounding);
  draw_->AddRect(min, max, hot ? Col(glow, a) : IM_COL32(255, 255, 255, int(60 * a)), rounding,
                 0, std::max(1.0f, (hot ? 2.0f : 1.0f) * unit_));
  return hot;
}

// Enables or disables the focused mod, pointing at the pane it goes to.
void ModsScreen::DrawArrow(ImVec2 min, ImVec2 max, Focus row, bool right) {
  const bool hot = SideButton(min, max, row, Part::kAction, false, kAccent);
  const ImU32 ink = Col(kText, alpha_ * (hot ? 1.0f : 0.8f));
  const float h = max.y - min.y;
  const float cx = (min.x + max.x) * 0.5f;
  const float cy = (min.y + max.y) * 0.5f;
  const float dir = right ? 1.0f : -1.0f;
  const float reach = h * 0.2f;
  draw_->AddLine(ImVec2(cx - dir * reach, cy), ImVec2(cx + dir * reach * 0.4f, cy), ink,
                 std::max(1.5f, 2.0f * unit_));
  const ImVec2 tip(cx + dir * reach, cy);
  ImVec2 top(cx + dir * reach * 0.2f, cy - h * 0.14f);
  ImVec2 bottom(cx + dir * reach * 0.2f, cy + h * 0.14f);
  if (!right)
    std::swap(top, bottom);
  draw_->AddTriangleFilled(tip, bottom, top, ink);
}

// Right of the focused mod; acts as Remove, or Restore when it is marked.
void ModsScreen::DrawTrash(ImVec2 min, ImVec2 max, Focus row, bool restore, bool selected) {
  const bool hot = SideButton(min, max, row, Part::kTrash, selected, restore ? kGood : kQuitRed);
  const float u = unit_;
  const float h = max.y - min.y;
  const ImU32 ink = Col(kText, alpha_ * (hot ? 1.0f : 0.8f));
  const float thick = std::max(1.5f, 2.0f * u);
  const float cx = (min.x + max.x) * 0.5f;
  if (restore) {
    // An open circle ending in an arrowhead.
    const float cy = (min.y + max.y) * 0.5f;
    const float r = h * 0.22f;
    const float kPi = 3.14159265f;
    const float end = 1.9f * kPi;
    draw_->PathArcTo(ImVec2(cx, cy), r, 0.35f * kPi, end);
    draw_->PathStroke(ink, 0, thick);
    const ImVec2 p(cx + r * std::cos(end), cy + r * std::sin(end));
    const ImVec2 tangent(-std::sin(end), std::cos(end));
    const ImVec2 normal(std::cos(end), std::sin(end));
    const float s = h * 0.11f;
    draw_->AddTriangleFilled(ImVec2(p.x + tangent.x * s * 1.4f, p.y + tangent.y * s * 1.4f),
                             ImVec2(p.x + normal.x * s, p.y + normal.y * s),
                             ImVec2(p.x - normal.x * s, p.y - normal.y * s), ink);
    return;
  }
  const float top_y = min.y + h * 0.24f;
  const float lid_y = top_y + h * 0.08f;
  const float body_top = lid_y + h * 0.06f;
  const float body_bottom = min.y + h * 0.78f;
  // Handle, lid, then the tapered bin.
  draw_->PathLineTo(ImVec2(cx - h * 0.07f, lid_y));
  draw_->PathLineTo(ImVec2(cx - h * 0.07f, top_y));
  draw_->PathLineTo(ImVec2(cx + h * 0.07f, top_y));
  draw_->PathLineTo(ImVec2(cx + h * 0.07f, lid_y));
  draw_->PathStroke(ink, 0, thick);
  draw_->AddLine(ImVec2(cx - h * 0.26f, lid_y), ImVec2(cx + h * 0.26f, lid_y), ink, thick);
  draw_->PathLineTo(ImVec2(cx - h * 0.19f, body_top));
  draw_->PathLineTo(ImVec2(cx - h * 0.15f, body_bottom));
  draw_->PathLineTo(ImVec2(cx + h * 0.15f, body_bottom));
  draw_->PathLineTo(ImVec2(cx + h * 0.19f, body_top));
  draw_->PathStroke(ink, 0, thick);
}

void ModsScreen::DrawModRow(Zone zone, int index, ImVec2 min, ImVec2 max) {
  const size_t at = zone == Zone::kEnabled ? enabled_[size_t(index)] : available_[size_t(index)];
  const std::string& id = entries_[at].id;
  const rex::system::ModInfo& mod = Info(id);
  const Focus self{zone, index};
  const bool focused = focus_ == self;
  const bool grabbed = grabbed_ == id;
  const bool pending = pending_removals_.contains(id);
  // The trash can takes the right end of the row under the cursor, as on the main page.
  if (focused && ready_) {
    const float side = max.y - min.y;
    DrawTrash(ImVec2(max.x - side, min.y), max, self, pending, trash_);
    max.x -= side + 8.0f * unit_;
    // A tap only selects, so a finger needs its own way to move the mod across.
    if (touch_ && !pending) {
      DrawArrow(ImVec2(max.x - side, min.y), max, self, zone == Zone::kAvailable);
      max.x -= side + 8.0f * unit_;
    }
  }
  Hit(min, max, self, true);

  const float a = alpha_ * (pending ? 0.55f : 1.0f);
  Plate(min, max, focused && !trash_, grabbed, a);
  const float u = unit_;
  const float pad = 12.0f * u;
  const float icon = kIconSize * u;
  auto* texture = LocalIcon(mod);
  if (!texture) {
    // An install without icon.png still has its catalog one.
    if (const auto* listed = Listed(id))
      texture = RemoteIcon(listed->icon_url);
  }
  if (texture) {
    const ImVec2 p(min.x + pad, std::floor(min.y + (max.y - min.y - icon) * 0.5f));
    draw_->AddImage(reinterpret_cast<ImTextureID>(texture), p, ImVec2(p.x + icon, p.y + icon),
                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, int(255 * a)));
  }
  const float text_x = min.x + pad + icon + 10.0f * u;
  const float avail = max.x - pad - text_x;
  const float line1 = 21.0f * u;
  const float line2 = 15.0f * u;
  const float y = std::floor(min.y + (max.y - min.y - line1 - line2 - 3.0f * u) * 0.5f);
  const float t = focused ? std::chrono::duration<float>(Clock::now() - focus_since_).count()
                          : -1.0f;

  std::vector<Seg> first;
  if (zone == Zone::kEnabled)
    first.push_back({"#" + std::to_string(index + 1) + " ", kHeader});
  first.push_back({mod.display_name.empty() ? id : mod.display_name, kText});
  if (!mod.version.empty())
    first.push_back({"  v" + mod.version, kMuted});

  std::vector<Seg> second{{id, kMuted}};
  if (pending)
    second.push_back({"  " + std::string(T("pending_removal", "[pending removal]")), kError});
  if (!mod.code.empty())
    second.push_back({"  " + std::string(T("code_badge", "[code]")), kCode});
  for (const auto& issue : issues_) {
    if (issue.id != id)
      continue;
    const bool is_error = issue.kind == rex::system::ModIssue::Kind::kError;
    second.push_back({"  " + std::string(is_error ? T("badge_error", "[error]")
                                                   : T("badge_warning", "[warning]")),
                      is_error ? kError : kWarn});
  }
  if (UpdateAvailable(id))
    second.push_back({"  " + Fmt("update_available", "Update available (v%s)",
                                 Listed(id)->version.c_str()),
                      kGood});
  else if (CatalogStatus() == CatalogLoad::kReady && !Listed(id))
    second.push_back({"  " + std::string(T("sideloaded_badge", "[Sideloaded]")), kMuted});

  draw_->PushClipRect(ImVec2(text_x, min.y), ImVec2(max.x - pad, max.y), true);
  Rich(draw_, font_, line1, text_x, y, avail, u, t, a, first);
  Rich(draw_, font_, line2, text_x, y + line1 + 3.0f * u, avail, u, t, a, second);
  draw_->PopClipRect();
}

void ModsScreen::DrawCatalogRow(int index, ImVec2 min, ImVec2 max) {
  const rex::system::CatalogMod& mod = view_[size_t(index)];
  const Focus self{Zone::kCatalog, index};
  const bool focused = focus_ == self;
  const float u = unit_;
  const float pad = 12.0f * u;
  const float icon = kIconSize * u;

  const bool installed = manifests_.contains(mod.mod_id);
  const bool incompatible = !Incompatible(mod).empty();
  const bool update = installed && UpdateAvailable(mod.mod_id);
  const char* status = incompatible ? T("install", "Install")
                       : update     ? T("update", "Update")
                       : installed  ? T("installed", "Installed")
                                    : T("install", "Install");
  const bool actionable = !incompatible && (!installed || update);
  const float status_size = 19.0f * u;
  const float status_w = font_->CalcTextSizeA(status_size, FLT_MAX, 0.0f, status).x;
  const float status_x = max.x - pad - status_w;
  // On touch the label is the install button, since tapping the row only selects it.
  const bool button = touch_ && focused && actionable && ready_;
  const ImVec2 button_min(status_x - 12.0f * u, min.y + 8.0f * u);
  Hit(min, ImVec2(button ? button_min.x : max.x, max.y), self, true);
  Plate(min, max, focused, false, alpha_);
  if (button)
    SideButton(button_min, ImVec2(max.x, max.y - 8.0f * u), self, Part::kAction, false, kGood);

  if (auto* texture = RemoteIcon(mod.icon_url)) {
    const ImVec2 p(min.x + pad, std::floor(min.y + (max.y - min.y - icon) * 0.5f));
    draw_->AddImage(reinterpret_cast<ImTextureID>(texture), p, ImVec2(p.x + icon, p.y + icon));
  }
  draw_->AddText(font_, status_size,
                 ImVec2(std::floor(status_x),
                        std::floor(min.y + (max.y - min.y - status_size) * 0.5f)),
                 Col(actionable ? kGood : kMuted,
                     alpha_ * (incompatible ? 0.5f : focused ? 1.0f : 0.85f)),
                 status);

  const float text_x = min.x + pad + icon + 10.0f * u;
  const float avail = status_x - 16.0f * u - text_x;
  const float line1 = 21.0f * u;
  const float line2 = 15.0f * u;
  const float y = std::floor(min.y + (max.y - min.y - line1 - line2 - 3.0f * u) * 0.5f);
  const float t = focused ? std::chrono::duration<float>(Clock::now() - focus_since_).count()
                          : -1.0f;
  std::vector<Seg> first{{mod.name.empty() ? mod.mod_id : mod.name, kText}};
  if (!mod.version.empty())
    first.push_back({"  v" + mod.version, kMuted});
  if (!mod.author.empty())
    first.push_back({"  " + Fmt("by_author", "by %s", mod.author.c_str()), kMuted});
  std::string description = mod.description;
  std::replace(description.begin(), description.end(), '\n', ' ');
  draw_->PushClipRect(ImVec2(text_x, min.y), ImVec2(status_x - 16.0f * u, max.y), true);
  Rich(draw_, font_, line1, text_x, y, avail, u, t, alpha_, first);
  Rich(draw_, font_, line2, text_x, y + line1 + 3.0f * u, avail, u, t, alpha_,
       {{description, kMuted}});
  draw_->PopClipRect();
}

void ModsScreen::DrawDetail(ImVec2 min, ImVec2 max) {
  Plate(min, max, false, false, alpha_ * 0.9f);
  const float u = unit_;
  struct Line {
    std::string text;
    ImVec4 color;
    float size;
  };
  std::vector<Line> lines;
  if (tab_ == 0 && !current_.empty() && manifests_.contains(current_)) {
    const rex::system::ModInfo& mod = Info(current_);
    std::string head = mod.display_name.empty() ? current_ : mod.display_name;
    if (!mod.version.empty())
      head += "  v" + mod.version;
    if (!mod.author.empty())
      head += "  " + Fmt("by_author", "by %s", mod.author.c_str());
    lines.push_back({head, kText, 19.0f});
    if (!mod.description.empty())
      lines.push_back({mod.description, kText, 16.0f});
    if (!mod.requires_mods.empty())
      lines.push_back({Fmt("requires", "requires: %s", JoinRequirements(mod.requires_mods).c_str()),
                       kMuted, 15.0f});
    if (!mod.min_game_version.empty() &&
        rex::system::CompareVersionStrings(kVersionString, mod.min_game_version) < 0)
      lines.push_back({Fmt("needs_game_version", "needs game version: >= %s",
                           mod.min_game_version.c_str()),
                       kMuted, 15.0f});
    if (const auto it = other_target_.find(current_); it != other_target_.end())
      lines.push_back({Fmt("other_target", "made for %s: not used on this game version",
                           it->second.c_str()),
                       kWarn, 15.0f});
    if (!mod.conflicts_mods.empty())
      lines.push_back({Fmt("conflicts", "conflicts: %s", Join(mod.conflicts_mods).c_str()), kMuted,
                       15.0f});
    for (const auto& issue : issues_) {
      if (issue.id == current_)
        lines.push_back({issue.message,
                         issue.kind == rex::system::ModIssue::Kind::kError ? kError : kWarn, 15.0f});
    }
  } else if (tab_ == 1 && focus_.zone == Zone::kCatalog && Valid(focus_)) {
    const auto& mod = view_[size_t(focus_.index)];
    std::string head = mod.name.empty() ? mod.mod_id : mod.name;
    if (!mod.version.empty())
      head += "  v" + mod.version;
    if (!mod.author.empty())
      head += "  " + Fmt("by_author", "by %s", mod.author.c_str());
    lines.push_back({head, kText, 19.0f});
    if (!mod.description.empty())
      lines.push_back({mod.description, kText, 16.0f});
    if (const std::string reason = Incompatible(mod); !reason.empty())
      lines.push_back({reason, kWarn, 15.0f});
  } else if (tab_ == 0 && entries_.empty()) {
    lines.push_back({T("no_mods", "No mods installed."), kMuted, 18.0f});
  } else if (tab_ == 1 && CatalogStatus() == CatalogLoad::kLoading) {
    lines.push_back({T("loading_catalog", "Loading catalog..."), kMuted, 18.0f});
  } else if (tab_ == 1 && CatalogStatus() == CatalogLoad::kFailed) {
    lines.push_back({T("catalog_failed", "Could not load the mod catalog. Select Refresh to retry."),
                     kWarn, 18.0f});
  }

  const float pad = 14.0f * u;
  const float wrap = max.x - min.x - 2.0f * pad;
  float y = min.y + 8.0f * u;
  draw_->PushClipRect(min, max, true);
  for (const Line& line : lines) {
    const float size = line.size * u;
    const ImVec2 extent = font_->CalcTextSizeA(size, FLT_MAX, wrap, line.text.c_str());
    draw_->AddText(font_, size, ImVec2(min.x + pad, std::floor(y)), Col(line.color, alpha_),
                   line.text.c_str(), nullptr, wrap);
    y += extent.y + 3.0f * u;
  }
  draw_->PopClipRect();
}

void ModsScreen::DrawHints() {
  // Button names for a gamepad, key names otherwise.
  const char* confirm = gamepad_ ? "A  " : "Enter  ";
  const char* back = gamepad_ ? "B  " : "Esc  ";
  const char* remove = gamepad_ ? "X  " : "Del  ";
  const char* grab = gamepad_ ? "Y  " : "G  ";
  const char* tabs = gamepad_ ? "LB RB  " : "Q E  ";
  const bool on_mod = tab_ == 0 && (focus_.zone == Zone::kAvailable || focus_.zone == Zone::kEnabled);
  std::string text;
  if (touch_ && !gamepad_) {
    text = std::string(TrOr("intro", "hint_tap", "Tap")) + "  " + Tr(IntroText::kHintSelect);
    if (tab_ == 0)
      text += std::string("      ") + TrOr("intro", "hint_hold_drag", "Hold and drag") + "  " +
              TrOr("intro", "hint_reorder", "Reorder");
    text += std::string("      ") + TrOr("intro", "hint_swipe", "Swipe") + "  " +
            TrOr("intro", "hint_scroll", "Scroll");
  } else if (!grabbed_.empty()) {
    text = std::string(confirm) + TrOr("intro", "hint_done", "Done") + "      " +
           (gamepad_ ? "D-pad  " : "Up Down  ") + TrOr("intro", "hint_reorder", "Reorder");
  } else {
    text = std::string(confirm) + Tr(IntroText::kHintSelect) + "      " + back +
           Tr(IntroText::kHintBack);
    if (on_mod)
      text += std::string("      ") + remove + T("remove", "Remove");
    if (on_mod && focus_.zone == Zone::kEnabled)
      text += std::string("      ") + grab + TrOr("intro", "hint_reorder", "Reorder");
    if (TabCount() > 1)
      text += std::string("      ") + tabs + TrOr("intro", "hint_tabs", "Tabs");
  }
  const float size = 17.0f * unit_;
  const ImVec2 extent = font_->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  draw_->AddText(font_, size, ImVec2(std::floor((size_.x - extent.x) * 0.5f), hint_y_),
                 Col(kText, 0.7f * alpha_), text.c_str());
}

bool ModsScreen::Draw(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha,
                      bool ready) {
  draw_ = draw;
  font_ = font;
  size_ = size;
  unit_ = unit;
  alpha_ = alpha;
  ready_ = ready;
  const ImVec2 delta = ImGui::GetIO().MouseDelta;
  moved_ = delta.x != 0.0f || delta.y != 0.0f;
  back_ = false;
  if (!loaded_) {
    Reload();
    if (CatalogStatus() != CatalogLoad::kLoading) {
      StartCatalogFetch();
    }
  }

  // A finished install changed what is on disk.
  const auto install = installer_.InstallSnapshot();
  if (was_installing_ && !install.in_progress) {
    rex::system::ModState::ApplyPendingUpdates(mods_root_);
    Reload();
  }
  was_installing_ = install.in_progress;

  BuildLists();
  BuildTools();
  TouchUpdate();
  Settle();
  TouchBegin();
  Input();
  const bool press = std::exchange(press_, false);
  const bool remove = std::exchange(remove_, false);
  if (press) {
    Press();
  } else if (remove && (focus_.zone == Zone::kAvailable || focus_.zone == Zone::kEnabled)) {
    ToggleRemoval(IdAt(focus_));
  }
  BuildLists();
  BuildTools();
  Settle();
  if (focus_.zone == Zone::kAvailable || focus_.zone == Zone::kEnabled)
    current_ = IdAt(focus_);
  BuildTools();
  if (focus_.zone != Zone::kAvailable && focus_.zone != Zone::kEnabled)
    trash_ = false;
  if (!(focus_ == shown_focus_)) {
    shown_focus_ = focus_;
    focus_since_ = Clock::now();
    focus_changed_ = true;
  }
  if (!grabbed_.empty() && focus_.zone != Zone::kEnabled)
    grabbed_.clear();

  const float u = unit;

  // Back, top left, where the version label sits on the main page.
  {
    const ImVec2 min(24.0f * u, 24.0f * u);
    const ImVec2 max(min.x + 150.0f * u, min.y + 44.0f * u);
    Hit(min, max, {Zone::kBack, 0}, true);
    Plate(min, max, focus_.zone == Zone::kBack, false, alpha);
    Text(min, max, Tr(IntroText::kHintBack), 24.0f * u, alpha, focus_.zone == Zone::kBack);
  }

  const char* title = T("mods_title", "Mods");
  const float title_size = 36.0f * u;
  const ImVec2 title_extent = font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title);
  draw->AddText(font, title_size, ImVec2(std::floor((size.x - title_extent.x) * 0.5f), 26.0f * u),
                Col(kText, alpha), title);

  const float width = std::min(1000.0f * u, size.x - 32.0f);
  const float x0 = (size.x - width) * 0.5f;
  const float x1 = x0 + width;

  // Tabs, with the count or the install status to their right.
  const float tab_y = 84.0f * u;
  const float tab_h = 40.0f * u;
  const char* tab_names[2] = {T("tab_installed", "Installed"), T("tab_all", "All")};
  for (int i = 0; i < TabCount(); ++i) {
    const ImVec2 min(x0 + float(i) * 180.0f * u, tab_y);
    const ImVec2 max(min.x + 170.0f * u, tab_y + tab_h);
    const bool focused = focus_ == Focus{Zone::kTab, i};
    Hit(min, max, {Zone::kTab, i}, true);
    Plate(min, max, focused, tab_ == i, alpha);
    Text(min, max, tab_names[i], 21.0f * u, alpha, focused || tab_ == i);
  }
  {
    std::string status;
    ImVec4 color = kMuted;
    if (install.in_progress) {
      status = install.total_bytes > 0
                   ? Fmt("installing_percent", "Installing... %.0f%%",
                         100.0 * double(install.downloaded_bytes) / double(install.total_bytes))
                   : T("installing", "Installing...");
    } else if (install.done) {
      status = install.message;
      color = install.ok ? kGood : kError;
    } else if (!status_.empty()) {
      status = status_;
      color = status_error_ ? kError : kGood;
    } else if (tab_ == 0) {
      status = entries_.size() == 1
                   ? Fmt("installed_one", "%zu mod installed; earlier entries win conflicts",
                         entries_.size())
                   : Fmt("installed_many", "%zu mods installed; earlier entries win conflicts",
                         entries_.size());
    }
    const float left = x0 + float(TabCount()) * 180.0f * u + 10.0f * u;
    const float text_size = 17.0f * u;
    const float w = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, status.c_str()).x;
    draw->PushClipRect(ImVec2(left, tab_y), ImVec2(x1, tab_y + tab_h), true);
    draw->AddText(font, text_size,
                  ImVec2(std::floor(std::max(left, x1 - w)),
                         std::floor(tab_y + (tab_h - text_size) * 0.5f)),
                  Col(color, alpha), status.c_str());
    draw->PopClipRect();
  }

  // Toolbar.
  const float tool_y = tab_y + tab_h + 8.0f * u;
  float tool_x = x0;
  for (size_t i = 0; i < tools_.size(); ++i) {
    const Tool& tool = tools_[i];
    const float w = font->CalcTextSizeA(19.0f * u, FLT_MAX, 0.0f, tool.label.c_str()).x + 32.0f * u;
    const ImVec2 min(tool_x, tool_y);
    const ImVec2 max(tool_x + w, tool_y + tab_h);
    tool_x += w + 10.0f * u;
    const bool focused = focus_ == Focus{Zone::kTool, int(i)};
    const float a = alpha * (tool.enabled ? 1.0f : 0.4f);
    Hit(min, max, {Zone::kTool, int(i)}, tool.enabled);
    Plate(min, max, focused, false, a);
    Text(min, max, tool.label.c_str(), 19.0f * u, a, focused);
  }

  // Lists and the detail box under them.
  const float detail_h = 92.0f * u;
  const float list_top = tool_y + tab_h + 12.0f * u;
  const float bottom = std::max(list_top + 90.0f * u, size.y - 130.0f * u - detail_h - 10.0f * u);
  if (tab_ == 0) {
    const float gap = 16.0f * u;
    const float pane_w = (width - gap) * 0.5f;
    const float head = 24.0f * u;
    draw->AddText(font, 18.0f * u, ImVec2(x0 + 4.0f * u, list_top), Col(kHeader, alpha),
                  T("pane_available", "Available"));
    draw->AddText(font, 18.0f * u, ImVec2(x0 + pane_w + gap + 4.0f * u, list_top),
                  Col(kHeader, alpha), T("pane_enabled", "Enabled"));
    const float row_h = 58.0f * u;
    List(Zone::kAvailable, Count(Zone::kAvailable), ImVec2(x0, list_top + head),
         ImVec2(x0 + pane_w, bottom), row_h,
         [&](int i, ImVec2 min, ImVec2 max) { DrawModRow(Zone::kAvailable, i, min, max); });
    List(Zone::kEnabled, Count(Zone::kEnabled), ImVec2(x0 + pane_w + gap, list_top + head),
         ImVec2(x1, bottom), row_h,
         [&](int i, ImVec2 min, ImVec2 max) { DrawModRow(Zone::kEnabled, i, min, max); });
  } else {
    List(Zone::kCatalog, Count(Zone::kCatalog), ImVec2(x0, list_top), ImVec2(x1, bottom),
         62.0f * u, [&](int i, ImVec2 min, ImVec2 max) { DrawCatalogRow(i, min, max); });
  }
  DrawDetail(ImVec2(x0, bottom + 10.0f * u), ImVec2(x1, bottom + 10.0f * u + detail_h));
  hint_y_ = bottom + 10.0f * u + detail_h + 14.0f * u;
  DrawHints();
  focus_changed_ = false;
  if (back_)
    Commit();
  return back_;
}

}  // namespace nocturne
