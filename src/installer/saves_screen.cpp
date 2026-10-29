#include "saves_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "document_picker.h"
#include "intro_music.h"
#include "intro_text.h"

namespace nocturne {
namespace {

constexpr ImVec4 kAccent(0xD4 / 255.0f, 0xA8 / 255.0f, 0x4A / 255.0f, 1.0f);
constexpr ImVec4 kText(0.96f, 0.93f, 0.88f, 1.0f);
constexpr ImVec4 kMuted(0.60f, 0.62f, 0.66f, 1.0f);
constexpr ImVec4 kHeader(0.83f, 0.66f, 0.29f, 1.0f);
constexpr ImVec4 kError(1.00f, 0.35f, 0.35f, 1.0f);
constexpr ImVec4 kQuitRed(0.90f, 0.22f, 0.22f, 1.0f);
constexpr ImVec4 kGood(0.45f, 0.85f, 0.55f, 1.0f);
constexpr ImVec4 kGold(1.00f, 0.80f, 0.30f, 1.0f);

// Slots fill a row left to right from the bottom one up, as the game's load menu has them.
constexpr int kColumns = 3;
constexpr int kSlotsPerPage = 15;
constexpr int kRows = kSlotsPerPage / kColumns;
constexpr int kPages = (kSaveSlotCount + kSlotsPerPage - 1) / kSlotsPerPage;

int ColumnOf(int slot) {
  return (slot % kSlotsPerPage) % kColumns;
}

// From the top.
int RowOf(int slot) {
  return kRows - 1 - (slot % kSlotsPerPage) / kColumns;
}

int SlotAt(int page, int column, int row) {
  return page * kSlotsPerPage + (kRows - 1 - row) * kColumns + column;
}

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
  return Tr("saves", key);
}

// The text with each {} replaced by the next argument.
std::string Fill(const char* key, std::initializer_list<std::string> args) {
  std::string text = S(key);
  size_t at = 0;
  for (const std::string& arg : args) {
    if (at = text.find("{}", at); at == std::string::npos)
      break;
    text.replace(at, 2, arg);
    at += arg.size();
  }
  return text;
}

// "01" for slot 0; the messages name the slot in their own words around it.
std::string SlotNumber(int slot) {
  char number[8];
  std::snprintf(number, sizeof(number), "%02d", slot + 1);
  return number;
}

std::string SlotName(int slot) {
  return Fill("slot", {SlotNumber(slot)});
}

// Fits `text` in `width`, shrinking it when it is wider.
void FitText(ImDrawList* draw, ImFont* font, float size, ImVec2 min, ImVec2 max, ImU32 color,
             const char* text) {
  ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const float width = max.x - min.x;
  if (extent.x > width) {
    size *= width / extent.x;
    extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  }
  draw->AddText(font, size,
                ImVec2(std::floor(min.x + (width - extent.x) * 0.5f),
                       std::floor(min.y + (max.y - min.y - extent.y) * 0.5f)),
                color, text);
}

}  // namespace

SavesScreen::SavesScreen(rex::ui::ImmediateDrawer* drawer, rex::ui::WindowedAppContext* context,
                         std::filesystem::path user_data_root)
    : drawer_(drawer), context_(context), root_(std::move(user_data_root)) {}

SavesScreen::~SavesScreen() {
  *alive_ = false;
  // The frame that last drew them may still be in flight, so they are never freed.
  static std::vector<std::unique_ptr<rex::ui::ImmediateTexture>> graveyard;
  for (auto& frames : thumbnails_) {
    for (auto& texture : frames)
      graveyard.push_back(std::move(texture));
  }
}

void SavesScreen::Reopen(bool gamepad) {
  loaded_ = false;
  gamepad_ = gamepad;
  focus_ = {};
  page_ = 0;
  status_.clear();
  CancelDelete();
}

