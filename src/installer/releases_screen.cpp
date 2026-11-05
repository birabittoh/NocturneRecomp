#include "releases_screen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

#include <SDL3/SDL_misc.h>

#include <rex/net/http.h>

#include "gen/version.generated.h"
#include "intro_music.h"
#include "intro_text.h"
#include "ui/ui_text.h"

namespace nocturne {

struct ReleasesScreen::Fetch {
  std::mutex mutex;
  bool done = false;
  bool failed = false;
  std::vector<Release> releases;
};

namespace {

using Release = ReleasesScreen::Release;
using Block = ReleasesScreen::Block;
using Span = ReleasesScreen::Span;

constexpr ImVec4 kAccent(0x3A / 255.0f, 0xAA / 255.0f, 0xDC / 255.0f, 1.0f);
constexpr ImVec4 kText(0.96f, 0.93f, 0.88f, 1.0f);
constexpr ImVec4 kBright(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kMuted(0.60f, 0.62f, 0.66f, 1.0f);
constexpr ImVec4 kHeader(0.60f, 0.85f, 1.00f, 1.0f);
constexpr ImVec4 kCode(1.00f, 0.86f, 0.62f, 1.0f);
constexpr ImVec4 kError(1.00f, 0.35f, 0.35f, 1.0f);
constexpr ImVec4 kGood(0.45f, 0.85f, 0.55f, 1.0f);
constexpr ImVec4 kGold(1.00f, 0.80f, 0.30f, 1.0f);

constexpr const char* kReleasesUrl =
    "https://api.github.com/repos/birabittoh/NocturneRecomp/releases?per_page=100&page=";

// Kept for the whole run: GitHub allows few unauthenticated requests an hour.
std::shared_ptr<ReleasesScreen::Fetch> g_fetch;

ImU32 Col(const ImVec4& c, float a = 1.0f) {
  return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a));
}

bool Pressed(std::initializer_list<ImGuiKey> keys) {
  // Alt+Enter toggles fullscreen.
  if (ImGui::GetIO().KeyAlt)
    return false;
  return std::any_of(keys.begin(), keys.end(), [](ImGuiKey k) { return ImGui::IsKeyPressed(k); });
}

const char* S(const char* key) {
  return Tr("releases", key);
}

bool Inside(ImVec2 p, ImVec2 min, ImVec2 max) {
  return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y;
}

void AppendUtf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  } else {
    out += char(0xF0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3F));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}

// Just enough JSON for the releases list: strings are decoded, the rest skipped.
struct Json {
  const std::string& s;
  size_t i = 0;

  void Ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
      ++i;
  }
  bool Eat(char c) {
    Ws();
    if (i < s.size() && s[i] == c) {
      ++i;
      return true;
    }
    return false;
  }
  bool Starts(const char* word) {
    Ws();
    return s.compare(i, std::char_traits<char>::length(word), word) == 0;
  }
  uint32_t Hex4() {
    uint32_t v = 0;
    for (int n = 0; n < 4 && i < s.size(); ++n, ++i) {
      const char c = s[i];
      v = v * 16 + uint32_t(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return v;
  }
  bool String(std::string& out) {
    out.clear();
    if (!Eat('"'))
      return false;
    while (i < s.size()) {
      const char c = s[i++];
      if (c == '"')
        return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size())
        return false;
      switch (const char e = s[i++]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          uint32_t c16 = Hex4();
          if (c16 >= 0xD800 && c16 < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
            i += 2;
            c16 = 0x10000 + ((c16 - 0xD800) << 10) + (Hex4() - 0xDC00);
          }
          AppendUtf8(out, c16);
          break;
        }
        default: out += e; break;
      }
    }
    return false;
  }
  bool Skip() {
    Ws();
    if (i >= s.size())
      return false;
    std::string ignored;
    if (s[i] == '"')
      return String(ignored);
    const char open = s[i];
    if (open == '{' || open == '[') {
      ++i;
      const char close = open == '{' ? '}' : ']';
      if (Eat(close))
        return true;
      do {
        if (open == '{' && (!String(ignored) || !Eat(':')))
          return false;
        if (!Skip())
          return false;
      } while (Eat(','));
      return Eat(close);
    }
    const size_t start = i;
    while (i < s.size() && !std::strchr(",}] \n\r\t", s[i]))
      ++i;
    return i > start;
  }
};

