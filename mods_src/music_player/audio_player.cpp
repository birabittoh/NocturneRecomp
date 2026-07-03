#include "audio_player.h"

#include <string>
#include <string_view>

#include <imgui.h>

#include "track_names.h"

#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace nocturne {
namespace {

using XmpApp = rex::kernel::xam::apps::XmpApp;

XmpApp* GetXmpApp() {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->app_manager()) {
    return nullptr;
  }
  return static_cast<XmpApp*>(ks->app_manager()->FindApp(0xFA));
}

std::string FilenameFromPath(const std::u16string& path) {
  std::string utf8 = rex::string::to_utf8(path);
  auto slash = utf8.find_last_of("/\\");
  if (slash != std::string::npos) {
    utf8 = utf8.substr(slash + 1);
  }
  auto dot = utf8.find_last_of('.');
  if (dot != std::string::npos) {
    utf8 = utf8.substr(0, dot);
  }
  return utf8;
}

// A "group" is either a lone track or a primary+companion pair that together
// represent one musical track (e.g. shiro1 + shiro2 → "shiro").
struct TrackGroup {
  int primary_index;    // index into known_songs_
  int companion_index;  // index into known_songs_, or -1
};

// Builds the flat known_songs list into logical groups. Companion tracks
// (those that are the "…2.wma" of some "…1.wma" primary) are absorbed into
// their primary's group and do not appear as separate rows.
std::vector<TrackGroup> BuildGroups(XmpApp* xmp) {
  const auto& songs = xmp->known_songs();
  const int n = static_cast<int>(songs.size());

  // For each index, record whether it is someone else's companion.
  std::vector<bool> absorbed(n, false);
  std::vector<int> companion(n, -1);
  for (int i = 0; i < n; ++i) {
    int ci = xmp->CompanionIndexOf(static_cast<size_t>(i));
    companion[i] = ci;
    if (ci >= 0) {
      absorbed[ci] = true;
    }
  }

  std::vector<TrackGroup> groups;
  groups.reserve(n);
  for (int i = 0; i < n; ++i) {
    if (!absorbed[i]) {
      groups.push_back({i, companion[i]});
    }
  }
  return groups;
}

// Returns the display label for a group. When a companion is present, strip
// the trailing "1" from the primary's filename (shiro1 → shiro).
std::string GroupLabel(const XmpApp* xmp, const TrackGroup& g) {
  const auto& song = xmp->known_songs()[g.primary_index];
  std::string label;
  if (!song.name.empty()) {
    label = rex::string::to_utf8(song.name);
  } else {
    label = FilenameFromPath(song.file_path);
  }
  if (g.companion_index >= 0 && !label.empty() && label.back() == '1') {
    label.pop_back();
  }
  std::string_view pretty = PrettyTrackName(label);
  if (!pretty.empty()) {
    label += " (";
    label.append(pretty);
    label += ")";
  }
  return label;
}