void SavesScreen::Reload() {
  slots_ = ReadSaveSlots(root_);
  static std::vector<std::unique_ptr<rex::ui::ImmediateTexture>> graveyard;
  for (int i = 0; i < kSaveSlotCount; ++i) {
    auto& textures = thumbnails_[size_t(i)];
    for (auto& texture : textures)
      graveyard.push_back(std::move(texture));
    textures.clear();
    if (!drawer_)
      continue;
    for (const auto& rgba : slots_[size_t(i)].frames)
      textures.push_back(drawer_->CreateTexture(kSaveThumbWidth, kSaveThumbHeight,
                                                rex::ui::ImmediateTextureFilter::kNearest, false,
                                                rgba.data()));
  }
  loaded_ = true;
}

void SavesScreen::Move(int dx, int dy) {
  Focus& f = focus_;
  switch (f.zone) {
    case Zone::kBack:
      if (dy > 0)
        f = {Zone::kSlot, SlotAt(page_, 0, 0)};
      return;
    case Zone::kMusic:
      if (dy < 0)
        f = before_bottom_;
      else if (dx > 0 && IntroLanguageCount() >= 2)
        f = {Zone::kPicker, 0};
      return;
    case Zone::kPicker:
      return;
    case Zone::kSlot:
    case Zone::kExport:
    case Zone::kDelete:
    case Zone::kImport:
      break;
  }
  const int column = ColumnOf(f.index);
  const int row = RowOf(f.index);
  if (dy) {
    if (row + dy < 0) {
      f = {Zone::kBack, 0};
    } else if (row + dy >= kRows) {
      before_bottom_ = {Zone::kSlot, f.index};
      if (IntroMusicAvailable())
        f = {Zone::kMusic, 0};
      else if (IntroLanguageCount() >= 2)
        f = {Zone::kPicker, 0};
    } else {
      f = {Zone::kSlot, SlotAt(page_, column, row + dy)};
    }
    return;
  }
  // Across: the row, then its buttons, then the next column's row.
  const std::vector<Zone> parts = Used(f.index)
                                      ? std::vector<Zone>{Zone::kSlot, Zone::kExport, Zone::kDelete}
                                      : std::vector<Zone>{Zone::kSlot, Zone::kImport};
  const int at = int(std::find(parts.begin(), parts.end(), f.zone) - parts.begin());
  const int next = at + dx;
  if (next >= 0 && next < int(parts.size()))
    f.zone = parts[size_t(next)];
  else if (dx > 0 && column + 1 < kColumns)
    f = {Zone::kSlot, SlotAt(page_, column + 1, row)};
  else if (dx < 0 && column > 0)
    f = {Zone::kSlot, SlotAt(page_, column - 1, row)};
  else if (dx > 0 && page_ + 1 < kPages)
    f = {Zone::kSlot, SlotAt(page_ + 1, 0, row)};
  else if (dx < 0 && page_ > 0)
    f = {Zone::kSlot, SlotAt(page_ - 1, kColumns - 1, row)};
}

void SavesScreen::ChangePage(int delta) {
  const int page = std::clamp(page_ + delta, 0, kPages - 1);
  if (page == page_)
    return;
  const bool on_slot = focus_.zone == Zone::kSlot || focus_.zone == Zone::kExport ||
                       focus_.zone == Zone::kDelete || focus_.zone == Zone::kImport;
  if (on_slot)
    focus_ = {Zone::kSlot, page * kSlotsPerPage + focus_.index % kSlotsPerPage};
  page_ = page;
}

// A row exports its save, or imports into it when empty.
void SavesScreen::Press(const Focus& focus) {
  switch (focus.zone) {
    case Zone::kBack:
      back_ = true;
      break;
    case Zone::kMusic:
      SetIntroMusicEnabled(!IntroMusicEnabled());
      break;
    case Zone::kSlot:
      Used(focus.index) ? Export(focus.index) : Import(focus.index);
      break;
    case Zone::kExport:
      Export(focus.index);
      break;
    case Zone::kImport:
      Import(focus.index);
      break;
    case Zone::kDelete:
      if (Used(focus.index)) {
        // Twice, starting on Cancel, so a stray press cannot delete a save.
        deleting_ = focus.index;
        confirm_step_ = 1;
        confirm_on_delete_ = false;
      }
      break;
    case Zone::kPicker:
      break;
  }
}

