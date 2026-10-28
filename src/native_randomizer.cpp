// nocturnerecomp: the randomizer pages of the game's own settings screen (main
// menu -> Randomizer, see main_menu.cpp): the options of the Randomizer
// overlay (rando_menu.h) as native rows, two pages switched with the shoulder
// buttons like the enhancements ones. Only what goes into a seed: fast warps,
// the map tracker and the like are enhancements.
//
// The rows edit a copy of the overlay's options, taken when the screen opens
// (BeginRandomizerEdit). A commits it: the Game row's Randomized generates a
// seed from it, applied once done, and Vanilla goes back to the vanilla game.
// B drops it. Typing a seed and reading the spoiler log stay in the overlay:
// here the Seed row's left/right rolls a new random seed, and back.
//
// Like the overlay, the pages only show the options that matter for what is
// chosen (Relayout): Min. Complexity for the presets that let the player pick
// it, the What to randomize page for the presets that don't fix it, and
// nothing but the Game row for the vanilla game. The rows are live, writing
// the copy as they change, so the layout follows each change at once.
//
// Labels are short: a row's label has to fit between the list's left edge and
// the value arrows, about 20 Latin characters at the list's scale.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "native_options_page.h"
#include "rando/rando.h"
#include "rando_menu.h"
#include "rando_runtime.h"
#include "ui_text.h"