// Bold, inline code, [links](url) and <url>; the rest is literal.
std::vector<Span> ParseInline(const std::string& text) {
  std::vector<Span> spans;
  Span current;
  auto flush = [&] {
    if (!current.text.empty())
      spans.push_back(current);
    current.text.clear();
    current.url.clear();
  };
  for (size_t i = 0; i < text.size();) {
    if (!current.code && (text.compare(i, 2, "**") == 0 || text.compare(i, 2, "__") == 0)) {
      flush();
      current.bold = !current.bold;
      i += 2;
      continue;
    }
    if (text[i] == '`') {
      flush();
      current.code = !current.code;
      ++i;
      continue;
    }
    if (!current.code && text[i] == '[') {
      const size_t close = text.find("](", i);
      const size_t end = close == std::string::npos ? close : text.find(')', close);
      if (end != std::string::npos) {
        flush();
        Span link = current;
        link.text = text.substr(i + 1, close - i - 1);
        for (size_t at; (at = link.text.find("**")) != std::string::npos;)
          link.text.erase(at, 2);
        link.url = text.substr(close + 2, end - close - 2);
        spans.push_back(link);
        i = end + 1;
        continue;
      }
    }
    if (!current.code && text.compare(i, 5, "<http") == 0) {
      const size_t end = text.find('>', i);
      if (end != std::string::npos) {
        flush();
        Span link = current;
        link.text = link.url = text.substr(i + 1, end - i - 1);
        spans.push_back(link);
        i = end + 1;
        continue;
      }
    }
    current.text += text[i++];
  }
  flush();
  return spans;
}

bool IsStoreHeading(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return text.find("get the game on") != std::string::npos;
}

std::vector<Block> ParseMarkdown(const std::string& markdown) {
  std::vector<Block> blocks;
  auto gap = [&] {
    if (!blocks.empty() && !blocks.back().gap)
      blocks.push_back({0, false, true});
  };
  size_t at = 0;
  while (at <= markdown.size()) {
    size_t end = markdown.find('\n', at);
    if (end == std::string::npos)
      end = markdown.size();
    std::string line = markdown.substr(at, end - at);
    at = end + 1;
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
    int columns = 0;
    size_t first = 0;
    for (; first < line.size() && (line[first] == ' ' || line[first] == '\t'); ++first)
      columns += line[first] == '\t' ? 4 : 1;
    line.erase(0, first);
    while (!line.empty() && line.back() == ' ')
      line.pop_back();
    if (line.empty() || line == "---" || line == "***" || line.rfind("<!--", 0) == 0) {
      gap();
      continue;
    }
    Block block;
    if (line[0] == '#') {
      const size_t level = line.find_first_not_of('#');
      if (level != std::string::npos && line[level] == ' ') {
        if (IsStoreHeading(line))
          continue;
        block.heading = int(std::min<size_t>(level, 3));
        line.erase(0, level + 1);
      }
    } else if (line.size() > 1 && (line[0] == '*' || line[0] == '-' || line[0] == '+') &&
               line[1] == ' ') {
      block.bullet = true;
      block.indent = columns / 2;
      line.erase(0, 2);
    } else {
      block.indent = columns / 2;
    }
    block.spans = ParseInline(line);
    blocks.push_back(std::move(block));
  }
  while (!blocks.empty() && blocks.back().gap)
    blocks.pop_back();
  return blocks;
}

bool Literal(Json& json, const char* word) {
  const bool match = json.Starts(word);
  json.Skip();
  return match;
}

