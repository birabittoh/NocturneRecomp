// nocturnerecomp: the row tables native_options.cpp shows on the game's own
// settings screen. One screen, several pages: Help & Options -> Settings shows
// the settings page, Help & Options -> Enhancements the enhancements page
// (native_enhancements.cpp). See native_options.cpp for how a table becomes
// rows on that screen.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ui/native_ui.h"

namespace nocturne {

// Rows the stock screen builds itself: Graphics / Volume / Change Screen Size.
inline constexpr uint32_t kVanillaRowCount = 3;

// The option list has room for 16 rows (sub_825D3200 zeroes exactly 16
// value-widget slots), so at most that many show at once; a page with more
// scrolls through them.
inline constexpr uint32_t kListMaxRows = 16;

// Upper bound on any page's table, for the per-row state native_options.cpp
// keeps.
inline constexpr uint32_t kMaxPageRows = 32;

struct OptionRow {
  // Which row of the page this occupies, counting from the top of the page
  // rather than the top of the screen (the two differ once a page scrolls).
  //
  // On a page that keeps the stock rows, < kVanillaRowCount *takes over* a
  // stock row instead of adding one: the row keeps its position and its own
  // localized label, but its value is seeded from `get` and applied through
  // `commit`, and it is no longer mirrored back into the game's model: so
  // whatever the stock row used to drive is left alone entirely.
  uint32_t list_row;

  // Label, re-read every time the row is painted. Leave the key null on an
  // override row to keep the game's own label (which is the usual choice: it
  // is already localized, and set at the right scale).
  NativeText label;

  // Enumerated row: value_count values, indices 0..value_count-1, painted
  // with `values` (one text per value) or, for text only known at runtime,
  // `value_text`. Leave both null for a numeric row, which uses
  // min/max/step/multiplier and is rendered by the game itself.
  uint32_t value_count = 0;
  const NativeText* values = nullptr;
  const char16_t* (*value_text)(int32_t value) = nullptr;

  // Optional: how many of the values are actually offered right now, re-asked
  // every time the screen binds. For a row whose tail entries only become
  // reachable once something exists to back them (the Preset row's "Custom").
  // Must never exceed `value_count`.
  uint32_t (*live_value_count)() = nullptr;

  // Numeric row bounds. Ignored on an enumerated row.
  int32_t min = 0;
  int32_t max = 0;
  int32_t step = 1;
  int32_t multiplier = 1;

  // Value to show when the screen opens, and where it goes on A. Both get the
  // row, so one pair of functions can serve every row backed by the same kind
  // of setting (see the cvar helpers below).
  //
  // Both null makes this a *label-only* row: the stock row keeps its value,
  // its behaviour and its model copy-back untouched, and only its text is
  // replaced. Useful for correcting a label without adopting the setting.
  int32_t (*get)(const OptionRow& row) = nullptr;
  void (*commit)(const OptionRow& row, int32_t value) = nullptr;

  // Value X ("defaults") puts back on a page that owns all its rows. Null
  // falls back to the value the row was opened with.
  int32_t (*reset)(const OptionRow& row) = nullptr;

  // The cvar behind the row, for the cvar helpers below.
  const char* cvar = nullptr;

  // Apply on every left/right as well as on A, so the player gets immediate
  // feedback (and B puts the original value back). Right for things you can
  // perceive while the menu is open, like volume.
  bool live = false;

  // The setting can change behind the screen's back (Alt+Enter toggling
  // fullscreen): `get` is re-asked every frame and the row repainted when it
  // differs from what is shown.
  bool follow = false;

  // Override rows only: force the stock setting this row displaced to its
  // maximum, so whatever now backs the row has the full range to work with
  // rather than being multiplied down by a leftover value nobody can reach
  // any more.
  bool pin_stock_to_max = false;

  // Override rows only: returning false leaves the row completely alone:
  // the stock row keeps its vanilla behaviour, model copy-back and all.
  bool (*available)() = nullptr;

  // Optional: while this returns true the row holds the value `get` returns,
  // whatever left/right or X do, for a setting something else decides (an
  // enhancement the active seed applies itself). `commit` still runs on A,
  // with that value, so it should leave the setting alone while locked.
  bool (*locked)(const OptionRow& row) = nullptr;

  // Help text for the page's description column while the row is highlighted
  // (OptionsPage::row_description). A null key shows none.
  NativeText tooltip;
};

// Text shown in a column left of the list (OptionsPage::description).
// `warning` leads it in a warning colour; `text`'s paragraphs are separated by
// '\n'. Both empty shows nothing.
struct PageDescription {
  std::u16string_view warning;
  std::u16string_view text;
};

struct OptionsPage {
  const OptionRow* rows;
  uint32_t row_count;

  // Rows on the page, the stock ones included when the page keeps them.
  uint32_t total_rows;

  // How many rows show at once. A page with more scrolls them into view as
  // the selection moves past the top or bottom row.
  uint32_t visible_rows;

