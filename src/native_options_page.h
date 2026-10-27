// nocturnerecomp: the row tables native_options.cpp shows on the game's own
// settings screen. One screen, several pages: Help & Options -> Settings shows
// the settings page, Help & Options -> Enhancements the enhancements page
// (native_enhancements.cpp). See native_options.cpp for how a table becomes
// rows on that screen.
#pragma once

#include <cstddef>
#include <cstdint>

#include "native_ui.h"

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

  // Override rows only: force the stock setting this row displaced to its
  // maximum, so whatever now backs the row has the full range to work with
  // rather than being multiplied down by a leftover value nobody can reach
  // any more.
  bool pin_stock_to_max = false;

  // Override rows only: returning false leaves the row completely alone:
  // the stock row keeps its vanilla behaviour, model copy-back and all.
  bool (*available)() = nullptr;
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

  // Every row is this page's own: the stock Graphics/Volume model is neither
  // shown nor mirrored, and X resets the rows themselves instead of the
  // game's settings.
  bool owns_all_rows = false;

  // Y opens the stretch screen, with a prompt for it on the bar.
  bool stretch_prompt = false;
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

// The enhancements page (native_enhancements.cpp).
const OptionsPage& EnhancementsOptionsPage();

}  // namespace nocturne