// One page of the releases list, appended; false when it does not parse.
bool ParsePage(const std::string& body, std::vector<Release>& out, size_t& count) {
  Json json{body};
  count = 0;
  if (!json.Eat('['))
    return false;
  if (json.Eat(']'))
    return true;
  do {
    if (!json.Eat('{'))
      return false;
    Release release;
    std::string markdown;
    bool draft = false;
    if (!json.Eat('}')) {
      do {
        std::string key;
        if (!json.String(key) || !json.Eat(':'))
          return false;
        std::string* target = key == "tag_name"       ? &release.tag
                              : key == "name"         ? &release.name
                              : key == "published_at" ? &release.date
                              : key == "body"         ? &markdown
                                                      : nullptr;
        if (target && json.Starts("\"")) {
          if (!json.String(*target))
            return false;
        } else if (key == "prerelease") {
          release.prerelease = Literal(json, "true");
        } else if (key == "draft") {
          draft = Literal(json, "true");
        } else if (!json.Skip()) {
          return false;
        }
      } while (json.Eat(','));
      if (!json.Eat('}'))
        return false;
    }
    ++count;
    if (draft || release.tag.empty())
      continue;
    release.date = release.date.substr(0, 10);
    release.blocks = ParseMarkdown(markdown);
    out.push_back(std::move(release));
  } while (json.Eat(','));
  return json.Eat(']');
}

void StartFetch(std::shared_ptr<ReleasesScreen::Fetch> fetch) {
  std::thread([fetch] {
    std::vector<Release> releases;
    bool ok = true;
    for (int page = 1; page <= 10; ++page) {
      const auto response = rex::net::HttpGet(kReleasesUrl + std::to_string(page));
      size_t count = 0;
      if (!response.ok() || !ParsePage(response.body, releases, count)) {
        ok = page > 1;
        break;
      }
      if (count < 100)
        break;
    }
    std::stable_sort(releases.begin(), releases.end(),
                     [](const Release& a, const Release& b) { return a.date > b.date; });
    std::lock_guard<std::mutex> lock(fetch->mutex);
    fetch->releases = std::move(releases);
    fetch->failed = !ok;
    fetch->done = true;
  }).detach();
}

std::string Bare(std::string tag) {
  if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
    tag.erase(0, 1);
  return tag;
}

}  // namespace

ReleasesScreen::ReleasesScreen() = default;

void ReleasesScreen::Reopen(bool gamepad) {
  gamepad_ = gamepad;
  zone_ = Zone::kList;
  dragging_ = 0;
  if (g_fetch) {
    std::lock_guard<std::mutex> lock(g_fetch->mutex);
    if (g_fetch->done && g_fetch->failed)
      g_fetch.reset();
  }
  if (!g_fetch) {
    g_fetch = std::make_shared<Fetch>();
    StartFetch(g_fetch);
    selected_ = -1;
  }
  fetch_ = g_fetch;
}

void ReleasesScreen::Select(int index) {
  if (index == selected_)
    return;
  selected_ = index;
  content_scroll_ = content_target_ = 0.0f;
  const float top = float(index) * row_h_;
  if (top < list_scroll_)
    list_scroll_ = top;
  else if (top + row_h_ > list_scroll_ + list_view_)
    list_scroll_ = top + row_h_ - list_view_;
}