// Small vector-drawn transport icons, so the player's controls read as real
// icons instead of ASCII-art text glued to a button label.
namespace icons {

constexpr float kButtonSize = 22.0f;

void DrawPrev(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float s = (p1.y - p0.y) * 0.26f;
  constexpr float kBarW = 2.2f;
  float tri_w = s * 1.6f;
  float half = (kBarW + tri_w) * 0.5f;
  float bar_left = c.x - half;
  float bar_right = bar_left + kBarW;
  dl->AddRectFilled(ImVec2(bar_left, c.y - s), ImVec2(bar_right, c.y + s), col);
  dl->AddTriangleFilled(ImVec2(bar_right + tri_w, c.y - s), ImVec2(bar_right + tri_w, c.y + s),
                        ImVec2(bar_right, c.y), col);
}

void DrawNext(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float s = (p1.y - p0.y) * 0.26f;
  constexpr float kBarW = 2.2f;
  float tri_w = s * 1.6f;
  float half = (kBarW + tri_w) * 0.5f;
  float bar_right = c.x + half;
  float bar_left = bar_right - kBarW;
  dl->AddRectFilled(ImVec2(bar_left, c.y - s), ImVec2(bar_right, c.y + s), col);
  dl->AddTriangleFilled(ImVec2(bar_left - tri_w, c.y - s), ImVec2(bar_left - tri_w, c.y + s),
                        ImVec2(bar_left, c.y), col);
}

void DrawPlay(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float s = (p1.y - p0.y) * 0.28f;
  dl->AddTriangleFilled(ImVec2(c.x - s * 0.6f, c.y - s), ImVec2(c.x - s * 0.6f, c.y + s),
                        ImVec2(c.x + s * 0.9f, c.y), col);
}

void DrawPause(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float h = (p1.y - p0.y) * 0.28f;
  constexpr float kBarW = 3.0f, kGap = 4.0f;
  dl->AddRectFilled(ImVec2(c.x - kGap * 0.5f - kBarW, c.y - h), ImVec2(c.x - kGap * 0.5f, c.y + h),
                    col);
  dl->AddRectFilled(ImVec2(c.x + kGap * 0.5f, c.y - h), ImVec2(c.x + kGap * 0.5f + kBarW, c.y + h),
                    col);
}

void DrawStop(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float s = (p1.y - p0.y) * 0.22f;
  dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col);
}

// Padlock. Drawn closed (shackle centered over the body) or open (shackle
// swung off to the side), matching the usual "song lock" UI convention.
void DrawLock(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, bool locked) {
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float body_w = (p1.x - p0.x) * 0.30f;
  float body_h = (p1.y - p0.y) * 0.34f;
  ImVec2 body_min(c.x - body_w, c.y - body_h * 0.1f);
  ImVec2 body_max(c.x + body_w, c.y + body_h);
  dl->AddRectFilled(body_min, body_max, col, 1.5f);

  float shackle_r = body_w * 0.85f;
  float thickness = 2.0f;
  ImVec2 hinge = locked ? ImVec2(c.x, body_min.y) : ImVec2(c.x + shackle_r * 0.5f, body_min.y);
  constexpr float kPi = 3.14159265f;
  dl->PathArcTo(hinge, shackle_r, kPi, 2.0f * kPi, 16);
  dl->PathStroke(col, 0, thickness);
}

}  // namespace icons

// A fixed-size button that paints a vector icon (from |icons|) instead of a
// text label. |active| draws it in the "pressed/toggled" button color.
template <typename DrawIconFn>
bool IconButton(const char* str_id, DrawIconFn&& draw_icon, bool active = false) {
  ImGui::PushID(str_id);
  ImVec2 size(icons::kButtonSize, icons::kButtonSize);
  bool pressed = ImGui::InvisibleButton(str_id, size);
  bool hovered = ImGui::IsItemHovered();
  bool held = ImGui::IsItemActive();

  ImGuiCol bg = active || held ? ImGuiCol_ButtonActive
                : hovered      ? ImGuiCol_ButtonHovered
                               : ImGuiCol_Button;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetItemRectMin();
  ImVec2 p1 = ImGui::GetItemRectMax();
  dl->AddRectFilled(p0, p1, ImGui::GetColorU32(bg), ImGui::GetStyle().FrameRounding);
  draw_icon(dl, p0, p1, ImGui::GetColorU32(ImGuiCol_Text));

  ImGui::PopID();
  return pressed;
}

}  // namespace