void SavesScreen::Export(int slot) {
  if (!context_ || !Used(slot))
    return;
  waiting_ = true;
  status_.clear();
  std::weak_ptr<bool> alive = alive_;
  const std::filesystem::path root = root_;
  PickFileToWrite(
      *context_, SaveSlotExportName(slot), root / "cache",
      [root, slot](const std::filesystem::path& archive) {
        return ExportSaveSlot(root, slot, archive);
      },
      [this, alive, slot](std::optional<std::string> error) {
        if (alive.expired())
          return;
        waiting_ = false;
        if (!error)
          return;
        status_error_ = !error->empty();
        status_ = status_error_ ? Fill("export_failed", {*error}) : Fill("exported", {SlotNumber(slot)});
      });
}

void SavesScreen::Import(int slot) {
  if (!context_ || Used(slot))
    return;
  waiting_ = true;
  status_.clear();
  std::weak_ptr<bool> alive = alive_;
  PickFileToRead(*context_, [this, alive, slot](std::optional<std::filesystem::path> picked) {
    if (alive.expired())
      return;
    waiting_ = false;
    if (!picked)
      return;
    PreparedSave save;
    std::string error = PrepareSaveImport(root_, *picked, save);
    if (error.empty())
      error = WriteSaveSlot(root_, slot, save);
    status_error_ = !error.empty();
    if (status_error_)
      status_ = Fill("import_failed", {error});
    else
      status_ = Fill("imported", {SlotNumber(slot)});
    Reload();
    focus_ = {Zone::kSlot, slot};
  });
}

void SavesScreen::Delete() {
  const int slot = deleting_;
  CancelDelete();
  const std::string error = DeleteSaveSlot(root_, slot);
  status_error_ = !error.empty();
  status_ = status_error_ ? Fill("delete_failed", {SlotNumber(slot), error})
                          : Fill("deleted", {SlotNumber(slot)});
  Reload();
  focus_ = {Zone::kSlot, slot};
}

void SavesScreen::CancelDelete() {
  deleting_ = -1;
  confirm_step_ = 0;
}

void SavesScreen::ConfirmInput() {
  if (Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft, ImGuiKey_RightArrow,
               ImGuiKey_D, ImGuiKey_GamepadDpadRight})) {
    confirm_on_delete_ = !confirm_on_delete_;
  } else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                      ImGuiKey_GamepadFaceDown})) {
    if (!confirm_on_delete_) {
      CancelDelete();
    } else if (confirm_step_ == 1) {
      confirm_step_ = 2;
      confirm_on_delete_ = false;
    } else {
      Delete();
    }
  } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
    CancelDelete();
  }
}

void SavesScreen::Input() {
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
  if (!ready_ || waiting_ || focus_.zone == Zone::kPicker)
    return;
  if (confirm_step_) {
    ConfirmInput();
    return;
  }
  const bool on_slot = focus_.zone == Zone::kSlot || focus_.zone == Zone::kExport ||
                       focus_.zone == Zone::kDelete || focus_.zone == Zone::kImport;
  if (Pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown})) {
    Move(0, 1);
  } else if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp})) {
    Move(0, -1);
  } else if (Pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight})) {
    Move(1, 0);
  } else if (Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft})) {
    Move(-1, 0);
  } else if (Pressed({ImGuiKey_Q, ImGuiKey_PageUp, ImGuiKey_GamepadL1})) {
    ChangePage(-1);
  } else if (Pressed({ImGuiKey_E, ImGuiKey_PageDown, ImGuiKey_GamepadR1})) {
    ChangePage(1);
  } else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                      ImGuiKey_GamepadFaceDown})) {
    Press(focus_);
  } else if (on_slot && Pressed({ImGuiKey_Delete, ImGuiKey_GamepadFaceLeft})) {
    Press({Zone::kDelete, focus_.index});
  } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
    back_ = true;
  }
}