void ReleasesScreen::Input(int count) {
  for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; ++k) {
    if (ImGui::IsKeyPressed(ImGuiKey(k), false))
      gamepad_ = true;
  }
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
    if (ImGui::IsKeyPressed(ImGuiKey(k), false))
      gamepad_ = false;
  }
  const ImGuiIO& io = ImGui::GetIO();
  if (moved_ || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    gamepad_ = false;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      touch_ = io.MouseSource == ImGuiMouseSource_TouchScreen;
    else if (io.MouseSource != ImGuiMouseSource_TouchScreen)
      touch_ = false;
  }
  for (int k = ImGuiKey_NamedKey_BEGIN; k <= ImGuiKey_GamepadRStickDown; ++k) {
    if (ImGui::IsKeyPressed(ImGuiKey(k), false))
      touch_ = false;
  }
  // The intro's picker has the keys while the cursor is on it.
  if (!ready_ || zone_ == Zone::kPicker)
    return;
  const float step = 60.0f * unit_;
  if (Pressed({ImGuiKey_PageDown, ImGuiKey_GamepadR1})) {
    content_target_ += content_view_ * 0.8f;
  } else if (Pressed({ImGuiKey_PageUp, ImGuiKey_GamepadL1})) {
    content_target_ -= content_view_ * 0.8f;
  } else if (Pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown})) {
    if (zone_ == Zone::kBack)
      zone_ = Zone::kList;
    else if (zone_ == Zone::kContent)
      content_target_ += step;
    else if (zone_ == Zone::kList && selected_ + 1 < count)
      Select(selected_ + 1);
    else if (zone_ == Zone::kList)
      zone_ = Zone::kMusic;
  } else if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp})) {
    if (zone_ == Zone::kContent)
      content_target_ -= step;
    else if (zone_ == Zone::kList && selected_ > 0)
      Select(selected_ - 1);
    else if (zone_ == Zone::kList)
      zone_ = Zone::kBack;
    else if (zone_ == Zone::kMusic)
      zone_ = Zone::kList;
  } else if (Pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight})) {
    if (zone_ == Zone::kList)
      zone_ = Zone::kContent;
    else if (zone_ == Zone::kMusic && IntroLanguageCount() >= 2)
      zone_ = Zone::kPicker;
  } else if (Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft})) {
    if (zone_ == Zone::kContent)
      zone_ = Zone::kList;
  } else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                      ImGuiKey_GamepadFaceDown})) {
    if (zone_ == Zone::kBack)
      back_ = true;
    else if (zone_ == Zone::kList)
      zone_ = Zone::kContent;
    else if (zone_ == Zone::kMusic)
      SetIntroMusicEnabled(!IntroMusicEnabled());
  } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
    back_ = true;
  }
}

