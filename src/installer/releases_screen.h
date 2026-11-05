#pragma once

// The start screen's Releases page, opened from the version label: every
// GitHub release on the left, the selected one's notes on the right. Drawn in
// the main page's style and driven by keyboard, gamepad, mouse or touch, like
// the Saves page.

#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

namespace nocturne {

class ReleasesScreen {
 public:
  ReleasesScreen();

  // Fetches the list again if the last attempt failed.
  void Reopen(bool gamepad);

  // The cursor is on the intro's music toggle or language picker, which the intro draws.
  bool MusicFocused() const { return zone_ == Zone::kMusic; }
  bool PickerFocused() const { return zone_ == Zone::kPicker; }
  void FocusPicker() { zone_ = Zone::kPicker; }
  void FocusMusic() { zone_ = Zone::kMusic; }
  void ReturnFocus() { zone_ = Zone::kList; }

  // Draws into the current window and returns whether Back was chosen.
  bool Draw(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha, bool ready);

  struct Span {
    std::string text;
    bool bold = false;
    bool code = false;
    std::string url;
  };
  // One Markdown line: a heading (level 1..3), a bullet, a paragraph line or a gap.
  struct Block {
    int heading = 0;
    bool bullet = false;
    bool gap = false;
    int indent = 0;
    std::vector<Span> spans;
  };
  struct Release {
    std::string tag;
    std::string name;
    std::string date;
    bool prerelease = false;
    std::vector<Block> blocks;
  };
  struct Fetch;

 private:
  enum class Zone { kBack, kList, kContent, kMusic, kPicker };

  // A laid out word of the notes, relative to the content's top left.
  struct Word {
    ImVec2 pos;
    float size;
    ImVec4 color;
    bool bold;
    std::string text;
    std::string url;
  };
  // A bullet dot, or a heading's rule when `rule` is set.
  struct Mark {
    ImVec2 pos;
    float width;
    bool rule;
  };

  void Input(int count);
  void Pointer(const std::vector<Release>* releases);
  void Select(int index);
  float Flow(const std::vector<Span>& spans, float left, float right, float y, float size,
             const ImVec4& color, bool bold);
  void Layout(const Release& release, float width);
  void DrawList(const std::vector<Release>& releases);
  void DrawContent(const std::vector<Release>* releases, const char* message);
  void DrawHints(float y);
  void ScrollBar(ImVec2 min, ImVec2 max, float scroll, float content);

  std::shared_ptr<Fetch> fetch_;
  ImVec2 list_min_{0, 0}, list_max_{0, 0};
  ImVec2 content_min_{0, 0}, content_max_{0, 0};

  // Per frame.
  ImDrawList* draw_ = nullptr;
  ImFont* font_ = nullptr;
  ImVec2 size_{0, 0};
  float unit_ = 1.0f;
  float alpha_ = 1.0f;
  bool ready_ = false;
  bool moved_ = false;

  Zone zone_ = Zone::kList;
  bool back_ = false;
  bool gamepad_ = false;
  // The last pointer input was a finger, which has no hover.
  bool touch_ = false;
  int selected_ = -1;
  // The list row under a moving mouse; -1 for none.
  int hovered_ = -1;

  float list_scroll_ = 0.0f;
  float list_height_ = 0.0f;
  float list_view_ = 0.0f;
  float row_h_ = 0.0f;
  // Eased toward by the keys; drag and wheel move both.
  float content_scroll_ = 0.0f;
  float content_target_ = 0.0f;
  float content_height_ = 0.0f;
  float content_view_ = 0.0f;

  // 1 for the list, 2 for the notes, while a press that began over one is held.
  int dragging_ = 0;
  bool dragged_ = false;

  int laid_out_ = -1;
  float laid_width_ = 0.0f;
  float laid_unit_ = 0.0f;
  std::vector<Word> words_;
  std::vector<Mark> marks_;
};

}  // namespace nocturne