// Mouse and touch: a press acts on release over the same thing, a hover only
// moves the cursor when the mouse moves, so a resting cursor does not undo the keys.
bool SavesScreen::Hit(ImVec2 min, ImVec2 max, Focus self) {
  // Nothing under the question may take its clicks.
  if (confirm_step_)
    return false;
  ImGui::SetCursorScreenPos(min);
  ImGui::PushID(int(self.zone) * 100 + self.index);
  const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(max.x - min.x, max.y - min.y));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  if (!ready_ || waiting_)
    return false;
  if (hovered && moved_ && !touch_)
    focus_ = self;
  if (clicked) {
    focus_ = self;
    // A tap on a row only selects it; its buttons act.
    if (self.zone != Zone::kSlot)
      Press(self);
  }
  return hovered && !touch_;
}

void SavesScreen::Plate(ImVec2 min, ImVec2 max, bool focused, float a) {
  const float rounding = 6.0f * unit_;
  draw_->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((focused ? 150 : 100) * a)), rounding);
  draw_->AddRect(min, max, focused ? Col(kAccent, a) : IM_COL32(255, 255, 255, int(60 * a)),
                 rounding, 0, std::max(1.0f, (focused ? 2.0f : 1.0f) * unit_));
}

// Export (an arrow out of a tray), Import (an arrow into it) or Delete (a
// trash can); the hints name them.
void SavesScreen::DrawSideButton(ImVec2 min, ImVec2 max, Focus self) {
  const bool hovered = Hit(min, max, self);
  const bool hot = hovered || focus_ == self;
  const float u = unit_;
  const float a = alpha_;
  const ImVec4& glow = self.zone == Zone::kDelete   ? kQuitRed
                       : self.zone == Zone::kImport ? kGood
                                                    : kAccent;
  const float rounding = 6.0f * u;
  draw_->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((hot ? 150 : 100) * a)), rounding);
  if (hot)
    draw_->AddRectFilled(min, max, Col(glow, 0.3f * a), rounding);
  draw_->AddRect(min, max, hot ? Col(glow, a) : IM_COL32(255, 255, 255, int(60 * a)), rounding, 0,
                 std::max(1.0f, (hot ? 2.0f : 1.0f) * u));

  const ImU32 ink = Col(kText, a * (hot ? 1.0f : 0.8f));
  const float thick = std::max(1.5f, 2.0f * u);
  const float cx = (min.x + max.x) * 0.5f;
  const float s = std::min(max.x - min.x, max.y - min.y) * 0.46f;
  const float top = (min.y + max.y - s) * 0.5f;
  const float bottom = top + s;
  if (self.zone == Zone::kDelete) {
    // Handle, lid, then the tapered bin.
    const float lid = top + s * 0.18f;
    draw_->PathLineTo(ImVec2(cx - s * 0.12f, lid));
    draw_->PathLineTo(ImVec2(cx - s * 0.12f, top));
    draw_->PathLineTo(ImVec2(cx + s * 0.12f, top));
    draw_->PathLineTo(ImVec2(cx + s * 0.12f, lid));
    draw_->PathStroke(ink, 0, thick);
    draw_->AddLine(ImVec2(cx - s * 0.45f, lid), ImVec2(cx + s * 0.45f, lid), ink, thick);
    draw_->PathLineTo(ImVec2(cx - s * 0.33f, lid + s * 0.1f));
    draw_->PathLineTo(ImVec2(cx - s * 0.26f, bottom));
    draw_->PathLineTo(ImVec2(cx + s * 0.26f, bottom));
    draw_->PathLineTo(ImVec2(cx + s * 0.33f, lid + s * 0.1f));
    draw_->PathStroke(ink, 0, thick);
  } else {
    const bool up = self.zone == Zone::kExport;
    // The tray.
    draw_->PathLineTo(ImVec2(cx - s * 0.45f, bottom - s * 0.3f));
    draw_->PathLineTo(ImVec2(cx - s * 0.45f, bottom));
    draw_->PathLineTo(ImVec2(cx + s * 0.45f, bottom));
    draw_->PathLineTo(ImVec2(cx + s * 0.45f, bottom - s * 0.3f));
    draw_->PathStroke(ink, 0, thick);
    // The arrow, leaving it or going in.
    const float tip = up ? top : bottom - s * 0.15f;
    const float tail = up ? bottom - s * 0.15f : top;
    const float head = s * 0.25f * (up ? 1.0f : -1.0f);
    draw_->AddLine(ImVec2(cx, tail), ImVec2(cx, tip + head * 0.5f), ink, thick);
    draw_->AddTriangleFilled(ImVec2(cx, tip), ImVec2(cx + s * 0.2f, tip + head),
                             ImVec2(cx - s * 0.2f, tip + head), ink);
  }
}