// Wheel, drag and tap over the two panels; a press that moves scrolls instead of picking.
void ReleasesScreen::Pointer(const std::vector<Release>* releases) {
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mouse = io.MousePos;
  const bool over_list = Inside(mouse, list_min_, list_max_);
  const bool over_content = Inside(mouse, content_min_, content_max_);
  hovered_ = -1;
  if (!ready_) {
    dragging_ = 0;
    return;
  }
  if (io.MouseWheel != 0.0f) {
    if (over_list)
      list_scroll_ -= io.MouseWheel * row_h_;
    else if (over_content)
      content_target_ -= io.MouseWheel * 90.0f * unit_;
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    dragging_ = over_list ? 1 : over_content ? 2 : 0;
    dragged_ = false;
  }
  if (dragging_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
    if (std::fabs(drag.x) + std::fabs(drag.y) > 8.0f * unit_)
      dragged_ = true;
    if (dragged_ && dragging_ == 1) {
      list_scroll_ -= io.MouseDelta.y;
    } else if (dragged_ && dragging_ == 2) {
      content_scroll_ -= io.MouseDelta.y;
      content_target_ = content_scroll_;
    }
  }
  const int row = releases && row_h_ > 0.0f
                      ? int(std::floor((mouse.y - list_min_.y - 6.0f * unit_ + list_scroll_) / row_h_))
                      : -1;
  const bool on_row = releases && over_list && row >= 0 && row < int(releases->size());
  if (on_row && !touch_)
    hovered_ = row;
  if (!dragging_ || !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    return;
  const int was = dragging_;
  dragging_ = 0;
  if (dragged_)
    return;
  if (was == 1 && on_row) {
    zone_ = Zone::kList;
    Select(row);
  } else if (was == 2 && over_content) {
    zone_ = Zone::kContent;
    const ImVec2 origin(content_min_.x + 20.0f * unit_, content_min_.y + 18.0f * unit_ - content_scroll_);
    for (const Word& word : words_) {
      if (word.url.empty())
        continue;
      const ImVec2 min(origin.x + word.pos.x, origin.y + word.pos.y);
      const ImVec2 extent = font_->CalcTextSizeA(word.size, FLT_MAX, 0.0f, word.text.c_str());
      if (Inside(mouse, min, ImVec2(min.x + extent.x, min.y + extent.y))) {
        SDL_OpenURL(word.url.c_str());
        break;
      }
    }
  }
}

// Word wrapped spans from `y`; returns the y below the last line.
float ReleasesScreen::Flow(const std::vector<Span>& spans, float left, float right, float y,
                           float size, const ImVec4& color, bool bold) {
  const float line_h = size * 1.35f;
  const float space = font_->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x;
  float x = left;
  bool need_space = false;
  auto place = [&](std::string text, const Span& span, std::string url) {
    float w = font_->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    if (x > left && x + (need_space ? space : 0.0f) + w > right) {
      y += line_h;
      x = left;
    } else if (x > left && need_space) {
      x += space;
    }
    need_space = false;
    const ImVec4& ink = !url.empty() ? kAccent
                        : span.code  ? kCode
                        : span.bold  ? kBright
                                     : color;
    // Wider than a whole line, as long URLs are: broken anywhere.
    while (w > right - x && text.size() > 1) {
      size_t cut = 1;
      for (size_t next = 1; next <= text.size(); ++next) {
        if (next < text.size() && (uint8_t(text[next]) & 0xC0) == 0x80)
          continue;
        if (font_->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str(), text.c_str() + next).x >
            right - x)
          break;
        cut = next;
      }
      words_.push_back({ImVec2(x, y), size, ink, bold || span.bold, text.substr(0, cut), url});
      text.erase(0, cut);
      w = font_->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
      y += line_h;
      x = left;
    }
    words_.push_back({ImVec2(x, y), size, ink, bold || span.bold, text, url});
    x += w;
  };
  for (const Span& span : spans) {
    size_t at = 0;
    while (at < span.text.size()) {
      if (span.text[at] == ' ') {
        need_space = true;
        ++at;
        continue;
      }
      size_t end = span.text.find(' ', at);
      if (end == std::string::npos)
        end = span.text.size();
      std::string word = span.text.substr(at, end - at);
      std::string url = span.url;
      if (url.empty() && !span.code &&
          (word.rfind("https://", 0) == 0 || word.rfind("http://", 0) == 0)) {
        url = word;
        while (!url.empty() && std::strchr(".,;:)", url.back()))
          url.pop_back();
      }
      place(std::move(word), span, std::move(url));
      at = end;
    }
  }
  return y + line_h;
}

void ReleasesScreen::Layout(const Release& release, float width) {
  words_.clear();
  marks_.clear();
  const float u = unit_;
  const Span title{release.name.empty() ? release.tag : release.name};
  float y = Flow({title}, 0.0f, width, 0.0f, 30.0f * u, kText, true);
  std::string date = release.date;
  if (release.prerelease)
    date += std::string("   ") + S("prerelease");
  y = Flow({Span{date}}, 0.0f, width, y, 16.0f * u, kMuted, false);
  y += 6.0f * u;
  marks_.push_back({ImVec2(0.0f, y), width, true});
  y += 16.0f * u;
  for (size_t i = 0; i < release.blocks.size(); ++i) {
    const Block& block = release.blocks[i];
    if (block.gap) {
      y += 10.0f * u;
      continue;
    }
    if (block.heading) {
      if (i > 0)
        y += 8.0f * u;
      const float size = (block.heading == 1 ? 28.0f : block.heading == 2 ? 25.0f : 21.0f) * u;
      y = Flow(block.spans, 0.0f, width, y, size, kHeader, true);
      if (block.heading <= 2) {
        marks_.push_back({ImVec2(0.0f, y), width, true});
        y += 8.0f * u;
      }
      continue;
    }
    const float size = 19.0f * u;
    const float left = 24.0f * u * float(block.indent);
    if (block.bullet) {
      marks_.push_back({ImVec2(left + 8.0f * u, y + size * 0.62f), 0.0f, false});
      y = Flow(block.spans, left + 22.0f * u, width, y, size, kText, false);
      y += 3.0f * u;
    } else {
      y = Flow(block.spans, left, width, y, size, kText, false);
    }
  }
  content_height_ = y + 12.0f * u;
}

