#include "intro_screen.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/net/http.h>
#include <rex/system/auto_updater.h>
#include <rex/filesystem.h>
#include <rex/system/mod_state.h>
#include <rex/system/mod_version.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "images.generated.h"
#include "intro_music.h"
#include "mods_screen.h"
#include "saves_screen.h"
#include "intro_text.h"
#include "version.generated.h"
#include "native_renderer_plume.h"
#include "release_id.h"
#include "settings.h"

REXCVAR_DECLARE(bool, auto_update_enabled);

namespace nocturne {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kFadeSeconds = 1.6f;

// The progress theme's crimson fading to black, with antique gold as the
// accent and ivory text: the castle's colors.
constexpr ImU32 kTop = IM_COL32(0x3A, 0x07, 0x10, 255);
constexpr ImU32 kBottom = IM_COL32(0x07, 0x02, 0x04, 255);
constexpr ImVec4 kAccent(0xD4 / 255.0f, 0xA8 / 255.0f, 0x4A / 255.0f, 1.0f);
constexpr ImVec4 kText(0.95f, 0.92f, 0.86f, 1.0f);
constexpr ImVec4 kQuitRed(0.90f, 0.22f, 0.22f, 1.0f);
constexpr ImVec4 kError(1.0f, 0.55f, 0.50f, 1.0f);

rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::WindowedAppContext* g_context = nullptr;
std::filesystem::path g_user_data_root;
std::optional<Clock::time_point> g_first_shown;

// The SDK's AutoUpdater cannot check yet (it needs a Runtime, which does not
// exist here), so the release lookup is done directly and only its installer
// is used. Leaked on purpose: a download in flight must not block exit.
struct UpdateCheck {
  std::mutex mutex;
  std::string newer;
  rex::system::UpdateInfo info;
  rex::system::AutoUpdater updater;
};
UpdateCheck* g_update_check = nullptr;

// The string value following `"key":` at or after `from`, empty when absent.
std::string JsonString(const std::string& body, const char* key, size_t from = 0) {
  const size_t k = body.find(std::string("\"") + key + "\"", from);
  if (k == std::string::npos)
    return {};
  const size_t open = body.find('"', body.find(':', k) + 1);
  const size_t close = open == std::string::npos ? open : body.find('"', open + 1);
  return close == std::string::npos ? std::string() : body.substr(open + 1, close - open - 1);
}

void StartUpdateCheck() {
  constexpr const char* kVersion = kVersionString;
  if (g_update_check || !kVersion[0] || !REXCVAR_GET(auto_update_enabled) ||
      !rex::system::AutoUpdater::SupportsSelfUpdate())
    return;
  g_update_check = new UpdateCheck;
  std::thread([state = g_update_check] {
    const auto response =
        rex::net::HttpGet("https://api.github.com/repos/birabittoh/NocturneRecomp/releases/latest");
    if (!response.ok())
      return;
    const std::string& body = response.body;
    const std::string tag = JsonString(body, "tag_name");
    std::string version = tag;
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
      version.erase(0, 1);
    if (version.empty() || rex::system::CompareVersionStrings(kVersion, version) >= 0)
      return;
    // The asset this platform installs, named like the SDK's update_asset_format.
    const std::string prefix =
        "\"nocturnerecomp-" + tag + "-" + rex::system::ModState::HostPlatformId() + ".";
    const size_t at = body.find(prefix);
    if (at == std::string::npos)
      return;
    rex::system::UpdateInfo info;
    info.tag = tag;
    info.version = version;
    const size_t end = body.find('"', at + 1);
    info.asset_name = body.substr(at + 1, end - at - 1);
    info.asset_url = JsonString(body, "browser_download_url", end);
    const std::string digest = JsonString(body, "digest", end);
    if (digest.rfind("sha256:", 0) == 0)
      info.sha256 = digest.substr(7);
    if (info.asset_url.empty())
      return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->info = info;
    state->newer = version;
  }).detach();
}

std::string NewerVersion() {
  if (!g_update_check)
    return {};
  std::lock_guard<std::mutex> lock(g_update_check->mutex);
  return g_update_check->newer;
}

struct Art {
  std::unique_ptr<rex::ui::ImmediateTexture> texture;
  float width = 0.0f;
  float height = 0.0f;
  bool tried = false;