void SavesScreen::DrawSlot(int slot, ImVec2 min, ImVec2 max) {
  const SaveSlotInfo& info = slots_[size_t(slot)];
  const float u = unit_;
  const float h = max.y - min.y;
  const bool selected = focus_.index == slot &&
                        (focus_.zone == Zone::kSlot || focus_.zone == Zone::kExport ||
                         focus_.zone == Zone::kDelete || focus_.zone == Zone::kImport);
  // The buttons take the right end of the focused row, as the trash can does on the Mods page.
  ImVec2 body_max = max;
  if (selected && !confirm_step_) {
    const float side = h - 8.0f * u;
    ImVec2 at(max.x - side, min.y + 4.0f * u);
    for (const Zone zone : info.used ? std::vector<Zone>{Zone::kDelete, Zone::kExport}
                                     : std::vector<Zone>{Zone::kImport}) {
      DrawSideButton(at, ImVec2(at.x + side, at.y + side), {zone, slot});
      body_max.x = at.x - 6.0f * u;
      at.x -= side + 6.0f * u;
    }
  }
  Hit(min, body_max, {Zone::kSlot, slot});
  const float a = alpha_;
  Plate(min, body_max, selected && focus_.zone == Zone::kSlot, a);

  const float pad = 6.0f * u;
  const float thumb_h = h - 2.0f * pad;
  const float thumb_w = thumb_h * float(kSaveThumbWidth) / float(kSaveThumbHeight);
  const ImVec2 thumb(min.x + pad, min.y + pad);
  const ImVec2 thumb_max(thumb.x + thumb_w, thumb.y + thumb_h);
  if (const auto& frames = thumbnails_[size_t(slot)]; !frames.empty()) {
    size_t frame = 0;
    if (frames.size() > 1 && info.frame_seconds > 0.0f)
      frame = size_t(ImGui::GetTime() / info.frame_seconds) % frames.size();
    draw_->AddImageRounded(reinterpret_cast<ImTextureID>(frames[frame].get()), thumb, thumb_max,
                           ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, int(255 * a)),
                           4.0f * u);
  } else {
    draw_->AddRectFilled(thumb, thumb_max, IM_COL32(255, 255, 255, int(12 * a)), 4.0f * u);
  }

  const float text_x = thumb_max.x + 12.0f * u;
  const float line1 = 21.0f * u;
  const float line2 = 16.0f * u;
  const int text_lines = 2;
  float y = std::floor(min.y + (h - line1 - float(text_lines - 1) * (line2 + 3.0f * u)) * 0.5f);
  draw_->PushClipRect(ImVec2(text_x, min.y), ImVec2(body_max.x - 6.0f * u, max.y), true);
  // The buttons narrow the row, so it says less: "#13", and no date.
  const bool compact = body_max.x != max.x;
  const std::string name = compact ? "#" + SlotNumber(slot) : SlotName(slot);
  draw_->AddText(font_, line1, ImVec2(text_x, y), Col(kText, a * (selected ? 1.0f : 0.85f)),
                 name.c_str());
  if (info.used) {
    // The map completion after the slot's name, as the game's title has it.
    const float x = text_x + font_->CalcTextSizeA(line1, FLT_MAX, 0.0f, name.c_str()).x + 10.0f * u;
    draw_->AddText(font_, line2, ImVec2(x, y + (line1 - line2) * 0.6f), Col(kHeader, a),
                   info.completion.c_str());
    if (info.randomized) {
      // A die at the row's right end, in the randomizer page's gold: the five face.
      const float side = 20.0f * u;
      const ImVec2 dmin(body_max.x - 10.0f * u - side, y + (line1 - side) * 0.5f);
      const ImVec2 dmax(dmin.x + side, dmin.y + side);
      draw_->AddRectFilled(dmin, dmax, Col(kGold, 0.18f * a), 4.0f * u);
      draw_->AddRect(dmin, dmax, Col(kGold, 0.9f * a), 4.0f * u, 0, std::max(1.0f, 1.5f * u));
      const ImVec2 mid((dmin.x + dmax.x) * 0.5f, (dmin.y + dmax.y) * 0.5f);
      const float off = side * 0.25f;
      const float pip = std::max(1.2f, side * 0.075f);
      for (const ImVec2 p : {ImVec2(0, 0), ImVec2(-off, -off), ImVec2(off, -off),
                             ImVec2(-off, off), ImVec2(off, off)})
        draw_->AddCircleFilled(ImVec2(mid.x + p.x, mid.y + p.y), pip, Col(kGold, a));
    }
  }
  y += line1 + 3.0f * u;
  if (!info.used) {
    draw_->AddText(font_, line2, ImVec2(text_x, y), Col(kMuted, a), S("empty"));
  } else {
    // The player's name, as the load menu shows it; the date at its right.
    const std::string& chapter = info.name;
    draw_->AddText(font_, line2, ImVec2(text_x, y), Col(kText, 0.85f * a), chapter.c_str());
    if (!compact) {
      const float date_size = 14.0f * u;
      const float date_w = font_->CalcTextSizeA(date_size, FLT_MAX, 0.0f, info.modified.c_str()).x;
      const float chapter_end =
          text_x + font_->CalcTextSizeA(line2, FLT_MAX, 0.0f, chapter.c_str()).x + 12.0f * u;
      draw_->AddText(font_, date_size,
                     ImVec2(std::max(chapter_end, body_max.x - 10.0f * u - date_w),
                            y + (line2 - date_size)),
                     Col(kMuted, 0.8f * a), info.modified.c_str());
    }
  }
  draw_->PopClipRect();
}

