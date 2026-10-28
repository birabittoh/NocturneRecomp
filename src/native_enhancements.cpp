// nocturnerecomp: the enhancements page of the game's own settings screen
// (Help & Options -> Enhancements): every setting of the overlay's
// Enhancements tabs (settings.cpp), as native rows. Same cvars, same order, one
// page per tab with Other holding the cosmetic and game mode rows, and like
// there a change reaches the game on the next frame, except the game mode: a
// running game keeps its own, so that one waits for the next new game (see
// rando_runtime.h). The intro skips (intro_skips.cpp) follow at the end of
// Other.
//
// Labels are short: a row's label has to fit between the list's left edge and
// the value arrows, about 20 Latin characters at the list's scale.
#include <cstdint>
#include <random>
#include <string>

#include <rex/cvar.h>

#include "enhancements.h"
#include "native_options_page.h"
#include "rando/rando.h"

namespace nocturne {

namespace {

// --- Text -------------------------------------------------------------------

// The text is res/lang's [native] table. An on/off row's label is keyed by
// its cvar (see BoolRow); the others are here.
constexpr NativeText kAlchemyVats{"enh_alchemy_vats"};
constexpr NativeText kWingSmashColor{"enh_wing_smash_color"};
constexpr NativeText kGameMode{"enh_game_mode"};

// The Help & Options entry's text, in the stock title's all caps. Each page's
// own title is its category (enh_title_*).
constexpr NativeText kTitle{"enhancements_title"};

// enhancements.cpp's kWingSmashColorNames, in its order.
constexpr NativeText kWingSmashColors[rando::kWingSmashColorCount + 1] = {
    NativeText{"enh_color_default"},
    NativeText{"enh_color_magenta"},
    NativeText{"enh_color_violet"},
    NativeText{"enh_color_olive"},
    NativeText{"enh_color_red"},
    NativeText{"enh_color_blue"},
    NativeText{"enh_color_green"},
    NativeText{"enh_color_orange"},
    NativeText{"enh_color_maroon"},
};

// rando::GameMode, in its order.
constexpr NativeText kGameModes[] = {
    NativeText{"enh_mode_normal"},
    NativeText{"enh_mode_bat_master"},
    NativeText{"enh_mode_leg_day"},
    NativeText{"enh_mode_richter"},
};
static_assert(std::size(kGameModes) == static_cast<size_t>(rando::GameMode::kCount));

// --- Behaviour ----------------------------------------------------------------

// The vats row is On/Off over a shuffle seed: switching it on rolls a fresh
// nonzero seed, as the overlay's toggle does, and leaving it on keeps the
// current shuffle.
int32_t GetAlchemyVats(const OptionRow& row) { return GetCvarInt(row) != 0 ? 1 : 0; }

void CommitAlchemyVats(const OptionRow& row, int32_t value) {
  const bool shuffled = GetCvarInt(row) != 0;
  if ((value != 0) == shuffled) {
    return;
  }
  uint32_t seed = 0;
  while (value != 0 && seed == 0) {
    seed = std::random_device{}();
  }
  rex::cvar::SetFlagByName(row.cvar, std::to_string(seed), /*persist=*/true);
  SaveCvar(row.cvar);
}

// An on/off row over a bool cvar, labelled by the [native] string of the same
// name as the cvar.
constexpr OptionRow BoolRow(uint32_t list_row, const char* cvar) {
  return {
      .list_row = list_row,
      .label = NativeText{cvar},
      .value_count = 2,
      .values = kOffOnTexts,
      .get = &GetCvarBool,
      .commit = &CommitCvarBool,
      .reset = &GetCvarDefault,
      .cvar = cvar,
  };
}

// One table per page, in the overlay's tab order (settings.cpp).
constexpr OptionRow kQualityOfLifeRows[] = {
    BoolRow(0, "enh_fast_warps"),
    BoolRow(1, "enh_anti_freeze"),
    BoolRow(2, "enh_remove_clock_statue"),
    BoolRow(3, "enh_boost_drop_rates"),
    BoolRow(4, "enh_death_on_vacation"),
    BoolRow(5, "enh_skip_maria_alchemy"),
    BoolRow(6, "enh_skip_maria_silver_ring"),
};

constexpr OptionRow kFixesRows[] = {
    BoolRow(0, "enh_outer_wall_elevator"),
    BoolRow(1, "enh_scylla_softlock"),
    BoolRow(2, "enh_bridge_input"),
    BoolRow(3, "enh_catacombs_escape"),
    BoolRow(4, "enh_clock_room_door"),
    BoolRow(5, "enh_key_items_stay"),
    BoolRow(6, "enh_demon_card_door"),
};

constexpr OptionRow kItemsRows[] = {
    BoolRow(0, "enh_reverse_library_card"),
    BoolRow(1, "enh_infinite_items"),
    BoolRow(2, "enh_free_jewel_of_open"),
    BoolRow(3, "enh_cheap_duplicator"),
};

constexpr OptionRow kOtherRows[] = {
    BoolRow(0, "enh_map_tracker"),
    BoolRow(1, "enh_clear_file"),
    {
        .list_row = 2,
        .label = kAlchemyVats,
        .value_count = 2,
        .values = kOffOnTexts,
        .get = &GetAlchemyVats,
        .commit = &CommitAlchemyVats,
        .reset = &GetCvarDefault,
        .cvar = kAlchemyVatsCvar,
    },
    {
        .list_row = 3,
        .label = kWingSmashColor,
        .value_count = static_cast<uint32_t>(std::size(kWingSmashColors)),
        .values = kWingSmashColors,
        .get = &GetCvarInt,
        .commit = &CommitCvarInt,
        .reset = &GetCvarDefault,
        .cvar = kWingSmashColorCvar,
    },
    {
        .list_row = 4,
        .label = kGameMode,
        .value_count = static_cast<uint32_t>(std::size(kGameModes)),
        .values = kGameModes,
        .get = &GetCvarInt,
        .commit = &CommitCvarInt,
        .reset = &GetCvarDefault,
        .cvar = kGameModeCvar,
    },
    // Not on the overlay's tabs (intro_skips.cpp). skip_intros takes effect at
    // the next launch, skip_richter at the next new game.
    BoolRow(5, "skip_intros"),
    BoolRow(6, "skip_richter"),
};

// What fits on the screen's backdrop: the list starts at y=150, 30 apart, and
// an eighth row would run into the stone border above the prompt bar.
constexpr uint32_t kVisibleRows = 7;

// The glyph name "BTN_LB" in the guest image, and the prompt that goes with it.
constexpr uint32_t kLeftBumperGlyph = 0x82012F50u;
constexpr NativeText kChangePagePrompt{"change_page_prompt"};

template <size_t N>
constexpr OptionsPage MakePage(const OptionRow (&rows)[N], NativeText title) {
  return {
      .rows = rows,
      .row_count = static_cast<uint32_t>(N),
      .total_rows = static_cast<uint32_t>(N),
      .visible_rows = kVisibleRows,
      .title = title,
      .owns_all_rows = true,
      .prompt_text = kChangePagePrompt,
      .prompt_glyph = kLeftBumperGlyph,
  };
}

static_assert(RowsAreWellFormed(kQualityOfLifeRows, std::size(kQualityOfLifeRows), true));
static_assert(RowsAreWellFormed(kFixesRows, std::size(kFixesRows), true));
static_assert(RowsAreWellFormed(kItemsRows, std::size(kItemsRows), true));
static_assert(RowsAreWellFormed(kOtherRows, std::size(kOtherRows), true));
static_assert(std::size(kQualityOfLifeRows) <= kVisibleRows &&
                  std::size(kFixesRows) <= kVisibleRows && std::size(kItemsRows) <= kVisibleRows &&
                  std::size(kOtherRows) <= kVisibleRows,
              "each page fits the screen without scrolling");

// The titles carry < > to hint at the shoulder buttons.
constexpr OptionsPage kPages[] = {
    MakePage(kQualityOfLifeRows, NativeText{"enh_title_qol"}),
    MakePage(kFixesRows, NativeText{"enh_title_fixes"}),
    MakePage(kItemsRows, NativeText{"enh_title_items"}),
    MakePage(kOtherRows, NativeText{"enh_title_other"}),
};

}  // namespace

const OptionsPage& EnhancementsOptionsPage() {
  // Every bool cvar of the overlay's tabs has a row here, then its three
  // others, then the two intro skips.
  static_assert(std::size(kQualityOfLifeRows) + std::size(kFixesRows) + std::size(kItemsRows) +
                    std::size(kOtherRows) ==
                static_cast<size_t>(rando::Enhancement::kCount) + 5);
  return kPages[0];
}

const OptionsPage* NeighbourEnhancementsPage(const OptionsPage& current, int direction) {
  const auto count = static_cast<int>(std::size(kPages));
  for (int i = 0; i < count; ++i) {
    if (&kPages[i] == &current) {
      return &kPages[((i + (direction > 0 ? 1 : count - 1)) % count)];
    }
  }
  return nullptr;
}

const char16_t* EnhancementsMenuText() {
  return kTitle.Get();
}

}  // namespace nocturne