  ImTextureID id() const { return texture ? reinterpret_cast<ImTextureID>(texture.get()) : ImTextureID{}; }
};

Art g_background;
Art g_logo;

// Art from res/ by file stem; missing art just draws nothing.
void Load(Art& art, const char* name) {
  if (art.tried)
    return;
  art.tried = true;
  for (const ResImage* image = kResImages; image->name; ++image) {
    if (std::strcmp(image->name, name) != 0)
      continue;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(image->data, image->size, w, h);
    auto* immediate = g_drawer->immediate_drawer();
    if (!immediate || rgba.empty()) {
      REXLOG_WARN("Intro: res/{}.png did not decode", name);
      return;
    }
    art.texture = immediate->CreateTexture(uint32_t(w), uint32_t(h),
                                           rex::ui::ImmediateTextureFilter::kLinear, false,
                                           rgba.data());
    art.width = float(w);
    art.height = float(h);
    return;
  }
}

ImFont* FindFont(const char* name) {
  for (ImFont* font : ImGui::GetIO().Fonts->Fonts) {
    if (std::strcmp(font->GetDebugName(), name) == 0)
      return font;
  }
  return ImGui::GetFont();
}

ImU32 Color(const ImVec4& c, float alpha = 1.0f) {
  return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

float Smooth(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Centered, word wrapped; returns the height used.
float CenteredText(ImDrawList* draw, ImFont* font, float size, float center_x, float y,
                   float wrap, ImU32 color, const std::string& text) {
  const char* begin = text.c_str();
  const char* end = begin + text.size();
  float line_y = y;
  while (begin < end) {
    const char* newline = static_cast<const char*>(std::memchr(begin, '\n', size_t(end - begin)));
    const char* stop = newline ? newline : end;
    const char* wrap_at = font->CalcWordWrapPosition(size, begin, stop, wrap);
    if (wrap_at == begin)
      wrap_at = stop;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, wrap_at);
    draw->AddText(font, size, ImVec2(std::floor(center_x - extent.x * 0.5f), line_y), color, begin,
                  wrap_at);
    line_y += size * 1.3f;
    begin = wrap_at;
    while (begin < stop && *begin == ' ')
      ++begin;
    if (begin == stop && newline)
      ++begin;
  }
  return line_y - y;
}

// Image scaled to cover the whole rect, cropped evenly.
void Cover(ImDrawList* draw, const Art& art, ImVec2 size, ImU32 tint) {
  const float scale = std::max(size.x / art.width, size.y / art.height);
  const float u = std::min(1.0f, size.x / (art.width * scale));
  const float v = std::min(1.0f, size.y / (art.height * scale));
  draw->AddImage(art.id(), ImVec2(0, 0), size, ImVec2((1 - u) * 0.5f, (1 - v) * 0.5f),
                 ImVec2((1 + u) * 0.5f, (1 + v) * 0.5f), tint);
}

// Slow rising embers, for when there is no background art and to give one life.
void Motes(ImDrawList* draw, ImVec2 size, float unit, float t, float alpha) {
  constexpr int kCount = 36;
  for (int i = 0; i < kCount; ++i) {
    const float seed = float(i) * 12.9898f;
    const float rx = std::fmod(std::sin(seed) * 43758.5453f, 1.0f);
    const float ry = std::fmod(std::sin(seed * 1.7f) * 24634.6345f, 1.0f);
    const float speed = 0.015f + 0.02f * std::fabs(ry);
    const float x = std::fabs(rx) * size.x + std::sin(t * 0.3f + seed) * 18.0f * unit;
    const float phase = std::fmod(std::fabs(ry) + t * speed, 1.0f);
    const float y = size.y * (1.1f - phase * 1.2f);
    const float glow = std::sin(phase * 3.14159f) * (0.5f + 0.5f * std::sin(t * 1.3f + seed));
    const float r = (1.5f + 2.5f * std::fabs(rx)) * unit;
    draw->AddCircleFilled(ImVec2(x, y), r * 3.0f, IM_COL32(255, 120, 70, int(18 * glow * alpha)));
    draw->AddCircleFilled(ImVec2(x, y), r, IM_COL32(255, 205, 160, int(150 * glow * alpha)));
  }
}

void Answer(GameDataChoice choice, int version);

class IntroDialog final : public rex::ui::ImGuiDialog {
 public:
  IntroDialog(rex::ui::ImGuiDrawer* drawer, const GameDataPrompt& prompt)
      : rex::ui::ImGuiDialog(drawer), prompt_(prompt) {
    BuildOptions();
  }

  // The runtime's input driver only learns of pads from the device added events
  // SDL queues when the subsystem first starts, so drop ours before it does.
  ~IntroDialog() override {
    if (pad_)
      SDL_CloseGamepad(pad_);
    if (pad_init_)
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }

  void Update(const GameDataPrompt& prompt) {
    const bool rebuild = prompt.confirm.empty() != prompt_.confirm.empty() ||
                         prompt.outdated.empty() != prompt_.outdated.empty() ||
                         prompt.adding != prompt_.adding ||
                         prompt.versions.size() != prompt_.versions.size() ||
                         prompt.selected_version != selected_version_ ||
                         prompt.can_extract != prompt_.can_extract;
    prompt_ = prompt;
    if (rebuild) {
      BuildOptions();
      on_picker_ = picker_open_ = on_music_ = on_trash_ = false;
    }
  }
  // The library lists the versions on the left and what to do on the right.
  // Picking a source has the sources on the left, then Extract. Asking to
  // delete has only a choice.
  void BuildOptions() {
    options_.clear();
    selected_version_ = prompt_.selected_version;
    if (!prompt_.confirm.empty()) {
      options_.push_back({IntroText::kCancel, GameDataChoice::kCancel, 0, 0});
      options_.push_back({IntroText::kDeleteFiles, GameDataChoice::kConfirmDelete, 1, 0});
      selected_ = 0;
      return;
    }
    if (!prompt_.outdated.empty()) {
      options_.push_back({IntroText::kSkip, GameDataChoice::kStart, 0, 0});
      options_.push_back({IntroText::kExtractAgain, GameDataChoice::kExtractAgain, 1, 0});
      selected_ = 1;
      return;
    }
    if (Library()) {
      // Versions fill a column top down, then the next; the plus takes the slot after.
      const int versions = std::min(int(prompt_.versions.size()), kMaxVersions);
      const bool can_add = versions < kMaxVersions;
      const int right = versions + (can_add ? 1 : 0) > kColumnRows ? 2 : 1;
      for (int i = 0; i < versions; ++i)
        options_.push_back({IntroText::kStart, GameDataChoice::kStart, i / kColumnRows,
                            i % kColumnRows, i});
      for (int i = 0; i < versions; ++i)
        options_.push_back({IntroText::kDeleteVersion, GameDataChoice::kDelete, kTrashColumn,
                            i % kColumnRows, i, true});
      options_.push_back({IntroText::kStart, GameDataChoice::kStart, right, 0});
      options_.push_back(
          {IntroText::kStart, GameDataChoice::kCancel, right, 1, -1, false, false, true});
      options_.push_back({IntroText::kStart, GameDataChoice::kCancel, right, 2, -1, false, true});
      if (can_add)
        options_.push_back({IntroText::kAddVersion, GameDataChoice::kAddVersion,
                            versions / kColumnRows, versions % kColumnRows});
      options_.push_back({IntroText::kQuit, GameDataChoice::kQuit, right, kCornerRow});
      selected_ = IndexOf(GameDataChoice::kStart);
      return;
    }
    options_.push_back({IntroText::kSelectFile, GameDataChoice::kDiscImage, 0, 0});
    options_.push_back({IntroText::kExtract, GameDataChoice::kExtract, 1, 0});
    if (prompt_.versions.empty())
      options_.push_back({IntroText::kQuit, GameDataChoice::kQuit, 1, kCornerRow});
    else
      options_.push_back({IntroText::kCancel, GameDataChoice::kCancel, 1, 1});
    selected_ = prompt_.can_extract ? IndexOf(GameDataChoice::kExtract) : 0;
  }
  void SetBusy(bool busy) { busy_ = busy; }
  void SetWorking(bool working) {
    working_ = working;
    if (working)
      phases_.fill(PhaseState::kPending);
  }
  void SetPhase(Phase phase, PhaseState state) { phases_[size_t(phase)] = state; }
  void SetProgress(const std::string& title, float fraction, const std::string& detail) {
    work_title_ = TrProgress(title);
    work_fraction_ = fraction;
    work_detail_ = detail;
  }

  // With the version row it applies to.
  std::optional<std::pair<GameDataChoice, int>> TakeChoice() {
    if (!choice_)
      return std::nullopt;
    const auto choice = std::make_pair(*choice_, selected_version_);
    choice_.reset();
    return choice;
  }

 protected:
  ImVec2 seen_size_{0, 0};
  Clock::time_point size_changed_;
  bool size_pending_ = false;
  int scroll_owner_ = -1;
  Clock::time_point scroll_since_;

  // The SDK's gamepad bridge only exists once the runtime is up, which is
  // after this screen, so read the pad here and hand it to ImGui as keys.
  void PollGamepad(ImGuiIO& io) {
    if (!pad_init_) {
      pad_init_ = true;
      SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }
    SDL_UpdateGamepads();
    if (pad_ && !SDL_GamepadConnected(pad_)) {
      SDL_CloseGamepad(pad_);
      pad_ = nullptr;
    }
    if (!pad_) {
      int count = 0;
      SDL_JoystickID* ids = SDL_GetGamepads(&count);
      if (ids && count > 0)
        pad_ = SDL_OpenGamepad(ids[0]);
      SDL_free(ids);
      if (!pad_)
        return;
    }
    // Without it ImGui zeroes every gamepad key each frame.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad_, a) / 32767.0f; };
    const float lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
    const float ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    auto down = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad_, b); };
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, down(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -0.5f);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 0.5f);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, down(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -0.5f);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, down(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 0.5f);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, down(SDL_GAMEPAD_BUTTON_SOUTH));
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, down(SDL_GAMEPAD_BUTTON_EAST));
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, down(SDL_GAMEPAD_BUTTON_WEST));
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, down(SDL_GAMEPAD_BUTTON_NORTH));
    io.AddKeyEvent(ImGuiKey_GamepadL1, down(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    io.AddKeyEvent(ImGuiKey_GamepadR1, down(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    io.AddKeyEvent(ImGuiKey_GamepadStart, down(SDL_GAMEPAD_BUTTON_START));
    io.AddKeyEvent(ImGuiKey_GamepadBack, down(SDL_GAMEPAD_BUTTON_BACK));
  }

  bool pad_init_ = false;
  SDL_Gamepad* pad_ = nullptr;

  void OnDraw(ImGuiIO& io) override {
    PollGamepad(io);
    Load(g_background, "background");
    Load(g_logo, "logo");

    const ImVec2 size = io.DisplaySize;
    // Saved once the size has held still, not while the edge is being dragged.
    if (size.x != seen_size_.x || size.y != seen_size_.y) {
      seen_size_ = size;
      size_changed_ = Clock::now();
      size_pending_ = true;
    } else if (size_pending_ && Clock::now() - size_changed_ > std::chrono::milliseconds(500)) {
      size_pending_ = false;
      PersistWindowSize();
    }
    const float t = std::chrono::duration<float>(Clock::now() - *g_first_shown).count();
    const float shown = Smooth(t / kFadeSeconds);
    // Laid out for 1280x720 and scaled to the window.
    const float unit = std::min(size.x / 1280.0f, size.y / 720.0f);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##intro", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Reread once at first, and again whenever the Mods page closes.
    if (!mod_block_known_ || (mods_was_open_ && !mods_open_)) {
      mod_block_ = ModsScreen::BlockingIssue();
      mod_block_known_ = true;
    }
    mods_was_open_ = mods_open_;

    draw->AddRectFilledMultiColor(ImVec2(0, 0), size, kTop, kTop, kBottom, kBottom);
    if (g_background.texture)
      Cover(draw, g_background, size, IM_COL32_WHITE);
    // Darkens the lower half the text sits on.
    draw->AddRectFilledMultiColor(ImVec2(0, size.y * 0.35f), size, IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190),
                                  IM_COL32(0, 0, 0, 190));
    Motes(draw, size, unit, t, shown);

    if (mods_open_) {
      DrawPage(*mods_, mods_open_, draw, size, unit, t);
    } else if (saves_open_) {
      DrawPage(*saves_, saves_open_, draw, size, unit, t);
    } else {
      float y = 46.0f * unit;
      if (g_logo.texture) {
        const float w = std::min(620.0f * unit, size.x * 0.9f);
        const float h = w * g_logo.height / g_logo.width;
        const float rise = (1.0f - Smooth((t - 0.2f) / 1.2f)) * 16.0f * unit;
        const ImVec2 min((size.x - w) * 0.5f, y + rise);
        draw->AddImage(g_logo.id(), min, ImVec2(min.x + w, min.y + h), ImVec2(0, 0), ImVec2(1, 1),
                       IM_COL32(255, 255, 255, int(255 * Smooth((t - 0.2f) / 1.2f))));
        y += h + 22.0f * unit;
      } else {
        y += 220.0f * unit;
      }

      const float body_in = Smooth((t - 0.7f) / 0.8f);
      ImFont* font = FindFont(kIntroFontName);
      const float wrap = std::min(820.0f * unit, size.x - 32.0f);
      if (working_) {
        y += DrawWork(draw, font, size, unit, y, wrap, body_in);
      } else {
        std::string body;
        if (!prompt_.confirm.empty()) {
          body = prompt_.confirm;
        } else if (!prompt_.outdated.empty()) {
          body = prompt_.outdated;
        } else if (Library()) {
          body = Tr(IntroText::kLibraryReady);
        } else if (prompt_.can_extract) {
          body = (prompt_.release.empty() ? std::string(Tr(IntroText::kFound))
                                          : Tr(IntroText::kFoundRelease, prompt_.release)) +
                 "\n" + Tr(IntroText::kPressExtract);
        } else {
          body = Tr(IntroText::kNeedFiles);
          body += std::string("\n") + Tr(IntroText::kSelectIsoOnly);
          if (!prompt_.copy_hint.empty())
            body += "\n" + Tr(IntroText::kCopyHint, prompt_.copy_hint);
        }
        y += CenteredText(draw, font, 22.0f * unit, size.x * 0.5f, y, wrap, Color(kText, body_in),
                          body);
        if (!Library() && !prompt_.error.empty() && !prompt_.can_extract && !prompt_.release.empty() &&
            prompt_.error.find(prompt_.release) == std::string::npos) {
          y += 8.0f * unit;
          y += CenteredText(draw, font, 20.0f * unit, size.x * 0.5f, y, wrap,
                            Color(kText, body_in), Tr(IntroText::kSelected, prompt_.release));
        }
        if (!prompt_.error.empty()) {
          y += 8.0f * unit;
          y += CenteredText(draw, font, 20.0f * unit, size.x * 0.5f, y, wrap,
                            Color(kError, body_in), prompt_.error);
        }
      }

      if (kVersionString[0] != '\0') {
        const std::string version = std::string("v") + kVersionString;
        draw->AddText(font, 18.0f * unit, ImVec2(24.0f * unit, 24.0f * unit),
                      IM_COL32(255, 255, 255, int(255 * body_in)), version.c_str());
      }
      if (working_) {
        on_picker_ = picker_open_ = on_music_ = on_trash_ = false;
        DrawLanguagePicker(draw, font, size, unit, body_in, false);
        DrawMusicToggle(draw, font, size, unit, body_in, false, false);
      } else {
        // Ready once faded in, and not while a native dialog is open.
        const bool ready = body_in >= 1.0f && !busy_;
        // One key press moves focus once, not again in the picker.
        const bool handled = DrawButtons(draw, font, size, unit,
                                         std::max(y + 22.0f * unit, size.y * 0.66f), body_in, ready);
        DrawLanguagePicker(draw, font, size, unit, body_in, ready && !handled);
        DrawMusicToggle(draw, font, size, unit, body_in, on_music_, ready && !handled);
        DrawHints(draw, font, size, unit, body_in);
      }

    }

    // Fade from black on the first showing only; a cancelled dialog comes back
    // to the screen as it was.
    if (shown < 1.0f)
      ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), size,
                                                    IM_COL32(0, 0, 0, int(255 * (1.0f - shown))));
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  // The Mods or Saves page in place of the main one.
  template <class Page>
  void DrawPage(Page& page, bool& open, ImDrawList* draw, ImVec2 size, float unit, float t) {
    const float body_in = Smooth((t - 0.7f) / 0.8f);
    ImFont* font = FindFont(kIntroFontName);
    const bool ready = body_in >= 1.0f && !busy_ && !working_;
    if (page.Draw(draw, font, size, unit, body_in, ready && !picker_open_))
      open = false;
    DrawMusicToggle(draw, font, size, unit, body_in, page.MusicFocused(), false);
    // The picker keeps its keys here too; the page is told where the cursor goes.
    on_picker_ = page.PickerFocused();
    on_music_ = false;
    DrawLanguagePicker(draw, font, size, unit, body_in, ready);
    if (on_picker_ && !page.PickerFocused())
      page.FocusPicker();
    else if (!on_picker_ && page.PickerFocused())
      on_music_ ? page.FocusMusic() : page.ReturnFocus();
    on_picker_ = on_music_ = false;
  }

  // Quit sits in the top right corner, one step above the right column.
  static constexpr int kCornerRow = -1;
  static constexpr int kColumnRows = 3;
  static constexpr int kTrashColumn = -2;

  bool CanDelete(int version) const {
    if (version < 0 || version >= int(prompt_.versions.size()))
      return false;
    return prompt_.versions[size_t(version)].can_delete;
  }

  bool ShowsUpdateButton() const {
    return !NewerVersion().empty() && Library() && !working_ && !busy_ && prompt_.confirm.empty();
  }

  // Top left, under the version: download, progress, restart.
  void DrawUpdateButton(ImDrawList* draw, ImFont* font, float unit, float alpha, ImVec2 min,
                        float w, float h) {
    using rex::system::AutoUpdater;
    const ImVec4 green(0.45f, 0.85f, 0.55f, 1.0f);
    const auto install = g_update_check->updater.InstallSnapshot();
    const bool staged = AutoUpdater::HasPendingSelfUpdate(AutoUpdater::InstallRoot());
    const std::string version = "v" + NewerVersion();
    std::string label;
    ImVec4 ink = kText;
    bool clickable = !picker_open_;
    if (install.in_progress) {
      char percent[16] = "0%";
      if (install.total_bytes > 0)
        std::snprintf(percent, sizeof(percent), "%.0f%%",
                      100.0 * double(install.downloaded_bytes) / double(install.total_bytes));
      label = percent;
      clickable = false;
    } else if (install.done && !install.ok) {
      label = install.message;
      ink = kError;
    } else if (staged) {
      label = REX_PLATFORM_ANDROID ? std::string(Tr("settings", "install_update")) + " " + version
                                   : Tr("settings", "update_restart", version);
    } else {
      label = Tr("settings", "update_download", version);
    }

    const ImVec2 max(min.x + w, min.y + h);
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID("##update");
    const bool pressed = ImGui::InvisibleButton("##button", ImVec2(w, h)) && clickable;
    const bool hovered = clickable && ImGui::IsItemHovered();
    ImGui::PopID();
    const float rounding = 6.0f * unit;
    const float a = alpha * (install.in_progress ? 0.4f : 1.0f);
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((hovered ? 150 : 100) * a)), rounding);
    draw->AddRect(min, max, Color(green, (hovered ? 1.0f : 0.7f) * a), rounding, 0,
                  std::max(1.0f, (hovered ? 2.0f : 1.0f) * unit));
    // Shrunk to fit the column.
    const float pad = 10.0f * unit;
    float size = 20.0f * unit;
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str());
    if (extent.x > w - 2.0f * pad) {
      size *= (w - 2.0f * pad) / extent.x;
      extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str());
    }
    draw->AddText(font, size,
                  ImVec2(std::floor(min.x + (w - extent.x) * 0.5f),
                         std::floor(min.y + (h - extent.y) * 0.5f)),
                  Color(ink, a * (hovered ? 1.0f : 0.8f)), label.c_str());
    if (!pressed)
      return;
    if (!staged) {
      g_update_check->updater.InstallAsync(g_update_check->info, AutoUpdater::InstallRoot());
    } else if (AutoUpdater::ApplyAndRestart(AutoUpdater::InstallRoot(),
                                            rex::filesystem::GetExecutablePath()) &&
               !REX_PLATFORM_ANDROID) {
      g_context->CallInUIThreadDeferred([] { Answer(GameDataChoice::kQuit, -1); });
    }
  }

  struct Option {
    IntroText label;
    GameDataChoice choice;
    int column;
    int row;
    // A version row in the library, by index; its choice is unused.
    int version = -1;
    // The trash can beside a version row; kept out of the keyboard grid.
    bool trash = false;
    // Opens the Mods page; answers nothing.
    bool mods = false;
    // Opens the Saves page; answers nothing.
    bool saves = false;
  };

  bool Library() const {
    return prompt_.confirm.empty() && prompt_.outdated.empty() && !prompt_.adding &&
           !prompt_.versions.empty();
  }

  const VersionRow* SelectedRow() const {
    return selected_version_ >= 0 && selected_version_ < int(prompt_.versions.size())
               ? &prompt_.versions[size_t(selected_version_)]
               : nullptr;
  }

  // The text of the release Extract applies to, or of every one Start does.
  uint32_t Languages() const {
    if (!Library())
      return prompt_.languages;
    uint32_t languages = 0;
    for (const VersionRow& row : prompt_.versions) {
      if (!row.languages)
        return 0;
      languages |= row.languages;
    }
    return languages;
  }

  // Whether those releases have text in language `index`; any does before one
  // is picked.
  bool Supported(int index) const {
    const uint32_t languages = Languages();
    return !languages || (languages & TextLanguageBit(IntroLanguageCodeAt(index))) != 0;
  }

  int IndexOf(GameDataChoice choice) const {
    for (size_t i = 0; i < options_.size(); ++i) {
      if (options_[i].version < 0 && options_[i].choice == choice)
        return int(i);
    }
    return 0;
  }

  // The phase row and the current item's progress; returns the height used.
  float DrawWork(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float top, float wrap,
                 float alpha) {
    const float t = std::chrono::duration<float>(Clock::now() - *g_first_shown).count();
    const float label_size = 20.0f * unit;
    const float step = std::min(220.0f * unit, size.x / float(kInstallPhaseCount));
    const float row_x = (size.x - step * float(kInstallPhaseCount)) * 0.5f;
    for (int i = 0; i < kInstallPhaseCount; ++i) {
      const PhaseState state = phases_[size_t(i)];
      const char* name = TrPhase(Phase(i));
      const ImVec2 extent = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, name);
      const float cx = row_x + step * (float(i) + 0.5f);
      const float r = 6.0f * unit;
      const ImVec2 dot(cx, top + r);
      if (state == PhaseState::kActive) {
        const float pulse = 0.65f + 0.35f * std::sin(t * 4.0f);
        draw->AddCircleFilled(dot, r * 1.9f, Color(kAccent, 0.25f * pulse * alpha));
        draw->AddCircleFilled(dot, r, Color(kAccent, alpha));
      } else if (state == PhaseState::kDone) {
        draw->AddCircleFilled(dot, r, Color(kText, 0.85f * alpha));
      } else if (state == PhaseState::kSkipped) {
        draw->AddLine(ImVec2(dot.x - r, dot.y), ImVec2(dot.x + r, dot.y),
                      Color(kText, 0.35f * alpha), std::max(1.0f, 2.0f * unit));
      } else {
        draw->AddCircle(dot, r, Color(kText, 0.45f * alpha), 0, std::max(1.0f, 1.5f * unit));
      }
      const float text_alpha = state == PhaseState::kActive   ? 1.0f
                               : state == PhaseState::kDone   ? 0.85f
                                                              : 0.4f;
      draw->AddText(font, label_size, ImVec2(std::floor(cx - extent.x * 0.5f), top + 2.4f * r),
                    Color(kText, text_alpha * alpha), name);
    }
    float used = 2.4f * 6.0f * unit + label_size + 18.0f * unit;

    const float bar_w = std::min(560.0f * unit, size.x - 32.0f);
    const float bar_h = 10.0f * unit;
    const ImVec2 min((size.x - bar_w) * 0.5f, top + used);
    const ImVec2 max(min.x + bar_w, min.y + bar_h);
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int(140 * alpha)), bar_h * 0.5f);
    if (work_fraction_ >= 0.0f && work_fraction_ <= 1.0f) {
      draw->AddRectFilled(min, ImVec2(min.x + bar_w * work_fraction_, max.y), Color(kAccent, alpha),
                          bar_h * 0.5f);
    } else {
      // Unknown length: a segment sweeping along.
      const float seg = bar_w * 0.25f;
      const float at = (std::fmod(t * 0.6f, 1.0f)) * (bar_w + seg) - seg;
      const float x0 = std::max(0.0f, at), x1 = std::min(bar_w, at + seg);
      if (x1 > x0)
        draw->AddRectFilled(ImVec2(min.x + x0, min.y), ImVec2(min.x + x1, max.y),
                            Color(kAccent, alpha), bar_h * 0.5f);
    }
    used += bar_h + 14.0f * unit;
    if (!work_title_.empty())
      used += CenteredText(draw, font, 22.0f * unit, size.x * 0.5f, top + used, wrap,
                           Color(kText, alpha), work_title_);
    if (!work_detail_.empty())
      used += CenteredText(draw, font, 18.0f * unit, size.x * 0.5f, top + used, wrap,
                           Color(kText, 0.6f * alpha), work_detail_);
    return used;
  }

  static bool Pressed(std::initializer_list<ImGuiKey> keys) {
    // Alt+Enter toggles fullscreen.
    if (ImGui::GetIO().KeyAlt)
      return false;
    return std::any_of(keys.begin(), keys.end(), [](ImGuiKey k) { return ImGui::IsKeyPressed(k); });
  }

  bool Enabled(int i) const {
    const Option& option = options_[size_t(i)];
    if (option.trash)
      return CanDelete(option.version);
    if (option.mods || option.saves)
      return true;
    if (option.version >= 0)
      return true;
    const VersionRow* row = SelectedRow();
    switch (option.choice) {
      case GameDataChoice::kStart:
        return Supported(IntroLanguage()) && mod_block_.empty();
      case GameDataChoice::kExtract:
        return prompt_.can_extract;
      case GameDataChoice::kDelete:
        return row && row->can_delete;
      default:
        return true;
    }
  }

  // Moves the cursor; landing on a version row picks that version.
  void Focus(int i) {
    if (i < 0)
      return;
    selected_ = i;
    if (options_[size_t(i)].version >= 0)
      selected_version_ = options_[size_t(i)].version;
  }

  int At(int column, int row) const {
    for (int i = 0; i < int(options_.size()); ++i) {
      const Option& option = options_[size_t(i)];
      if (option.column == column && option.row == row && Enabled(i))
        return i;
    }
    return -1;
  }

  int LastRow(int column) const {
    int last = -1;
    for (const Option& option : options_) {
      if (option.column == column)
        last = std::max(last, option.row);
    }
    return last;
  }

  // Up and down stay in a column, skipping what cannot be used, and below its
  // last button is the language picker. Left and right keep the row, or take
  // the nearest usable one; into the version list they land on the picked
  // version.
  void Move(int dx, int dy) {
    const Option& from = options_[size_t(selected_)];
    if (dy != 0) {
      for (int row = from.row + dy; row >= 0 && row <= LastRow(from.column); row += dy) {
        if (const int i = At(from.column, row); i >= 0) {
          Focus(i);
          return;
        }
      }
      if (dy < 0 && from.row == 0) {
        if (const int i = At(from.column, kCornerRow); i >= 0)
          Focus(i);
        return;
      }
      if (dy > 0)
        on_picker_ = true;
      return;
    }
    int columns = 0;
    for (const Option& option : options_)
      columns = std::max(columns, option.column + 1);
    const int column = (from.column + dx + columns) % columns;
    if (Library() && column < columns - 1 && from.column == columns - 1 &&
        selected_version_ >= 0) {
      for (int i = 0; i < int(options_.size()); ++i) {
        if (options_[size_t(i)].version == selected_version_) {
          Focus(i);
          return;
        }
      }
    }
    const int last = LastRow(column);
    for (int row = std::min(from.row, last); row >= 0; --row) {
      if (const int i = At(column, row); i >= 0) {
        Focus(i);
        return;
      }
    }
    for (int row = from.row + 1; row <= last; ++row) {
      if (const int i = At(column, row); i >= 0) {
        Focus(i);
        return;
      }
    }
  }

  // A version row picks that version for Delete and hands the cursor to Start.
  void Press(int i) {
    const Option& option = options_[size_t(i)];
    if (option.mods) {
      if (!mods_)
        mods_ = std::make_unique<ModsScreen>(g_drawer->immediate_drawer(), g_context);
      mods_->Reopen(gamepad_);
      mods_open_ = true;
      return;
    }
    if (option.saves) {
      if (!saves_)
        saves_ = std::make_unique<SavesScreen>(g_drawer->immediate_drawer(), g_context,
                                               g_user_data_root);
      saves_->Reopen(gamepad_);
      saves_open_ = true;
      return;
    }
    if (option.version < 0) {
      choice_ = option.choice;
      return;
    }
    selected_version_ = option.version;
    const int start = IndexOf(GameDataChoice::kStart);
    selected_ = Enabled(start) ? start : i;
  }

  GameDataChoice BackChoice() const {
    if (!prompt_.outdated.empty())
      return GameDataChoice::kStart;
    return !prompt_.confirm.empty() || (!Library() && !prompt_.versions.empty())
               ? GameDataChoice::kCancel
               : GameDataChoice::kQuit;
  }

  // Returns whether it handled a key.
  bool DrawButtons(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float top, float alpha,
                   bool ready) {
    const int count = int(options_.size());
    const bool library = Library();
    if (!Enabled(selected_)) {
      for (int i = 0; i < count; ++i) {
        if (Enabled(i)) {
          selected_ = i;
          break;
        }
      }
    }
    const bool keys = ready && !on_picker_ && !on_music_ && !picker_open_;
    const bool on_version = library && options_[size_t(selected_)].version >= 0 &&
                            CanDelete(selected_version_);
    if (!on_version)
      on_trash_ = false;
    bool handled = true;
    if (keys && on_trash_) {
      // Left returns to the version; right goes on to the next column.
      if (Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft})) {
        on_trash_ = false;
      } else if (Pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight})) {
        on_trash_ = false;
        Move(1, 0);
      } else if (Pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown})) {
        on_trash_ = false;
        Move(0, 1);
      } else if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp})) {
        on_trash_ = false;
        Move(0, -1);
      } else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                          ImGuiKey_GamepadFaceDown, ImGuiKey_Delete, ImGuiKey_GamepadFaceLeft})) {
        choice_ = GameDataChoice::kDelete;
      } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
        choice_ = BackChoice();
      } else {
        handled = false;
      }
    } else if (keys && Pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown})) {
      Move(0, 1);
    } else if (keys && Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp})) {
      Move(0, -1);
    } else if (keys && Pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight})) {
      if (on_version)
        on_trash_ = true;
      else
        Move(1, 0);
    } else if (keys && Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft})) {
      Move(-1, 0);
    } else if (keys && Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                                ImGuiKey_GamepadFaceDown})) {
      Press(selected_);
    } else if (keys && library && Pressed({ImGuiKey_Delete, ImGuiKey_GamepadFaceLeft})) {
      if (CanDelete(selected_version_))
        choice_ = GameDataChoice::kDelete;
    } else if (keys && Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
      choice_ = BackChoice();
    } else {
      handled = false;
    }

    const float h = 50.0f * unit;
    const float label_size = 24.0f * unit;
    const float gap = 12.0f * unit;
    const float column_gap = 24.0f * unit;
    int columns = 0;
    for (const Option& option : options_)
      columns = std::max(columns, option.column + 1);
    // Library: narrow version columns then the actions; otherwise two equal columns.
    float widths[3] = {300.0f * unit, 300.0f * unit, 0.0f};
    if (library) {
      for (int c = 0; c < columns - 1; ++c)
        widths[c] = (columns == 3 ? 320.0f : 400.0f) * unit;
      widths[columns - 1] = 200.0f * unit;
    }
    float total = float(columns - 1) * column_gap;
    for (int c = 0; c < columns; ++c)
      total += widths[c];
    float column_x[3];
    column_x[0] = (size.x - total) * 0.5f;
    for (int c = 1; c < columns; ++c)
      column_x[c] = column_x[c - 1] + widths[c - 1] + column_gap;
    // The list ends above the hints and the corner controls.
    const float bottom = size.y - 120.0f * unit;
    if (library)
      top = std::min(top, bottom - float(kColumnRows) * (h + gap) + gap);

    const float t = std::chrono::duration<float>(Clock::now() - *g_first_shown).count();
    for (int i = 0; i < count; ++i) {
      const Option& option = options_[size_t(i)];
      if (option.trash) {
        DrawTrash(draw, i, option, column_x, widths, top, h, unit, alpha, ready);
        continue;
      }
      const bool corner = option.row == kCornerRow;
      const bool plus = option.version < 0 && option.choice == GameDataChoice::kAddVersion;
      const int row = option.row;
      const float corner_size = 44.0f * unit;
      const float trash_room = option.version >= 0 && option.version == selected_version_ ? h + 8.0f * unit : 0.0f;
      const float w = corner ? corner_size : widths[option.column] - trash_room;
      const float box_h = corner ? corner_size : h;
      const ImVec2 min = corner ? ImVec2(size.x - 24.0f * unit - corner_size, 24.0f * unit)
                                : ImVec2(column_x[option.column], top + float(row) * (h + gap));
      const ImVec2 max(min.x + w, min.y + box_h);
      const ImVec4& accent = corner ? kQuitRed : kAccent;
      const bool usable = Enabled(i);
      ImGui::SetCursorScreenPos(min);
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##option", ImVec2(w, box_h)) && ready && usable && !picker_open_)
        Press(i);
      // Only a moving mouse selects, so a resting cursor does not undo the keys.
      if (ready && usable && !picker_open_ && ImGui::IsItemHovered() && MouseMoved()) {
        selected_ = i;
        on_picker_ = false;
        on_music_ = false;
        on_trash_ = false;
      }
      ImGui::PopID();

      const bool active = i == selected_ && usable && !on_picker_ && !on_music_ && !on_trash_;
      const bool picked = option.version >= 0 && option.version == selected_version_;
      const float rounding = 6.0f * unit;
      // Disabled while working, and what cannot be used yet.
      const float a = alpha * (usable && !working_ && !busy_ ? 1.0f : 0.4f);
      // Extract pulses a soft halo
      const bool glow = usable && option.version < 0 && option.choice == GameDataChoice::kExtract;
      const float pulse = 0.55f + 0.45f * std::sin(t * 3.0f);
      if (glow) {
        constexpr int kLayers = 8;
        for (int layer = kLayers; layer >= 1; --layer) {
          const float grow = float(layer) * 2.5f * unit * (0.7f + 0.3f * pulse);
          const float fade = 1.0f - float(layer - 1) / float(kLayers);
          draw->AddRectFilled(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow),
                              Color(kAccent, 0.09f * fade * fade * pulse * a), rounding + grow);
        }
      }
      draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((active ? 150 : 100) * a)), rounding);
      if (picked)
        draw->AddRectFilled(min, max, Color(kAccent, 0.25f * a), rounding);
      if (corner && active)
        draw->AddRectFilled(min, max, Color(accent, 0.3f * a), rounding);
      const ImU32 border = active ? Color(accent, a)
                           : glow ? Color(kAccent, (0.5f + 0.5f * pulse) * a)
                                  : IM_COL32(255, 255, 255, int(60 * a));
      draw->AddRect(min, max, border, rounding, 0,
                    std::max(1.0f, (active || glow ? 2.0f : 1.0f) * unit));
      if (option.version >= 0) {
        // Text slides only under the cursor, from the start each time it lands.
        if (!active) {
          if (scroll_owner_ == i)
            scroll_owner_ = -1;
        } else if (scroll_owner_ != i) {
          scroll_owner_ = i;
          scroll_since_ = Clock::now();
        }
        DrawVersionRow(draw, font, unit, min, max, prompt_.versions[size_t(option.version)],
                       a * (active || picked ? 1.0f : 0.8f),
                       active ? std::chrono::duration<float>(Clock::now() - scroll_since_).count()
                              : -1.0f);
      } else {
        const IntroText text = option.label;
        const ImU32 ink = Color(kText, a * (active ? 1.0f : 0.8f));
        if (plus) {
          const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
          const float r = box_h * 0.22f;
          const float thick = std::max(2.0f, 3.0f * unit);
          draw->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), ink, thick);
          draw->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), ink, thick);
        } else if (corner) {
          const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
          const float r = box_h * 0.22f;
          const float thick = std::max(2.0f, 3.0f * unit);
          draw->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ink, thick);
          draw->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), ink, thick);
        } else {
          const char* label = option.mods    ? (SdkUiText("mods_title") ? SdkUiText("mods_title") : "Mods")
                              : option.saves ? Tr("saves", "title")
                                             : Tr(text);
          const ImVec2 extent = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
          draw->AddText(font, label_size,
                        ImVec2(std::floor(min.x + (w - extent.x) * 0.5f),
                               std::floor(min.y + (box_h - extent.y) * 0.5f)),
                        ink, label);
        }
      }
      const GameDataChoice go = library ? GameDataChoice::kStart : GameDataChoice::kExtract;
      if (option.version < 0 && option.choice == go && library && prompt_.confirm.empty() &&
          !mod_block_.empty()) {
        DrawWarning(draw, font, size, unit, ImVec2(max.x, min.y), h, alpha, mod_block_);
      } else if (option.version < 0 && option.choice == go && prompt_.confirm.empty() &&
                 !Supported(IntroLanguage())) {
        const std::string release =
            library ? prompt_.versions.front().label : prompt_.release;
        DrawLanguageWarning(draw, font, size, unit, ImVec2(max.x, min.y), h, alpha, release);
      }
    }
    if (ShowsUpdateButton())
      DrawUpdateButton(draw, font, unit, alpha, ImVec2(24.0f * unit, 54.0f * unit), 260.0f * unit,
                       40.0f * unit);
    return keys && handled;
  }

  // Right of its version row; acts as Delete on that version.
  void DrawTrash(ImDrawList* draw, int i, const Option& option, const float* column_x,
                 const float* widths, float top, float h, float unit, float alpha, bool ready) {
    if (option.version != selected_version_)
      return;
    const int column = option.version / kColumnRows;
    const ImVec2 min(column_x[column] + widths[column] - h, top + float(option.row) * (h + 12.0f * unit));
    const ImVec2 max(min.x + h, min.y + h);
    const bool usable = CanDelete(option.version);
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID(1000 + i);
    if (ImGui::InvisibleButton("##trash", ImVec2(h, h)) && ready && usable && !picker_open_) {
      selected_version_ = option.version;
      choice_ = GameDataChoice::kDelete;
    }
    const bool hot = ready && usable && !picker_open_ && (ImGui::IsItemHovered() || on_trash_);
    ImGui::PopID();
    const float a = alpha * (usable && !working_ && !busy_ ? 1.0f : 0.4f);
    const float rounding = 6.0f * unit;
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((hot ? 150 : 100) * a)), rounding);
    if (hot)
      draw->AddRectFilled(min, max, Color(kQuitRed, 0.3f * a), rounding);
    draw->AddRect(min, max, hot ? Color(kQuitRed, a) : IM_COL32(255, 255, 255, int(60 * a)), rounding,
                  0, std::max(1.0f, (hot ? 2.0f : 1.0f) * unit));
    const ImU32 ink = Color(kText, a * (hot ? 1.0f : 0.8f));
    const float thick = std::max(1.5f, 2.0f * unit);
    const float cx = (min.x + max.x) * 0.5f;
    const float top_y = min.y + h * 0.24f;
    const float lid_y = top_y + h * 0.08f;
    const float body_top = lid_y + h * 0.06f;
    const float body_bottom = min.y + h * 0.78f;
    // Handle, lid, then the tapered bin.
    draw->PathLineTo(ImVec2(cx - h * 0.07f, lid_y));
    draw->PathLineTo(ImVec2(cx - h * 0.07f, top_y));
    draw->PathLineTo(ImVec2(cx + h * 0.07f, top_y));
    draw->PathLineTo(ImVec2(cx + h * 0.07f, lid_y));
    draw->PathStroke(ink, 0, thick);
    draw->AddLine(ImVec2(cx - h * 0.26f, lid_y), ImVec2(cx + h * 0.26f, lid_y), ink, thick);
    draw->PathLineTo(ImVec2(cx - h * 0.19f, body_top));
    draw->PathLineTo(ImVec2(cx - h * 0.15f, body_bottom));
    draw->PathLineTo(ImVec2(cx + h * 0.15f, body_bottom));
    draw->PathLineTo(ImVec2(cx + h * 0.19f, body_top));
    draw->PathStroke(ink, 0, thick);
  }

  // The label, and under it what sets the version apart.
  void DrawVersionRow(ImDrawList* draw, ImFont* font, float unit, ImVec2 min, ImVec2 max,
                      const VersionRow& row, float a, float scroll_t) {
    const float pad = 16.0f * unit;
    const float label_size = 21.0f * unit;
    const float note_size = 15.0f * unit;
    const float h = max.y - min.y;
    const float avail = max.x - min.x - 2.0f * pad;
    draw->PushClipRect(ImVec2(min.x + pad, min.y), ImVec2(max.x - pad, max.y), true);
    if (row.note.empty()) {
      DrawScrolling(draw, font, label_size, min.x + pad,
                    std::floor(min.y + (h - label_size) * 0.5f), avail, unit, scroll_t, Color(kText, a),
                    row.label.c_str());
    } else {
      const float y = std::floor(min.y + (h - label_size - note_size - 2.0f * unit) * 0.5f);
      DrawScrolling(draw, font, label_size, min.x + pad, y, avail, unit, scroll_t, Color(kText, a),
                    row.label.c_str());
      DrawScrolling(draw, font, note_size, min.x + pad, y + label_size + 2.0f * unit, avail, unit, scroll_t,
                    Color(kText, 0.6f * a), row.note.c_str());
    }
    draw->PopClipRect();
  }

  // Text wider than its button slides to show the rest, pausing at both ends.
  static void DrawScrolling(ImDrawList* draw, ImFont* font, float text_size, float x, float y,
                            float avail, float unit, float scroll_t, ImU32 color,
                            const char* text) {
    const float width = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, text).x;
    float offset = 0.0f;
    if (width > avail && scroll_t >= 0.0f) {
      constexpr float kPause = 1.0f;
      const float travel = (width - avail) / (50.0f * unit);
      const float phase = std::fmod(scroll_t, 2.0f * (travel + kPause));
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
    draw->AddText(font, text_size, ImVec2(std::floor(x - offset), y), color, text);
  }

  // Button hints while a gamepad drives the screen; the file dialogs need a
  // mouse or keyboard.
  void DrawHints(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha) {
    for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; ++k) {
      if (ImGui::IsKeyPressed(ImGuiKey(k), false))
        gamepad_ = true;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
      if (ImGui::IsKeyPressed(ImGuiKey(k), false))
        gamepad_ = false;
    }
    if (MouseMoved() || ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      gamepad_ = false;
    if (!gamepad_)
      return;
    std::string text = std::string("A  ") + Tr(IntroText::kHintSelect) + "      B  " +
                       Tr(BackChoice() == GameDataChoice::kQuit ? IntroText::kHintQuit
                                                                : IntroText::kHintBack);
    if (Library() && CanDelete(selected_version_))
      text += std::string("      X  ") + Tr(IntroText::kDeleteVersion);
    if (!Library() && prompt_.confirm.empty() && prompt_.outdated.empty())
      text += std::string("\n") + Tr(IntroText::kDialogNeedsMouse);
    CenteredText(draw, font, 17.0f * unit, size.x * 0.5f, size.y - 112.0f * unit,
                 std::min(640.0f * unit, size.x - 32.0f), Color(kText, 0.7f * alpha), text);
  }

  static bool MouseMoved() {
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    return delta.x != 0.0f || delta.y != 0.0f;
  }

  // Bottom right: the current language, opening upward into the list.
  void DrawLanguagePicker(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha,
                          bool ready) {
    const int count = IntroLanguageCount();
    if (count < 2)
      return;
    const int current = IntroLanguage();
    auto pick = [&](int index) {
      SetIntroLanguage(index);
      picker_open_ = false;
    };

    if (ready && picker_open_) {
      if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp}))
        picker_highlight_ = std::max(0, picker_highlight_ - 1);
      else if (Pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown}))
        picker_highlight_ = std::min(count - 1, picker_highlight_ + 1);
      else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                        ImGuiKey_GamepadFaceDown}))
        pick(picker_highlight_);
      else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}))
        picker_open_ = false;
    } else if (ready && on_picker_) {
      if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp})) {
        on_picker_ = false;
      } else if (IntroMusicAvailable() &&
                 Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft})) {
        on_picker_ = false;
        on_music_ = true;
      } else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                          ImGuiKey_GamepadFaceDown})) {
        picker_open_ = true;
        picker_highlight_ = current;
      } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
        if (mods_open_) {
          mods_->Commit();
          mods_open_ = false;
        } else if (saves_open_) {
          saves_open_ = false;
        } else {
          choice_ = GameDataChoice::kQuit;
        }
        on_picker_ = false;
      }
    }

    const float w = 220.0f * unit;
    const float h = 40.0f * unit;
    const float margin = 24.0f * unit;
    const float rounding = 6.0f * unit;
    const float text_size = 20.0f * unit;
    const float pad = 14.0f * unit;
    const ImVec2 min(size.x - margin - w, size.y - margin - h);
    const ImVec2 max(min.x + w, min.y + h);
    const float a = alpha * (working_ || busy_ ? 0.4f : 1.0f);

    // A press outside the open list closes it.
    bool over_picker = false;
    ImGui::SetCursorScreenPos(min);
    if (ImGui::InvisibleButton("##language", ImVec2(w, h)) && ready) {
      picker_open_ = !picker_open_;
      picker_highlight_ = current;
      on_picker_ = true;
    }
    over_picker |= ImGui::IsItemHovered();
    if (ready && !picker_open_ && ImGui::IsItemHovered() && MouseMoved())
      on_picker_ = true;

    const float caption = 16.0f * unit;
    draw->AddText(font, caption, ImVec2(min.x, min.y - caption * 1.4f), Color(kText, 0.6f * a),
                  Tr(IntroText::kLanguage));
    const bool focused = on_picker_ || picker_open_;
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((focused ? 150 : 100) * a)), rounding);
    draw->AddRect(min, max, focused ? Color(kAccent, a) : IM_COL32(255, 255, 255, int(60 * a)),
                  rounding, 0, std::max(1.0f, (focused ? 2.0f : 1.0f) * unit));
    const char* name = IntroLanguageName(current);
    const float text_y = std::floor(min.y + (h - text_size) * 0.5f);
    draw->AddText(font, text_size, ImVec2(min.x + pad, text_y), Color(kText, a), name);
    // An arrow pointing the way the list opens.
    const float s = 5.0f * unit;
    const ImVec2 tip(max.x - pad - s, min.y + h * 0.5f - s * 0.5f);
    draw->AddTriangleFilled(ImVec2(tip.x - s, tip.y + s), ImVec2(tip.x + s, tip.y + s), tip,
                            Color(kText, 0.8f * a));

    if (picker_open_) {
      const float item_h = 36.0f * unit;
      const ImVec2 list_min(min.x, min.y - 4.0f * unit - item_h * float(count));
      const ImVec2 list_max(max.x, min.y - 4.0f * unit);
      draw->AddRectFilled(list_min, list_max, IM_COL32(22, 5, 9, int(235 * a)), rounding);
      draw->AddRect(list_min, list_max, Color(kAccent, a), rounding, 0, std::max(1.0f, unit));
      for (int i = 0; i < count; ++i) {
        const ImVec2 item_min(list_min.x, list_min.y + item_h * float(i));
        const ImVec2 item_max(list_max.x, item_min.y + item_h);
        ImGui::SetCursorScreenPos(item_min);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##language_item", ImVec2(w, item_h)) && ready)
          pick(i);
        if (ImGui::IsItemHovered()) {
          over_picker = true;
          if (MouseMoved())
            picker_highlight_ = i;
        }
        ImGui::PopID();
        if (i == picker_highlight_)
          draw->AddRectFilled(item_min, item_max, Color(kAccent, 0.45f * a), rounding);
        // Still selectable; the warning says why Start is off.
        const float shade = Supported(i) ? (i == current ? 1.0f : 0.8f) : 0.35f;
        draw->AddText(font, text_size,
                      ImVec2(item_min.x + pad, std::floor(item_min.y + (item_h - text_size) * 0.5f)),
                      Color(kText, shade * a), IntroLanguageName(i));
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !over_picker)
        picker_open_ = false;
    }
  }

  // Bottom left: a note icon that mutes the music, and the piece playing.
  // `keys` lets the cursor on it be moved and pressed here; the Mods page drives it itself.
  void DrawMusicToggle(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha,
                       bool focused, bool keys) {
    // No track ships yet; the toggle appears once one does.
    if (!IntroMusicAvailable()) {
      on_music_ = false;
      return;
    }
    if (keys && focused) {
      if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp})) {
        on_music_ = false;
      } else if (Pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight})) {
        on_music_ = false;
        on_picker_ = true;
      } else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                          ImGuiKey_GamepadFaceDown})) {
        SetIntroMusicEnabled(!IntroMusicEnabled());
      } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
        choice_ = GameDataChoice::kQuit;
      }
    }
    const float s = 40.0f * unit;
    const float margin = 24.0f * unit;
    const ImVec2 min(margin, size.y - margin - s);
    const ImVec2 max(min.x + s, min.y + s);
    ImGui::SetCursorScreenPos(min);
    if (ImGui::InvisibleButton("##music", ImVec2(s, s)))
      SetIntroMusicEnabled(!IntroMusicEnabled());
    const bool on = IntroMusicEnabled();
    const bool hovered = ImGui::IsItemHovered() || focused;
    const float a = alpha;
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((hovered ? 150 : 100) * a)), 6.0f * unit);
    draw->AddRect(min, max, hovered ? Color(kAccent, a) : IM_COL32(255, 255, 255, int(60 * a)),
                  6.0f * unit, 0, std::max(1.0f, (hovered ? 2.0f : 1.0f) * unit));

    const ImU32 ink = Color(kText, (on ? 0.9f : 0.55f) * a);
    // Two beamed eighth notes, symmetric about the button's center.
    const float u = s / 40.0f;
    const ImVec2 c(min.x + s * 0.5f, min.y + s * 0.5f + 1.4f * u);
    auto at = [&](float x, float y) { return ImVec2(c.x + x * u, c.y + y * u); };
    for (const float head : {-7.0f, 7.0f}) {
      draw->AddEllipseFilled(at(head, 7.0f), ImVec2(4.6f * u, 3.3f * u), ink, -0.45f);
      draw->AddLine(at(head + 3.5f, 6.0f), at(head + 3.5f, head < 0 ? -9.0f : -12.0f), ink,
                    std::max(1.5f, 2.0f * u));
    }
    const ImVec2 beam[4] = {at(-4.5f, -10.0f), at(11.5f, -13.5f), at(11.5f, -9.0f), at(-4.5f, -5.5f)};
    draw->AddConvexPolyFilled(beam, 4, ink);
    if (!on)
      draw->AddLine(at(-12.0f, 12.0f), at(12.0f, -14.0f), Color(kText, a), std::max(1.5f, 2.0f * u));

    // The credit slides out from behind the icon, and back in when muted.
    const float step = ImGui::GetIO().DeltaTime / 0.7f;
    credit_ = on ? std::min(1.0f, credit_ + step) : std::max(0.0f, credit_ - step);
    const float shown = Smooth(credit_);
    const float text_size = 18.0f * unit;
    const char* credit = "Nocturne Op. 9, No. 2, performed by Aya Higuchi";
    const float text_x = max.x + 14.0f * unit;
    const ImVec2 extent = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, credit);
    draw->PushClipRect(ImVec2(text_x, min.y), ImVec2(size.x, max.y), true);
    const int first = draw->VtxBuffer.Size;
    draw->AddText(font, text_size,
                  ImVec2(std::floor(text_x - (1.0f - shown) * (extent.x + 14.0f * unit)),
                         std::floor(min.y + (s - text_size) * 0.5f)),
                  Color(kText, 0.75f * a), credit);
    // The left edge fades into the icon while sliding, and is solid at rest.
    const float edge = std::max(1.0f, 48.0f * unit * (1.0f - shown));
    for (int i = first; shown < 1.0f && i < draw->VtxBuffer.Size; ++i) {
      ImDrawVert& v = draw->VtxBuffer[i];
      const float k = std::clamp((v.pos.x - text_x) / edge, 0.0f, 1.0f);
      v.col = (v.col & 0x00FFFFFF) | (ImU32(float(v.col >> 24) * k) << 24);
    }
    draw->PopClipRect();
  }

  // The reason Start is off, to the right of the Start button whose top right
  // corner is `start_max`.
  void DrawLanguageWarning(ImDrawList* draw, ImFont* font, ImVec2 size, float unit,
                           ImVec2 start_max, float h, float a, const std::string& release) {
    DrawWarning(draw, font, size, unit, start_max, h, a, Tr(IntroText::kUnsupportedLanguage, release));
  }

  void DrawWarning(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, ImVec2 start_max,
                   float h, float a, const std::string& text) {
    const float text_size = 18.0f * unit;
    const float text_x = start_max.x + 16.0f * unit;
    const float wrap = std::max(size.x - text_x - 16.0f * unit, 120.0f * unit);
    const ImVec2 box = font->CalcTextSizeA(text_size, FLT_MAX, wrap, text.c_str());
    draw->AddText(font, text_size,
                  ImVec2(text_x, std::floor(start_max.y + (h - box.y) * 0.5f)),
                  Color(kText, a), text.c_str(), nullptr, wrap);
  }

  GameDataPrompt prompt_;
  std::vector<Option> options_;
  int selected_ = 0;
  // The version Delete applies to, and the version list's first shown row.
  int selected_version_ = -1;
  bool gamepad_ = false;
  // The language picker has the keys instead of selected_.
  bool on_picker_ = false;
  // The music toggle has the keys instead of selected_.
  bool on_music_ = false;
  // The cursor is on the trash can beside the picked version.
  bool on_trash_ = false;
  bool picker_open_ = false;
  int picker_highlight_ = 0;
  bool busy_ = false;
  bool working_ = false;
  float credit_ = 0.0f;
  std::array<PhaseState, kInstallPhaseCount> phases_{};
  std::string work_title_;
  std::string work_detail_;
  float work_fraction_ = -1.0f;
  std::optional<GameDataChoice> choice_;
  std::unique_ptr<ModsScreen> mods_;
  bool mods_open_ = false;
  std::unique_ptr<SavesScreen> saves_;
  bool saves_open_ = false;
  bool mods_was_open_ = false;
  // Why enabled mods stop the game starting; empty when none do.
  std::string mod_block_;
  bool mod_block_known_ = false;
};