void ReleasesScreen::ScrollBar(ImVec2 min, ImVec2 max, float scroll, float content) {
  const float view = max.y - min.y;
  if (content <= view)
    return;
  const float track = view - 12.0f * unit_;
  const float h = std::max(24.0f * unit_, track * view / content);
  const float top = min.y + 6.0f * unit_ + (track - h) * scroll / (content - view);
  const float x = max.x - 6.0f * unit_;
  draw_->AddRectFilled(ImVec2(x - 2.0f * unit_, top), ImVec2(x + 1.0f * unit_, top + h),
                       IM_COL32(255, 255, 255, int(70 * alpha_)), 2.0f * unit_);
}

void ReleasesScreen::DrawList(const std::vector<Release>& releases) {
  const float u = unit_;
  const float a = alpha_;
  const std::string current = Bare(kVersionString);
  const float pad = 6.0f * u;
  draw_->PushClipRect(list_min_, list_max_, true);
  for (int i = 0; i < int(releases.size()); ++i) {
    const Release& release = releases[size_t(i)];
    const ImVec2 min(list_min_.x + pad, list_min_.y + pad + float(i) * row_h_ - list_scroll_);
    const ImVec2 max(list_max_.x - pad - 8.0f * u, min.y + row_h_ - pad);
    if (max.y < list_min_.y || min.y > list_max_.y)
      continue;
    const bool picked = i == selected_;
    const bool focused = picked && zone_ == Zone::kList;
    const float rounding = 6.0f * u;
    draw_->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((focused || i == hovered_ ? 150 : 90) * a)),
                         rounding);
    if (picked)
      draw_->AddRectFilled(min, max, Col(kAccent, 0.25f * a), rounding);
    draw_->AddRect(min, max, focused ? Col(kAccent, a) : IM_COL32(255, 255, 255, int(50 * a)),
                   rounding, 0, std::max(1.0f, (focused ? 2.0f : 1.0f) * u));
    const float tag_size = 21.0f * u;
    const float date_size = 15.0f * u;
    const float y = std::floor(min.y + (max.y - min.y - tag_size - date_size - 2.0f * u) * 0.5f);
    const float x = min.x + 14.0f * u;
    draw_->AddText(font_, tag_size, ImVec2(x, y), Col(kText, a * (picked ? 1.0f : 0.85f)),
                   release.tag.c_str());
    draw_->AddText(font_, date_size, ImVec2(x, y + tag_size + 2.0f * u), Col(kMuted, a),
                   release.date.c_str());
    const bool running = !current.empty() && Bare(release.tag) == current;
    if (running || release.prerelease) {
      const char* badge = running ? S("current") : S("prerelease");
      const float w = font_->CalcTextSizeA(date_size, FLT_MAX, 0.0f, badge).x;
      draw_->AddText(font_, date_size, ImVec2(max.x - 12.0f * u - w, y + tag_size + 2.0f * u),
                     Col(running ? kGood : kGold, a), badge);
    }
  }
  draw_->PopClipRect();
  ScrollBar(list_min_, list_max_, list_scroll_, list_height_);
}