void SavesScreen::DrawConfirm() {
  const float u = unit_;
  const float a = alpha_;
  draw_->AddRectFilled(ImVec2(0, 0), size_, IM_COL32(0, 0, 0, int(150 * a)));
  const float w = std::min(640.0f * u, size_.x - 32.0f);
  const float pad = 24.0f * u;
  const float text_size = 21.0f * u;
  const std::string message =
      confirm_step_ == 1
          ? Fill("confirm_delete", {SlotNumber(deleting_), slots_[size_t(deleting_)].name})
          : Fill("confirm_delete_again", {SlotNumber(deleting_)});
  const ImVec2 extent = font_->CalcTextSizeA(text_size, FLT_MAX, w - 2.0f * pad, message.c_str());
  const float button_h = 48.0f * u;
  const float h = pad + extent.y + 20.0f * u + button_h + pad;
  const ImVec2 min((size_.x - w) * 0.5f, (size_.y - h) * 0.5f);
  const ImVec2 max(min.x + w, min.y + h);
  draw_->AddRectFilled(min, max, IM_COL32(22, 5, 9, int(240 * a)), 8.0f * u);
  draw_->AddRect(min, max, Col(kAccent, a), 8.0f * u, 0, std::max(1.0f, 1.5f * u));
  draw_->AddText(font_, text_size, ImVec2(min.x + pad, min.y + pad), Col(kText, a), message.c_str(),
                 nullptr, w - 2.0f * pad);

  // The second time the buttons swap places, so the same press twice does not delete.
  const float button_w = 190.0f * u;
  const float gap = 16.0f * u;
  const float y = max.y - pad - button_h;
  const float left = (size_.x - 2.0f * button_w - gap) * 0.5f;
  for (int i = 0; i < 2; ++i) {
    const bool remove = (i == 1) == (confirm_step_ == 1);
    const ImVec2 bmin(left + float(i) * (button_w + gap), y);
    const ImVec2 bmax(bmin.x + button_w, y + button_h);
    ImGui::SetCursorScreenPos(bmin);
    ImGui::PushID(500 + i);
    const bool clicked = ImGui::InvisibleButton("##confirm", ImVec2(button_w, button_h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (ready_ && hovered && moved_ && !touch_)
      confirm_on_delete_ = remove;
    const bool focused = confirm_on_delete_ == remove;
    const ImVec4& accent = remove ? kQuitRed : kAccent;
    draw_->AddRectFilled(bmin, bmax, IM_COL32(0, 0, 0, int((focused ? 150 : 100) * a)), 6.0f * u);
    if (focused && remove)
      draw_->AddRectFilled(bmin, bmax, Col(kQuitRed, 0.3f * a), 6.0f * u);
    draw_->AddRect(bmin, bmax, focused ? Col(accent, a) : IM_COL32(255, 255, 255, int(60 * a)),
                   6.0f * u, 0, std::max(1.0f, (focused ? 2.0f : 1.0f) * u));
    FitText(draw_, font_, 22.0f * u, ImVec2(bmin.x + 8.0f * u, bmin.y),
            ImVec2(bmax.x - 8.0f * u, bmax.y), Col(kText, a * (focused ? 1.0f : 0.8f)),
            remove ? S("delete") : Tr(IntroText::kCancel));
    if (!clicked || !ready_)
      continue;
    if (!remove) {
      CancelDelete();
    } else if (confirm_step_ == 1) {
      confirm_step_ = 2;
      confirm_on_delete_ = false;
    } else {
      Delete();
    }
    return;
  }
}

void SavesScreen::DrawHints(float y) {
  const char* confirm = gamepad_ ? "A  " : "Enter  ";
  const char* back = gamepad_ ? "B  " : "Esc  ";
  const char* delete_key = gamepad_ ? "X  " : "Del  ";
  const char* gap = "      ";
  std::string text;
  if (touch_ && !gamepad_) {
    text = std::string(TrOr("intro", "hint_tap", "Tap")) + "  " + Tr(IntroText::kHintSelect);
  } else if (confirm_step_) {
    text = std::string(confirm) + Tr(IntroText::kHintSelect) + gap + back + Tr(IntroText::kCancel);
  } else {
    const bool on_slot = focus_.zone == Zone::kSlot || focus_.zone == Zone::kExport ||
                         focus_.zone == Zone::kDelete || focus_.zone == Zone::kImport;
    const bool used = on_slot && Used(focus_.index);
    const char* action = !on_slot                       ? Tr(IntroText::kHintSelect)
                         : focus_.zone == Zone::kDelete ? S("delete")
                         : used                         ? S("export")
                                                        : S("import");
    text = std::string(confirm) + action;
    if (used && focus_.zone != Zone::kDelete)
      text += gap + std::string(delete_key) + S("delete");
    text += gap + std::string(back) + Tr(IntroText::kHintBack);
  }
  if (gamepad_ && !confirm_step_)
    text += gap + std::string("LB/RB  ") + S("page");
  const float size = 17.0f * unit_;
  const ImVec2 extent = font_->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  draw_->AddText(font_, size, ImVec2(std::floor((size_.x - extent.x) * 0.5f), y),
                 Col(kText, 0.7f * alpha_), text.c_str());
}

bool SavesScreen::Draw(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha,
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
  if (!loaded_)
    Reload();
  Input();

  const float u = unit;
  // Back, top left, where the version label sits on the main page.
  {
    const ImVec2 min(24.0f * u, 24.0f * u);
    const ImVec2 max(min.x + 150.0f * u, min.y + 44.0f * u);
    Hit(min, max, {Zone::kBack, 0});
    Plate(min, max, focus_.zone == Zone::kBack, alpha);
    FitText(draw, font, 24.0f * u, min, max,
            Col(kText, alpha * (focus_.zone == Zone::kBack ? 1.0f : 0.8f)), Tr(IntroText::kHintBack));
  }
  const char* title = S("title");
  const float title_size = 36.0f * u;
  const ImVec2 title_extent = font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title);
  draw->AddText(font, title_size, ImVec2(std::floor((size.x - title_extent.x) * 0.5f), 26.0f * u),
                Col(kText, alpha), title);

  const float width = std::min(1000.0f * u, size.x - 32.0f);
  const float x0 = (size.x - width) * 0.5f;
  const float column_gap = 16.0f * u;
  const float column_w = (width - column_gap * float(kColumns - 1)) / float(kColumns);
  const float row_h = 72.0f * u;
  const float row_gap = 10.0f * u;
  const float top = 96.0f * u;
  if (focus_.zone == Zone::kSlot || focus_.zone == Zone::kExport ||
      focus_.zone == Zone::kDelete || focus_.zone == Zone::kImport)
    page_ = focus_.index / kSlotsPerPage;
  for (int i = page_ * kSlotsPerPage; i < (page_ + 1) * kSlotsPerPage; ++i) {
    const ImVec2 min(x0 + float(ColumnOf(i)) * (column_w + column_gap),
                     top + float(RowOf(i)) * (row_h + row_gap));
    DrawSlot(i, min, ImVec2(min.x + column_w, min.y + row_h));
  }
  const float bottom = top + kRows * (row_h + row_gap) - row_gap;

  // "< 1 / 2 >" under the grid.
  {
    const float size_px = 20.0f * u;
    const float arrow_w = 40.0f * u;
    char label[32];
    std::snprintf(label, sizeof(label), "%d / %d", page_ + 1, kPages);
    const float label_w = font->CalcTextSizeA(size_px, FLT_MAX, 0.0f, label).x;
    const float cy = bottom + 28.0f * u;
    const float cx = size.x * 0.5f;
    draw->AddText(font, size_px, ImVec2(std::floor(cx - label_w * 0.5f), cy - size_px * 0.5f),
                  Col(kText, alpha), label);
    for (int dir = -1; dir <= 1; dir += 2) {
      const bool enabled = page_ + dir >= 0 && page_ + dir < kPages;
      const float ax = cx + float(dir) * (label_w * 0.5f + 12.0f * u + arrow_w * 0.5f);
      const ImVec2 amin(ax - arrow_w * 0.5f, cy - 16.0f * u);
      const ImVec2 amax(ax + arrow_w * 0.5f, cy + 16.0f * u);
      bool hovered = false;
      if (!confirm_step_ && ready_ && !waiting_ && enabled) {
        ImGui::SetCursorScreenPos(amin);
        ImGui::PushID(900 + dir);
        if (ImGui::InvisibleButton("##page", ImVec2(amax.x - amin.x, amax.y - amin.y)))
          ChangePage(dir);
        hovered = ImGui::IsItemHovered() && !touch_;
        ImGui::PopID();
      }
      const ImU32 ink = Col(hovered ? kAccent : kText, alpha * (enabled ? 1.0f : 0.25f));
      const float t = 7.0f * u;
      const float s = float(dir);
      draw->AddTriangleFilled(ImVec2(ax + s * t, cy), ImVec2(ax - s * t, cy - t),
                              ImVec2(ax - s * t, cy + t), ink);
    }
  }

  std::string status = waiting_ ? std::string(S("waiting")) : status_;
  if (!status.empty()) {
    const float text_size = 18.0f * u;
    const ImVec2 extent = font->CalcTextSizeA(text_size, FLT_MAX, width, status.c_str());
    draw->AddText(font, text_size, ImVec2(std::floor((size.x - std::min(extent.x, width)) * 0.5f),
                                          bottom + 52.0f * u),
                  Col(waiting_ ? kMuted : status_error_ ? kError : kGood, alpha), status.c_str(),
                  nullptr, width);
  }
  DrawHints(bottom + 96.0f * u);
  if (confirm_step_ && deleting_ >= 0)
    DrawConfirm();
  return back_;
}

}  // namespace nocturne