std::unique_ptr<IntroDialog> g_dialog;
std::function<void(GameDataChoice, int)> g_on_choice;

// The main loop sleeps until an event arrives, so frames are posted to it.
// One at a time: a busy UI thread (extraction) must not pile them up.
std::thread g_ticker;
std::atomic<bool> g_ticking{false};
std::atomic<bool> g_frame_queued{false};

void Answer(GameDataChoice choice, int version) {
  if (auto on_choice = g_on_choice)
    on_choice(choice, version);
}

void PresentFrame() {
  if (g_dialog) {
    PlumePresentOverlayOnly();
    // Taken after the frame, so answering can destroy the dialog safely.
    if (g_dialog) {
      if (const auto choice = g_dialog->TakeChoice())
        Answer(choice->first, choice->second);
    }
  }
  // Cleared last: queued while this runs, the next frame keeps the UI thread
  // from ever returning to SDL's event pump, which freezes Wayland input.
  g_frame_queued = false;
}

void StartTicker() {
  if (g_ticking.exchange(true))
    return;
  g_ticker = std::thread([] {
    while (g_ticking.load(std::memory_order_acquire)) {
      if (!g_frame_queued.exchange(true))
        g_context->CallInUIThreadDeferred(PresentFrame);
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
  });
}

void StopTicker() {
  if (!g_ticking.exchange(false))
    return;
  g_ticker.join();
}

}  // namespace