namespace nocturne {

namespace {

// --- Text -------------------------------------------------------------------

constexpr NativeText kGameLabel{"rando_game"};
constexpr NativeText kGameValues[2] = {NativeText{"rando_game_vanilla"},
                                       NativeText{"rando_game_randomized"}};
constexpr NativeText kSeedLabel{"rando_seed"};
constexpr NativeText kPresetLabel{"rando_preset"};
constexpr NativeText kComplexityLabel{"rando_complexity"};
constexpr NativeText kForceNoComplexity{"rando_force_no_complexity"};
constexpr NativeText kRelics{"rando_relics"};
constexpr NativeText kStartingGear{"rando_starting_gear"};
constexpr NativeText kEquipment{"rando_equipment"};
constexpr NativeText kDrops{"rando_drops"};
constexpr NativeText kShop{"rando_shop"};
constexpr NativeText kCandles{"rando_candles"};

// --- The options being edited -------------------------------------------------

rando::Options g_options;
// The Game row: 0 vanilla, 1 randomized.
int32_t g_game = 1;

// The Seed row's values: the seed the screen opened with, then random ones,
// rolled the first time the row shows each. Left from the first wraps round
// to the last, so either direction gives a new seed and the other way back.
constexpr uint32_t kSeedChoices = 16;
std::array<std::string, kSeedChoices> g_seeds;
std::array<std::u16string, kSeedChoices> g_seed_texts;
int32_t g_seed_choice = 0;

const std::string& Seed(int32_t choice) {
  std::string& seed = g_seeds[choice];
  if (seed.empty()) {
    seed = rando::RandomSeedName();
  }
  return seed;
}

const char16_t* SeedText(int32_t choice) {
  g_seed_texts[choice] = Utf8ToUtf16(Seed(choice));
  return g_seed_texts[choice].c_str();
}

// Room for every preset (rando::Presets(), whose count is only known at run
// time); the row offers as many as there are.
constexpr uint32_t kMaxPresets = 16;

uint32_t PresetCount() {
  return static_cast<uint32_t>(rando::Presets().size());
}

const char16_t* PresetText(int32_t index) {
  const auto& presets = rando::Presets();
  return static_cast<size_t>(index) < presets.size() ? CachedUtf16(presets[index].name) : nullptr;
}

const rando::Preset& CurrentPreset() {
  const auto& presets = rando::Presets();
  return presets[std::min<size_t>(g_options.preset, presets.size() - 1)];
}

// What the overlay does on picking a preset: its own minimum complexity, and
// the What to randomize options it hides switched on. A no-op for the preset
// already chosen, so committing the row again (B, page switches) changes
// nothing.
void SelectPreset(int32_t index) {
  if (index == g_options.preset || index < 0 ||
      static_cast<size_t>(index) >= rando::Presets().size()) {
    return;
  }
  g_options.preset = index;
  const rando::Preset& preset = CurrentPreset();
  if (preset.picks_complexity) {
    g_options.complexity = preset.default_complexity;
  }
  if (preset.sets_normal_options) {
    g_options.relics = g_options.starting_gear = g_options.equipment = g_options.drops =
        g_options.shop = g_options.candles = true;
  }
}

// An on/off row's option, by its label.
struct BoolField {
  NativeText label;
  bool rando::Options::*field;
};
constexpr BoolField kBoolFields[] = {
    {kForceNoComplexity, &rando::Options::force_no_complexity},
    {kRelics, &rando::Options::relics},
    {kStartingGear, &rando::Options::starting_gear},
    {kEquipment, &rando::Options::equipment},
    {kDrops, &rando::Options::drops},
    {kShop, &rando::Options::shop},
    {kCandles, &rando::Options::candles},
};

bool rando::Options::* Field(const OptionRow& row) {
  for (const BoolField& f : kBoolFields) {
    if (std::string_view(f.label.key) == row.label.key) {
      return f.field;
    }
  }
  return nullptr;
}

constexpr OptionRow BoolRow(uint32_t list_row, NativeText label) {
  return {
      .list_row = list_row,
      .label = label,
      .value_count = 2,
      .values = kOffOnTexts,
      .get = [](const OptionRow& row) -> int32_t {
        auto field = Field(row);
        return field && g_options.*field ? 1 : 0;
      },
      .commit =
          [](const OptionRow& row, int32_t value) {
            if (auto field = Field(row)) {
              g_options.*field = value != 0;
            }
          },
      .reset = [](const OptionRow& row) -> int32_t {
        auto field = Field(row);
        return field && rando::Options{}.*field ? 1 : 0;
      },
      .live = true,
  };
}

// --- The pages ----------------------------------------------------------------

constexpr OptionRow kSeedRows[] = {
    {
        .list_row = 0,
        .label = kGameLabel,
        .value_count = 2,
        .values = kGameValues,
        .get = [](const OptionRow&) { return g_game; },
        .commit = [](const OptionRow&, int32_t value) { g_game = value; },
        .reset = [](const OptionRow&) -> int32_t { return 1; },
        .live = true,
    },
    {
        .list_row = 1,
        .label = kSeedLabel,
        .value_count = kSeedChoices,
        .value_text = &SeedText,
        .get = [](const OptionRow&) { return g_seed_choice; },
        .commit =
            [](const OptionRow&, int32_t value) {
              g_seed_choice = value;
              g_options.seed = Seed(value);
            },
        .reset = [](const OptionRow&) -> int32_t { return 0; },
        .live = true,
    },
    {
        .list_row = 2,
        .label = kPresetLabel,
        .value_count = kMaxPresets,
        .value_text = &PresetText,
        .live_value_count = &PresetCount,
        .get = [](const OptionRow&) { return g_options.preset; },
        .commit = [](const OptionRow&, int32_t value) { SelectPreset(value); },
        .reset = [](const OptionRow&) { return rando::Options{}.preset; },
        .live = true,
    },
    {
        // Only some presets let the player pick it; the others set their own.
        .list_row = 3,
        .label = kComplexityLabel,
        .min = 1,
        .max = 8,
        .get = [](const OptionRow&) { return g_options.complexity; },
        .commit = [](const OptionRow&, int32_t value) { g_options.complexity = value; },
        .reset = [](const OptionRow&) { return rando::Options{}.complexity; },
        .live = true,
    },
    BoolRow(4, kForceNoComplexity),
};

// What to randomize. Presets that fix these ignore them.
constexpr OptionRow kShuffleRows[] = {
    BoolRow(0, kRelics),    BoolRow(1, kStartingGear),
    BoolRow(2, kEquipment), BoolRow(3, kDrops),
    BoolRow(4, kShop),      BoolRow(5, kCandles),
};

// The same screen geometry as the enhancements pages.
constexpr uint32_t kVisibleRows = 7;
constexpr uint32_t kLeftBumperGlyph = 0x82012F50u;  // "BTN_LB"
constexpr NativeText kChangePagePrompt{"change_page_prompt"};

// A on any page: the options as edited, then the Game row's choice.
void Accept() {
  rando::Options options = g_options;
  const rando::Preset& preset =
      rando::Presets()[std::min<size_t>(options.preset, rando::Presets().size() - 1)];
  // As the overlay does on picking such a preset, which then hides them.
  if (preset.sets_normal_options) {
    options.relics = options.starting_gear = options.equipment = options.drops = options.shop =
        options.candles = true;
  }
  if (g_game) {
    GetRandoMenu().Randomize(options);
  } else {
    GetRandoMenu().SetOptions(options);
    if (GetRandoRuntime().state().active) {
      GetRandoRuntime().RequestRevert();
    }
  }
}

// The full tables, every row in place: what a layout picks its rows from.
static_assert(RowsAreWellFormed(kSeedRows, std::size(kSeedRows), true));
static_assert(RowsAreWellFormed(kShuffleRows, std::size(kShuffleRows), true));
static_assert(std::size(kSeedRows) <= kVisibleRows && std::size(kShuffleRows) <= kVisibleRows,
              "each page fits the screen without scrolling");
// Every option of the overlay but the seed, preset and complexity has a row.
static_assert(std::size(kBoolFields) == 7);

bool Relayout();

constexpr OptionsPage MakePage(NativeText title) {
  return {
      .visible_rows = kVisibleRows,
      .title = title,
      .owns_all_rows = true,
      .prompt_glyph = kLeftBumperGlyph,
      .accept = &Accept,
      .relayout = &Relayout,
  };
}

// The pages as laid out for the options being edited: the rows that matter,
// one after the other.
std::array<OptionRow, std::size(kSeedRows)> g_seed_page_rows;
std::array<OptionRow, std::size(kShuffleRows)> g_shuffle_page_rows;
constexpr NativeText kSeedTitle{"rando_title_seed"};
constexpr NativeText kSinglePageTitle{"rando_title"};
OptionsPage g_pages[] = {
    MakePage(kSeedTitle),
    MakePage(NativeText{"rando_title_shuffle"}),
};

// Which rows matter, as the overlay decides.
struct Shown {
  bool randomized = false;
  bool complexity = false;  // the preset lets the player pick it
  bool shuffle = false;     // the preset leaves What to randomize to the player
  bool operator==(const Shown&) const = default;
};

Shown CurrentShown() {
  const rando::Preset& preset = CurrentPreset();
  const bool randomized = g_game != 0;
  return {randomized, randomized && preset.picks_complexity,
          randomized && !preset.fixes_randomization};
}

Shown g_shown;
bool g_laid_out = false;

// Copies the rows of `all` that `shown` keeps into `out`, on consecutive
// lines, and points `page` at them.
template <size_t N, typename F>
void LayOut(const OptionRow (&all)[N], std::array<OptionRow, N>& out, OptionsPage& page,
            F&& shown) {
  uint32_t count = 0;
  for (const OptionRow& row : all) {
    if (shown(row)) {
      out[count] = row;
      out[count].list_row = count;
      ++count;
    }
  }
  page.rows = out.data();
  page.row_count = page.total_rows = count;
}

// OptionsPage::relayout for both pages.
bool Relayout() {
  const Shown shown = CurrentShown();
  if (g_laid_out && shown == g_shown) {
    return false;
  }
  g_shown = shown;
  g_laid_out = true;
  LayOut(kSeedRows, g_seed_page_rows, g_pages[0], [&](const OptionRow& row) {
    const std::string_view key = row.label.key;
    if (key == kGameLabel.key) {
      return true;
    }
    return key == kComplexityLabel.key ? shown.complexity : shown.randomized;
  });
  LayOut(kShuffleRows, g_shuffle_page_rows, g_pages[1],
         [&](const OptionRow&) { return shown.shuffle; });
  // The shoulder buttons' prompt, and the < > around the title that hint at
  // them, while there is another page to go to.
  const bool paged = g_pages[1].row_count > 0;
  for (OptionsPage& page : g_pages) {
    page.prompt_text = paged ? kChangePagePrompt : NativeText{};
  }
  g_pages[0].title = paged ? kSeedTitle : kSinglePageTitle;
  return true;
}

}  // namespace

const OptionsPage& RandomizerOptionsPage() {
  return g_pages[0];
}

const OptionsPage* NeighbourRandomizerPage(const OptionsPage& current, int direction) {
  if (!IsRandomizerPage(current)) {
    return nullptr;
  }
  // The pages with rows; an empty one is skipped, and with one page left
  // there is nowhere to go.
  std::array<const OptionsPage*, std::size(g_pages)> pages;
  int count = 0, at = 0;
  for (const OptionsPage& page : g_pages) {
    if (&page == &current) {
      at = count;
    }
    if (page.row_count > 0 || &page == &current) {
      pages[count++] = &page;
    }
  }
  if (count < 2) {
    return nullptr;
  }
  return pages[(at + (direction > 0 ? 1 : count - 1)) % count];
}

bool IsRandomizerPage(const OptionsPage& page) {
  for (const OptionsPage& p : g_pages) {
    if (&p == &page) {
      return true;
    }
  }
  return false;
}

void BeginRandomizerEdit() {
  g_options = GetRandoMenu().options();
  // What New Game gets: a seed, or one still being generated for it.
  g_game = GetRandoRuntime().state().new_game ||
                   rando::Generator::Get().status() == rando::Status::kRunning
               ? 1
               : 0;
  g_seeds = {};
  g_seed_choice = 0;
  if (g_options.seed.empty()) {
    g_options.seed = rando::RandomSeedName();
  }
  g_seeds[0] = g_options.seed;
  const auto& presets = rando::Presets();
  if (g_options.preset < 0 || static_cast<size_t>(g_options.preset) >= presets.size()) {
    g_options.preset = 0;
  }
  g_laid_out = false;
  Relayout();
}

}  // namespace nocturne