void ReleasesScreen::DrawContent(const std::vector<Release>* releases, const char* message) {
  const float u = unit_;
  const float a = alpha_;
  const ImVec2 inner_min(content_min_.x + 20.0f * u, content_min_.y + 18.0f * u);
  const ImVec2 inner_max(content_max_.x - 20.0f * u, content_max_.y - 12.0f * u);
  if (message) {
    const float size = 20.0f * u;
    const ImVec2 extent = font_->CalcTextSizeA(size, FLT_MAX, inner_max.x - inner_min.x, message);
    draw_->AddText(font_, size,
                   ImVec2(std::floor((inner_min.x + inner_max.x - extent.x) * 0.5f),
                          std::floor((inner_min.y + inner_max.y - extent.y) * 0.5f)),
                   Col(std::strcmp(message, S("failed")) == 0 ? kError : kMuted, a), message,
                   nullptr, inner_max.x - inner_min.x);
    return;
  }
  const float width = inner_max.x - inner_min.x - 10.0f * u;
  if (laid_out_ != selected_ || laid_width_ != width || laid_unit_ != u) {
    Layout((*releases)[size_t(selected_)], width);
    laid_out_ = selected_;
    laid_width_ = width;
    laid_unit_ = u;
  }
  content_view_ = content_max_.y - content_min_.y;
  const float limit = std::max(0.0f, content_height_ + 30.0f * u - content_view_);
  content_target_ = std::clamp(content_target_, 0.0f, limit);
  content_scroll_ += (content_target_ - content_scroll_) *
                     std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
  content_scroll_ = std::clamp(content_scroll_, 0.0f, limit);

  const ImVec2 origin(inner_min.x, inner_min.y - content_scroll_);
  draw_->PushClipRect(ImVec2(content_min_.x, content_min_.y + 4.0f * u),
                      ImVec2(content_max_.x, content_max_.y - 4.0f * u), true);
  for (const Mark& mark : marks_) {
    const ImVec2 at(origin.x + mark.pos.x, origin.y + mark.pos.y);
    if (mark.rule)
      draw_->AddLine(at, ImVec2(at.x + mark.width, at.y), IM_COL32(255, 255, 255, int(40 * a)),
                     std::max(1.0f, u));
    else
      draw_->AddCircleFilled(at, 3.0f * u, Col(kAccent, a));
  }
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool over = Inside(mouse, content_min_, content_max_);
  const float bold = std::max(0.5f, 0.7f * u);
  for (const Word& word : words_) {
    const ImVec2 at(origin.x + word.pos.x, origin.y + word.pos.y);
    if (at.y + word.size < content_min_.y || at.y > content_max_.y)
      continue;
    const ImU32 ink = Col(word.color, a);
    draw_->AddText(font_, word.size, at, ink, word.text.c_str());
    if (word.bold)
      draw_->AddText(font_, word.size, ImVec2(at.x + bold, at.y), ink, word.text.c_str());
    if (word.url.empty())
      continue;
    const ImVec2 extent = font_->CalcTextSizeA(word.size, FLT_MAX, 0.0f, word.text.c_str());
    const bool hot = over && !touch_ && Inside(mouse, at, ImVec2(at.x + extent.x, at.y + extent.y));
    if (hot)
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    draw_->AddLine(ImVec2(at.x, at.y + word.size), ImVec2(at.x + extent.x, at.y + word.size),
                   Col(kAccent, (hot ? 1.0f : 0.45f) * a), std::max(1.0f, u));
  }
  draw_->PopClipRect();
  ScrollBar(content_min_, content_max_, content_scroll_, content_height_ + 30.0f * u);
}

void ReleasesScreen::DrawHints(float y) {
  if (touch_ && !gamepad_)
    return;
  const std::string text = std::string(gamepad_ ? "LB RB  " : "PgUp PgDn  ") + S("scroll") +
                           "      " + (gamepad_ ? "B  " : "Esc  ") + Tr(IntroText::kHintBack);
  const float size = 17.0f * unit_;
  const ImVec2 extent = font_->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  draw_->AddText(font_, size, ImVec2(std::floor((size_.x - extent.x) * 0.5f), y),
                 Col(kText, 0.7f * alpha_), text.c_str());
}