void BindIntroScreen(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context,
                     const std::filesystem::path& user_data_root) {
  g_drawer = drawer;
  g_context = context;
  g_user_data_root = user_data_root;
}

bool IntroScreenAvailable() {
  return g_drawer && g_context && PlumeBackendReady();
}

void ShowIntroScreen(const GameDataPrompt& prompt,
                     std::function<void(GameDataChoice, int)> on_choice) {
  g_on_choice = std::move(on_choice);
  if (g_dialog) {
    g_dialog->Update(prompt);
    g_dialog->SetBusy(false);
    return;
  }
  if (!g_first_shown)
    g_first_shown = Clock::now();
  g_dialog = std::make_unique<IntroDialog>(g_drawer, prompt);
  StartUpdateCheck();
  StartTicker();
  StartIntroMusic();
}

bool ShowIntroWork() {
  if (!IntroScreenAvailable())
    return false;
  if (!g_dialog) {
    if (!g_first_shown)
      g_first_shown = Clock::now();
    g_dialog = std::make_unique<IntroDialog>(g_drawer, GameDataPrompt{});
    StartTicker();
  }
  g_dialog->SetWorking(true);
  g_dialog->SetPhase(Phase::kExtract, PhaseState::kSkipped);
  return true;
}

void SetIntroBusy(bool busy) {
  if (g_dialog)
    g_dialog->SetBusy(busy);
}