  // Replaces the screen's title. A null key keeps the game's own.
  NativeText title;

  // The row arrows on either side of the title, hinting at the shoulder
  // buttons: there is another page to go to.
  bool title_arrows = false;

  // Every row is this page's own: the stock Graphics/Volume model is neither
  // shown nor mirrored, and X resets the rows themselves instead of the
  // game's settings.
  bool owns_all_rows = false;

  // Y opens the stretch screen, with a prompt for it on the bar.
  bool stretch_prompt = false;

  // A fourth prompt on the bar, after the stock three: its text and the guest
  // address of its glyph's image name. A null key shows none.
  NativeText prompt_text;
  uint32_t prompt_glyph = 0;

  // Text drawn to the right of the title, whatever the rows do (a credit),
  // past where the title's right arrow goes so it stays put with or without
  // it. A null key shows none.
  NativeText credit;

  // Runs on A once every row of the page has been committed.
  void (*accept)() = nullptr;

  // A page whose rows depend on its own values (options that only matter for
  // some choices of another): asked again after every change and after X, it
  // lays its rows out afresh in `rows`/`row_count`/`total_rows` and returns
  // true when they changed, for the screen to rebind them. Its rows should be
  // `live`, so that what it lays out from is the value just chosen. It may
  // also change its title and prompt, which the screen then redraws.
  bool (*relayout)() = nullptr;

  // Optional: text for the column left of the list, asked again every frame,
  // so it follows the rows' values (the randomizer's preset description).
  PageDescription (*description)() = nullptr;

  // Like `description`, but for the row highlighted, by its list_row and the
  // value it shows (not yet committed on a row that isn't `live`).
  PageDescription (*row_description)(uint32_t list_row, int32_t value) = nullptr;
};

// Every row on a unique, in-range line; label-only rows only on stock lines of
// a page that keeps them; every line of a page that owns its rows covered by
// a row with a value behind it.
template <size_t N>
consteval bool RowsAreWellFormed(const OptionRow (&rows)[N], uint32_t total_rows,
                                 bool owns_all_rows) {
  if (N > kMaxPageRows || total_rows < N || total_rows > kMaxPageRows) {
    return false;
  }
  for (size_t i = 0; i < N; ++i) {
    const OptionRow& row = rows[i];
    if (row.list_row >= total_rows) {
      return false;
    }
    if (row.get == nullptr) {
      if (owns_all_rows || row.label.key == nullptr || row.list_row >= kVanillaRowCount) {
        return false;
      }
    }
    if ((row.values != nullptr || row.value_text != nullptr) != (row.value_count > 0)) {
      return false;
    }
    if (row.available != nullptr && (owns_all_rows || row.list_row >= kVanillaRowCount)) {
      return false;
    }
    for (size_t j = i + 1; j < N; ++j) {
      if (row.list_row == rows[j].list_row) {
        return false;
      }
    }
  }
  // Unique and in range, so the right count means every line is covered.
  uint32_t appended = 0;
  for (size_t i = 0; i < N; ++i) {
    appended += rows[i].list_row >= kVanillaRowCount ? 1 : 0;
  }
  if (owns_all_rows ? N != total_rows : appended != total_rows - kVanillaRowCount) {
    return false;
  }
  return true;
}

// Rows backed by a cvar named by OptionRow::cvar, saved to settings.toml as
// soon as they change.
//
// A bool cvar as Off/On (index 0/1).
int32_t GetCvarBool(const OptionRow& row);
void CommitCvarBool(const OptionRow& row, int32_t value);
// An integer cvar, the index being the value itself.
int32_t GetCvarInt(const OptionRow& row);
void CommitCvarInt(const OptionRow& row, int32_t value);
// Either kind's compiled-in default.
int32_t GetCvarDefault(const OptionRow& row);

// Writes `cvar` to settings.toml, the same file the overlay saves to.
void SaveCvar(const char* cvar);

// The enhancements pages (native_enhancements.cpp): one per category, the
// shoulder buttons switching between them on the open screen.
const OptionsPage& EnhancementsOptionsPage();  // the first
// The page after (direction > 0) or before `current`, wrapping; null when
// `current` is not an enhancements page.
const OptionsPage* NeighbourEnhancementsPage(const OptionsPage& current, int direction);
// "ENHANCEMENTS", for the Help & Options entries.
const char16_t* EnhancementsMenuText();

// The randomizer pages (native_randomizer.cpp), the same way. Their rows edit
// a copy of the randomizer's options, which BeginRandomizerEdit takes afresh;
// A then randomizes with it (see the pages' `accept`).
const OptionsPage& RandomizerOptionsPage();  // the first
const OptionsPage* NeighbourRandomizerPage(const OptionsPage& current, int direction);
bool IsRandomizerPage(const OptionsPage& page);
void BeginRandomizerEdit();

}  // namespace nocturne