class AudioPlayerDialog : public rex::ui::ImGuiDialog {
 public:
  explicit AudioPlayerDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    rex::ui::RegisterBind("bind_audio_player", "F8", "Toggle audio player",
                          [this] { visible_ = !visible_; });
  }

  ~AudioPlayerDialog() override { rex::ui::UnregisterBind("bind_audio_player"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Player", &visible_)) {
      ImGui::End();
      return;
    }

    XmpApp* xmp = GetXmpApp();
    if (!xmp) {
      ImGui::TextUnformatted("Audio system not available.");
      ImGui::End();
      return;
    }

    const auto& songs = xmp->known_songs();
    if (songs.empty()) {
      ImGui::TextUnformatted("No tracks discovered yet.");
      ImGui::End();
      return;
    }

    // Build grouped view. current_group is the index of the group containing
    // the playing track (primary or companion).
    auto groups = BuildGroups(xmp);
    const int num_groups = static_cast<int>(groups.size());
    const int playing_ki = xmp->playing_known_index();

    int current_group = -1;
    for (int g = 0; g < num_groups; ++g) {
      if (groups[g].primary_index == playing_ki ||
          groups[g].companion_index == playing_ki) {
        current_group = g;
        break;
      }
    }

    bool is_playing = xmp->state() == XmpApp::State::kPlaying;
    bool is_paused = xmp->state() == XmpApp::State::kPaused;

    if (current_group >= 0) {
      std::string now = GroupLabel(xmp, groups[current_group]);
      ImGui::Text("%s: %s", is_playing ? "Playing" : "Paused", now.c_str());
    } else {
      ImGui::TextUnformatted("Stopped");
    }

    // Skip to previous group.
    if (IconButton("##prev", icons::DrawPrev)) {
      int prev = current_group > 0 ? current_group - 1 : num_groups - 1;
      xmp->PlayKnownSong(groups[prev].primary_index);
    }
    ImGui::SameLine();
    if (is_playing) {
      if (IconButton("##playpause", icons::DrawPause)) {
        xmp->XMPPause();
      }
    } else {
      if (IconButton("##playpause", icons::DrawPlay)) {
        if (is_paused) {
          xmp->XMPContinue();
        } else if (num_groups > 0) {
          int g = current_group >= 0 ? current_group : 0;
          xmp->PlayKnownSong(groups[g].primary_index);
        }
      }
    }
    ImGui::SameLine();
    // Skip to next group.
    if (IconButton("##next", icons::DrawNext)) {
      int next = current_group >= 0 ? (current_group + 1) % num_groups : 0;
      xmp->PlayKnownSong(groups[next].primary_index);
    }
    ImGui::SameLine();
    if (IconButton("##stop", icons::DrawStop)) {
      xmp->XMPStop(0, /*user_initiated=*/true);
    }
    ImGui::SameLine();
    // Lock toggle: while locked, the game can't switch tracks on its own
    // (e.g. on a room transition) and its volume (e.g. a room fade or a
    // settings-menu slider) -- only an explicit pick/adjustment from this
    // player (or the transport buttons above) changes them.
    bool locked = xmp->locked();
    if (IconButton("##lock", [locked](ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
          icons::DrawLock(dl, p0, p1, col, locked);
        }, locked)) {
      xmp->SetLocked(!locked);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(locked ? "Locked" : "Unlocked");
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("Vol.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    float vol = xmp->audible_volume();
    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp)) {
      xmp->SetVolume(vol);
    }

    ImGui::Separator();

    ImGui::BeginChild("##tracklist", ImVec2(0, 0), true);
    for (int g = 0; g < num_groups; ++g) {
      const TrackGroup& group = groups[g];
      std::string label = GroupLabel(xmp, group);

      bool is_current = (g == current_group) && (is_playing || is_paused);
      if (is_current) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.2f, 1.0f));
      }

      char buf[512];
      snprintf(buf, sizeof(buf), "%d. %s", g + 1, label.c_str());
      if (ImGui::Selectable(buf, is_current)) {
        xmp->PlayKnownSong(group.primary_index);
      }

      if (is_current) {
        ImGui::PopStyleColor();
      }
    }
    ImGui::EndChild();

    ImGui::End();
  }

 private:
  bool visible_ = false;
};

AudioPlayer::AudioPlayer() = default;
AudioPlayer::~AudioPlayer() = default;

AudioPlayer& GetAudioPlayer() {
  static AudioPlayer instance;
  return instance;
}

void AudioPlayer::Bind(rex::ui::Window* window, rex::ui::WindowedAppContext* context) {
  window_ = window;
  context_ = context;
}

void AudioPlayer::AttachDialog(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !dialog_) {
    dialog_ = std::make_unique<AudioPlayerDialog>(drawer);
  }
}

}  // namespace nocturne