void BeginIntroWork() {
  if (g_dialog)
    g_dialog->SetWorking(true);
}

void EndIntroWork() {
  if (g_dialog)
    g_dialog->SetWorking(false);
}

void SetIntroPhase(Phase phase, PhaseState state) {
  if (g_dialog)
    g_dialog->SetPhase(phase, state);
}

void ReportIntroProgress(const std::string& title, float fraction, const std::string& detail) {
  if (!g_dialog)
    return;
  g_dialog->SetProgress(title, fraction, detail);
  static Clock::time_point last;
  const auto now = Clock::now();
  if (now - last < std::chrono::milliseconds(33))
    return;
  last = now;
  // Without this the OS marks the window unresponsive, Android never learns the
  // surface went away, and the screen never follows a resize.
  g_context->ProcessPendingWindowEvents();
  PlumePresentOverlayOnly();
}

void HideIntroScreen() {
  StopTicker();
  StopIntroMusic();
  if (!g_dialog)
    return;
  g_dialog.reset();
  g_on_choice = nullptr;
  // One more frame, so the intro is gone behind whatever comes next.
  PlumePresentOverlayOnly();
}

bool RequestIntroQuit() {
  if (!g_dialog)
    return false;
  // Deferred: this runs inside the window's own event handling.
  g_context->CallInUIThreadDeferred([] { Answer(GameDataChoice::kQuit, -1); });
  return true;
}

void ReleaseIntroScreen() {
  g_background = {};
  g_logo = {};
}

}  // namespace nocturne