bool ReleasesScreen::Draw(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha,
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

  const std::vector<Release>* releases = nullptr;
  const char* message = S("loading");
  {
    std::lock_guard<std::mutex> lock(fetch_->mutex);
    if (fetch_->done) {
      message = fetch_->failed ? S("failed") : fetch_->releases.empty() ? S("empty") : nullptr;
      // Written once by the fetch before `done`, then never again.
      if (!fetch_->failed && !fetch_->releases.empty())
        releases = &fetch_->releases;
    }
  }
  const int count = releases ? int(releases->size()) : 0;
  if (releases && selected_ < 0) {
    const std::string current = Bare(kVersionString);
    selected_ = 0;
    for (int i = 0; i < count; ++i) {
      if (Bare((*releases)[size_t(i)].tag) == current)
        selected_ = i;
    }
    laid_out_ = -1;
    list_scroll_ = std::max(0.0f, float(selected_ - 2)) * 64.0f * unit;
  }

  const float u = unit;
  const float top = 96.0f * u;
  const float bottom = size.y - 128.0f * u;
  const float margin = std::max(24.0f * u, (size.x - 1600.0f * u) * 0.5f);
  const float list_w = std::min(300.0f * u, size.x * 0.3f);
  list_min_ = ImVec2(margin, top);
  list_max_ = ImVec2(margin + list_w, bottom);
  content_min_ = ImVec2(list_max_.x + 16.0f * u, top);
  content_max_ = ImVec2(size.x - margin, bottom);
  row_h_ = 64.0f * u;
  list_view_ = bottom - top;
  list_height_ = float(count) * row_h_ + 6.0f * u;

  Input(count);
  if (back_)
    return true;
  Pointer(releases);
  list_scroll_ = std::clamp(list_scroll_, 0.0f, std::max(0.0f, list_height_ - list_view_));

  // Back, top left, where the version label sits on the main page.
  {
    const ImVec2 min(24.0f * u, 24.0f * u);
    const ImVec2 max(min.x + 150.0f * u, min.y + 44.0f * u);
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton("##releases_back", ImVec2(max.x - min.x, max.y - min.y));
    if (ready && ImGui::IsItemHovered() && moved_ && !touch_)
      zone_ = Zone::kBack;
    if (ready && clicked)
      back_ = true;
    const bool focused = zone_ == Zone::kBack;
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((focused ? 150 : 100) * alpha)), 6.0f * u);
    draw->AddRect(min, max, focused ? Col(kAccent, alpha) : IM_COL32(255, 255, 255, int(60 * alpha)),
                  6.0f * u, 0, std::max(1.0f, (focused ? 2.0f : 1.0f) * u));
    const char* label = Tr(IntroText::kHintBack);
    const ImVec2 extent = font->CalcTextSizeA(24.0f * u, FLT_MAX, 0.0f, label);
    draw->AddText(font, 24.0f * u,
                  ImVec2(std::floor(min.x + (max.x - min.x - extent.x) * 0.5f),
                         std::floor(min.y + (max.y - min.y - extent.y) * 0.5f)),
                  Col(kText, alpha * (focused ? 1.0f : 0.8f)), label);
  }
  const char* title = S("title");
  const float title_size = 36.0f * u;
  const ImVec2 title_extent = font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title);
  draw->AddText(font, title_size, ImVec2(std::floor((size.x - title_extent.x) * 0.5f), 26.0f * u),
                Col(kText, alpha), title);

  for (const auto& [min, max, focused] :
       {std::tuple(list_min_, list_max_, false),
        std::tuple(content_min_, content_max_, zone_ == Zone::kContent)}) {
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int(110 * alpha)), 8.0f * u);
    draw->AddRect(min, max, focused ? Col(kAccent, alpha) : IM_COL32(255, 255, 255, int(50 * alpha)),
                  8.0f * u, 0, std::max(1.0f, (focused ? 2.0f : 1.0f) * u));
  }
  if (releases)
    DrawList(*releases);
  DrawContent(releases, message);
  DrawHints(size.y - 112.0f * u);
  return back_;
}

}  // namespace nocturne
