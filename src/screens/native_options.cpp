// native_options: shows pages of our own rows on the game's own settings
// screen (main menu or pause -> help & options -> settings, and -> enhancements
// for the second page), cycled in place with left/right exactly like the stock
// "Graphics" and "Volume" rows.
//
// ---------------------------------------------------------------------------
// Adding a row or a page
// ---------------------------------------------------------------------------
//
// A page is an OptionsPage over a table of OptionRows (native_options_page.h):
// g_settings_page below, and the enhancements page in native_enhancements.cpp.
// Append an entry to a table and you are done. Widget creation, layout,
// left/right cycling with wraparound, scrolling a page longer than the screen,
// commit on A, re-seed on B/X and the localized label are all handled
// generically from the table.
//
// A row is either:
//   * enumerated: give it `value_count` and `values` (the localized text per
//     value) or `value_text`; indices run 0..value_count-1, and this file
//     paints the text; or
//   * numeric: leave those empty and fill in min/max/step/multiplier; the
//     game's own renderer prints `multiplier * value`, same as the stock Volume
//     row, and this file only paints the label.
//
// `get` supplies the value to show when the screen opens; `commit` is handed
// the value when the player accepts with A. A row backed by a single cvar can
// name it in `cvar` and use the generic Get/CommitCvar* helpers.
//
// A new page also needs a way in: see help_options_menu.cpp, which picks the
// page with SelectSettingsPage before the screen opens.
//
// ---------------------------------------------------------------------------
// How that screen is built (all derived from the pinned build's default.xex)
// ---------------------------------------------------------------------------
//
// Front-end screens live in a 22-entry array built by the UI manager's init
// (sub_825ABED0); the index into that array is the "page" number screens pass
// to each other via sub_825CE8E8(8, 0, <page>, ...). Page 17 is the
// "Change Screen Size" screen graphics_settings.cpp already hooks, and the
// screen that navigates to it -- handler sub_825B3F58, widget builder
// sub_825B4650 -- is this settings screen.
//
// The rows are not hardcoded. The builder creates a generic *option list*
// widget (ctor sub_825D3200, vtable off_82016E98) and binds it to a settings
// model hanging off the UI manager at +312:
//
//   sub_825D32B0(list, x=115, y=150, count=3, spacing=30, ...)
//       creates, per row, a label widget (list+648+4*i) and a value widget
//       (list+760+4*i), marks the row cyclable (byte at list+584+i), and
//       lays them out top to bottom. The list has room for 16 rows.
//   sub_825D36B8(list, entries, count)
//       binds an array of 96-byte setting entries (model+12, count model+8)
//       and renders each row's current value into its value widget.
//
// Each 96-byte entry is a self-describing spinner:
//   +0  label string id      +12 min value        +24 display multiplier
//   +4  current value        +16 max value        +32.. inline array of
//   +20 step                                      per-value string ids
//                                                 (all zero => render the
//                                                  number itself)
//
// Left/right is sub_825D35C0(list, +-1): it calls sub_825800F8(entry, dir),
// which does `value += step * dir` and wraps at min/max, then re-renders just
// that row's text. That is the in-place cycling these rows need, and we get
// it for free by making each one a real entry in that array.
//
// So adding rows is an *append*: raise the count at the list-setup call, hand
// the bind call an extended copy of the table, and paint our rows' own text
// over the widgets the list just created. Appending *after* the stock rows
// matters: past index 2 a row draws the arrows and takes A as "accept and
// close", which is what the stock rows 0/1 do too.
//
// Row 2 is the exception, and is special-cased throughout the screen because
// it *was* "Change Screen Size...", the one row that opened another screen
// (page 17) rather than cycling a value. It is now the Preset row (see the
// "Preset" section below), which cycles like any other, so all three of those
// special cases are undone: the page-17 jump and the prompt-bar hint in
// NativeOptions_SettingsEvent, and the hidden left/right arrows in
// NativeOptions_SettingsUpdate. The screen itself is still reachable, but only
// through Y, which this file posts the page switch for directly.
//
// Note this screen is *shared*: on A or B its handler checks the in-game flag
// (byte at 0x82E4F808+29) and returns to page 18 (the pause menu) when a save
// is loaded, or page 4 (the main menu) when not. One instance, two entry
// points -- so these rows deliberately show in the main-menu settings as well
// as the paused one. That is distinct from the pre-game "Graphics"-only
// screen, which is a different screen entirely and is not touched here.
#include "screens/native_options.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "core/game_state.h"
#include "screens/graphics_settings.h"
#include "screens/native_options_page.h"
#include "ui/native_ui.h"
#include "core/settings.h"
#include "core/target.h"
#include "jp/jp_artwork.h"
#include "jp/jp_text.h"
#include "jp/jp_voices.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "core/function_hooks.h"

namespace nocturne {

namespace {

// ---------------------------------------------------------------------------
// Guest addresses and layout
// ---------------------------------------------------------------------------

// The four sub_825Dxxxx functions are shared by every option list in the game
// (the controls screen uses them too), so each hook filters on the specific
// list instance we claimed -- see g_list.
constexpr uint32_t kListSetupFnAddr = 0x825D32B0u;         // (list, x, y, count, spacing, ...)
constexpr uint32_t kBindRowsFnAddr = 0x825D36B8u;          // (list, entries, count)
constexpr uint32_t kCycleRowFnAddr = 0x825D35C0u;          // (list, direction)
constexpr uint32_t kMoveSelectionFnAddr = 0x825D40E0u;     // (list, direction, controller)
constexpr uint32_t kEnableRowFnAddr = 0x825D3530u;         // (list, row)
constexpr uint32_t kListUpdateFnAddr = 0x825D38A8u;        // (list, delta)
constexpr uint32_t kSetWidgetColourFnAddr = 0x825D2138u;   // (widget, argb)
constexpr uint32_t kSettingsEventFnAddr = 0x825B3F58u;     // (screen, message)
constexpr uint32_t kSettingsActivateFnAddr = 0x825B43E0u;  // (screen, user_index)
constexpr uint32_t kSettingsUpdateFnAddr = 0x825B4CC0u;    // (screen, delta)
constexpr uint32_t kSetWidgetTextByIdFnAddr = 0x825D1EE8u;  // (widget, string_id)
constexpr uint32_t kSetWidgetTextFnAddr = 0x825D1EE0u;     // (widget, utf16_text)
// Formats a setting entry's current value as display text: the number, or
// the localized string an enumerated stock row (Graphics) shows for it.
constexpr uint32_t kFormatEntryValueFnAddr = 0x82580060u;  // (entry, utf16_out, out_chars)
constexpr uint32_t kEntryValueChars = 0x400;
constexpr uint32_t kAllocFnAddr = 0x82576950u;             // (size) -> pointer

// The screen's own widget builder, and the prompt-bar pieces it uses. See
// NativeOptions_PromptBarLayout for how the fourth prompt is added.
constexpr uint32_t kPromptBarLayoutFnAddr = 0x825CA820u;  // (x, y, width, w1, w2, w3)
constexpr uint32_t kPromptCtorFnAddr = 0x825D1DB0u;       // (memory, parent, flag) -> prompt
constexpr uint32_t kPromptSetGlyphFnAddr = 0x825D1EF8u;   // (prompt, image)
constexpr uint32_t kPromptShowGlyphFnAddr = 0x825D2010u;  // (prompt)
constexpr uint32_t kPromptTextOffsetFnAddr = 0x825D2018u; // (prompt, dx, dy)
constexpr uint32_t kPromptSetPosFnAddr = 0x825D1FA8u;     // (prompt, x, y)
constexpr uint32_t kTextWidthFnAddr = 0x825CF008u;        // (text_widget) -> pixels
constexpr uint32_t kImageWidgetCtorFnAddr = 0x825D0C48u;  // (widget, parent, x, y, image)
constexpr uint32_t kFindImageFnAddr = 0x825CEB68u;        // (image_bank, name) -> image

// A prompt is a glyph widget plus a text widget, with the text drawn at a
// fixed offset from the glyph (kPromptTextOffsetFnAddr sets it; the stock
// prompts all use 30,5).
constexpr uint32_t kPromptGlyphOffset = 544;
constexpr uint32_t kPromptTextOffset = 548;
constexpr uint32_t kPromptGlyphWidthOffset = 536;
constexpr uint32_t kPromptSize = 552;
constexpr uint32_t kPromptTextDx = 30;
constexpr uint32_t kPromptTextDy = 5;

// Where the bar itself sits, and the pointers the builder feeds it. The
// builder lays it out over x=0..640 at y=423; LayoutPromptBar spreads it wider,
// over the margins the 4:3 panel leaves free in the widescreen frame (the same
// ones the description column uses), so five prompts still have room.
constexpr int32_t kPromptBarX = -90;
constexpr uint32_t kPromptBarY = 423;
constexpr uint32_t kPromptBarWidth = 820;

// The image bank the glyph names are resolved against, the prompt text colour,
// and the name of the Y glyph. The glyph names are single-character strings in
// a descending-letter table ("Z" 0x82202270, "Y" 0x82202274, "X" 0x82202278) --
// the stock bar uses the X, A and B entries of it.
constexpr uint32_t kImageBankPtrAddr = 0x82E7A570u;
constexpr uint32_t kPromptColourAddr = 0x82895098u;
constexpr uint32_t kYGlyphNameAddr = 0x82202274u;

// Every front-end page change goes through this one queue push -- both the way
// in to the stretch screen (which this file now posts itself, see
// NativeOptions_SettingsEvent's Y handling) and the pre-game watchdog switch
// that gets filtered back out. See NativeOptions_PostEvent.
constexpr uint32_t kPostEventFnAddr = 0x825CE8E8u;  // (class, arg1, page, controller)
constexpr uint32_t kEventClassPageSwitch = 8;

// The exact call site of the front-end watchdog's page switch, inside the UI
// manager's per-frame update (sub_825AAE90). Filtering on the
// return address keeps the suppression to that one post -- every other switch
// the manager makes, including the genuine "user signed out" one a few lines
// above it, goes through untouched.
constexpr uint32_t kWatchdogPostLr = 0x825AB030u;

// Front-end page index of the "Change Screen Size" screen, and where the
// manager keeps the current one.
constexpr uint32_t kResolutionPage = 17;
// The main menu, and the Help & Options screen it opens.
constexpr uint32_t kMainMenuPage = 1;
constexpr uint32_t kMainHelpOptionsPage = 4;
constexpr uint32_t kCurrentPageOffset = 4;

// The settings screen's widget builder. Used only as an address *range* to
// recognise its own call into the shared list-setup function: that call is a
// virtual dispatch, so the return address is all we have to tell this list
// apart from the controls screen's (which is set up identically, 96-byte
// entries and all, and must be left alone).
constexpr uint32_t kSettingsBuildFnAddr = 0x825B4650u;
constexpr uint32_t kSettingsBuildFnSize = 0x4D4u;

// Option-list instance layout (see the header comment above).
constexpr uint32_t kListSelectedRow = 536;
constexpr uint32_t kListRowLabels = 648;
constexpr uint32_t kListRowValues = 760;
// The list's own idea of normal row text scale (float), set to 0.75 by both
// sub_825D2C48 and sub_825D32B0. See SetRowText for why we reassert it.
constexpr uint32_t kListTextScale = 560;
// Normal (unhighlighted) row colour.
constexpr uint32_t kListNormalColour = 608;
// A row widget wraps an inner text widget, which is where the text scale
// sub_825CFC68/sub_825CEFA0 write actually lands.
constexpr uint32_t kWidgetInnerTextOffset = 548;
constexpr uint32_t kWidgetTextScaleOffset = 536;
// How many label widgets the list's own Update resets per frame -- hardcoded
// there, and the reason NativeOptions_ListUpdate exists.
constexpr uint32_t kStockLabelResetCount = 3;
constexpr float kFallbackTextScale = 0.75f;

// Setting-entry layout.
constexpr uint32_t kEntryStride = 96;
constexpr uint32_t kEntryLabelId = 0;
constexpr uint32_t kEntryValue = 4;
constexpr uint32_t kEntryMin = 12;
constexpr uint32_t kEntryMax = 16;
constexpr uint32_t kEntryStep = 20;
constexpr uint32_t kEntryMultiplier = 24;

// The screen's own option list, off the screen instance.
constexpr uint32_t kScreenListOffset = 560;

// The screen's title, and how its Activate sets it: with a profile signed in
// (the app object's vtable slot 0x6C, asked about the opening user), string
// 0xBA formatted with the gamertag and set as literal text; otherwise plain
// string 0xB9. Then it centres the title on x=320 by its measured width.
constexpr uint32_t kScreenTitleOffset = 564;
constexpr uint32_t kAppSignedInVtableSlot = 0x6C;
constexpr uint32_t kGetUserNameFnAddr = 0x825D75A0u;   // (user, buffer, size)
constexpr uint32_t kFormatStringFnAddr = 0x82587290u;  // (out, out_chars, id, arg, 0)
constexpr uint32_t kSetTextFnAddr = 0x825CEE40u;       // (text_widget, utf16_text)
constexpr uint32_t kSetTextByIdFnAddr = 0x825CFC68u;   // (text_widget, string_id)
constexpr uint32_t kTitleStringId = 0xB9;
constexpr uint32_t kTitleWithNameStringId = 0xBA;
constexpr uint32_t kTitleChars = 0x400;
constexpr uint32_t kUserNameBytes = 0x20;
constexpr uint32_t kTitleCenterX = 0x140;

// Rows the stock Activate greys out until a save is loaded: Volume and the
// row that was "Change Screen Size...". See NativeOptions_SettingsActivate.
constexpr uint32_t kGatedFirstRow = 1;
constexpr uint32_t kGatedLastRow = 2;

// The stock "Change Screen Size..." row's line. Still the list row the screen
// special-cases as "the one that opens another screen", whatever row of
// whichever page it shows, which is what NativeOptions_SettingsEvent,
// NativeOptions_SettingsUpdate and NativeOptions_ListUpdate undo.
constexpr uint32_t kSubmenuRow = 2;

// The screen's prompt-bar widget, and the string ids the screen sets it to:
// sub_825B3F58 opens with `set_text_by_id(screen+584, selected == 2 ? 9 : 4)`
// -- id 9 being the "opens a submenu" hint that row no longer deserves.
constexpr uint32_t kScreenPromptOffset = 584;
constexpr uint32_t kPromptOrdinaryRowStringId = 4;

// The screen's left/right arrow widgets, and the x positions sub_825B4CC0
// gives them for an ordinary row. Their y comes from the selected row's own
// value widget (+8), plus 2.
constexpr uint32_t kScreenLeftArrowOffset = 544;
constexpr uint32_t kScreenRightArrowOffset = 548;
constexpr uint32_t kArrowLeftX = 360;
constexpr uint32_t kArrowRightX = 525;
constexpr uint32_t kWidgetXOffset = 4;
constexpr uint32_t kWidgetYOffset = 8;
constexpr uint32_t kArrowYBias = 2;

// An image widget, as the builder makes the arrows: its size, and the image's
// width as the constructor copies it in. The arrows' image names in the
// guest image ("arrowsLTn" and its right-hand twin).
constexpr uint32_t kImageWidgetSize = 612;
constexpr uint32_t kImageWidgetWidthOffset = 536;
constexpr uint32_t kLeftArrowImageName = 0x82013984u;
constexpr uint32_t kRightArrowImageName = 0x82013978u;

// The app object and its menu latch (kAppObjectPtrAddr, InGameLatchAddress)
// live in game_state.h.

// The UI manager, again a pointer stored at this address.
constexpr uint32_t kUiManagerPtrAddr = 0x82E7A02Cu;

// Message layout for a screen's event handler: word 0 is the event class
// (0 = button press) and word 2 is the button. 4/5/6 are A/B/X, matching the
// screen's own "X defaults, A accept, B cancel" prompt bar.
constexpr uint32_t kMsgClass = 0;
constexpr uint32_t kMsgButton = 8;
// Word 3: which controller sent it. Every page switch the screen posts passes
// this straight through.
constexpr uint32_t kMsgController = 12;
constexpr uint32_t kMsgClassButton = 0;
constexpr uint32_t kButtonAccept = 4;
constexpr uint32_t kButtonCancel = 5;
constexpr uint32_t kButtonDefaults = 6;
// Y, which this screen receives and ignores -- there is no fourth prompt in
// its bar. Repurposed as the way back into the stretch screen the Preset row
// displaced; see NativeOptions_SettingsEvent.
constexpr uint32_t kButtonStretchScreen = 7;
// The shoulder buttons, which switch between the enhancements pages. By the
// same ordering after Y; static RE could not confirm it, so an id the screen
// doesn't handle is logged (see NativeOptions_SettingsEvent).
constexpr uint32_t kButtonPreviousPage = 8;
constexpr uint32_t kButtonNextPage = 9;

// True if ANY cvar changed this session (through this screen or otherwise --
// GetPendingRestartFlags() doesn't distinguish the source) still needs a
// restart to take effect. Mirrors settings.cpp's own AnyPendingRestart, which
// drives the ImGui overlay's "Some changes require a restart to take effect."
// banner -- same underlying state, just read from the guest-driven settings
// screen instead of ImGui.
bool AnyPendingRestart() { return !rex::cvar::GetPendingRestartFlags().empty(); }

// Set once by NativeOptions::Bind from Runtime::user_data_root(); empty means
// Bind() couldn't resolve it (or hasn't run yet), in which case SaveCvar just
// no-ops rather than guessing a path.
std::filesystem::path g_user_settings_path;

}  // namespace

// Persists a single cvar to the same settings.toml the ImGui overlay writes
// (see settings.cpp's SaveBasic/user_settings_path). Setting a cvar with
// persist=true only flags it as *worth* saving next time someone saves:
// unlike the overlay, which saves itself after every change, nothing here was
// actually writing the file, so a value picked from this screen looked
// committed (it applied) but reverted to whatever was last saved on the next
// launch.
//
// Resolved from g_user_settings_path (see NativeOptions::Bind), NOT the
// `user_data_root` *cvar* -- that cvar (runtime.h) is only a command-line
// override and defaults to "", so reading it back here would silently no-op
// on every normal launch. The actual resolved path lives on the Runtime
// object (Runtime::user_data_root()), which Bind() only has once, so it's
// cached in a global rather than re-derived per commit.
void SaveCvar(const char* cvar) {
  if (g_user_settings_path.empty()) {
    return;
  }
  rex::cvar::SaveConfigSubset(g_user_settings_path, {cvar});
}

int32_t GetCvarBool(const OptionRow& row) {
  return rex::cvar::GetFlagByName(row.cvar) == "true" ? 1 : 0;
}

void CommitCvarBool(const OptionRow& row, int32_t value) {
  const char* text = value ? "true" : "false";
  if (rex::cvar::GetFlagByName(row.cvar) == text) {
    return;
  }
  rex::cvar::SetFlagByName(row.cvar, text, /*persist=*/true);
  SaveCvar(row.cvar);
}

int32_t GetCvarInt(const OptionRow& row) {
  return static_cast<int32_t>(std::atoi(rex::cvar::GetFlagByName(row.cvar).c_str()));
}

void CommitCvarInt(const OptionRow& row, int32_t value) {
  const std::string text = std::to_string(value);
  if (rex::cvar::GetFlagByName(row.cvar) == text) {
    return;
  }
  rex::cvar::SetFlagByName(row.cvar, text, /*persist=*/true);
  SaveCvar(row.cvar);
}

int32_t GetCvarDefault(const OptionRow& row) {
  const auto* info = rex::cvar::GetFlagInfo(row.cvar);
  if (!info) {
    return 0;
  }
  if (info->default_value == "true" || info->default_value == "false") {
    return info->default_value == "true" ? 1 : 0;
  }
  return static_cast<int32_t>(std::atoi(info->default_value.c_str()));
}

namespace {

// ---------------------------------------------------------------------------
// The settings page
// ---------------------------------------------------------------------------

// --- Fullscreen and VSync ---------------------------------------------------

// Both apply live: `fullscreen` through the change callback
// ReXApp::SetupPresentation registers, 
// (see ApplyVsyncIfChanged). So both rows are `live`, same as Volume.
//
// The shared string table has no id for either label, so they are res/lang
// [native] strings rather than looked up by id. Title case to match the stock rows, not the
// all-caps prompt bar.
constexpr NativeText kFullscreenLabel{"fullscreen_label"};

// --- Gamma ----------------------------------------------------------------

// The same steps as the overlay's slider (settings.h), applied live: the
// picture behind the menu is the feedback, and B puts the original back.
constexpr NativeText kGammaLabel{"gamma_label"};

constexpr const char16_t* kGammaTexts[] = {u"0.8", u"0.9", u"1.0", u"1.1",
                                           u"1.2", u"1.3", u"1.4"};

const char16_t* GammaText(int32_t value) {
  return static_cast<size_t>(value) < std::size(kGammaTexts) ? kGammaTexts[value] : nullptr;
}

int32_t GetGammaIndex(const OptionRow&) { return DisplayGammaOptionIndex(); }

void CommitGammaIndex(const OptionRow& row, int32_t value) {
  if (value == DisplayGammaOptionIndex())
    return;
  SetDisplayGammaOption(value);
  SaveCvar(row.cvar);
}

// --- Screen filter --------------------------------------------------------

// The overlay's combo (settings.h), applied live like gamma.
constexpr NativeText kScreenFilterLabel{"screen_filter_label"};

constexpr NativeText kScreenFilterTexts[] = {NativeText{"filter_none"},
                                             NativeText{"filter_scanlines"},
                                             NativeText{"filter_lcd_grid"},
                                             NativeText{"filter_smooth"}};

// The built-in options, then the mods' filters by their own names.
const char16_t* ScreenFilterText(int32_t value) {
  if (static_cast<size_t>(value) < std::size(kScreenFilterTexts)) {
    return kScreenFilterTexts[value].Get();
  }
  const CustomScreenFilter* custom = ScreenFilterOptionCustom(value);
  return custom ? custom->name_u16.c_str() : nullptr;
}

uint32_t ScreenFilterChoiceCount() { return static_cast<uint32_t>(ScreenFilterOptionCount()); }

int32_t GetScreenFilterIndex(const OptionRow&) { return ScreenFilterOptionIndex(); }

void CommitScreenFilterIndex(const OptionRow& row, int32_t value) {
  if (value == ScreenFilterOptionIndex())
    return;
  SetScreenFilterOption(value);
  SaveCvar(row.cvar);
}

constexpr NativeText kScreenFilterIntensityLabel{"screen_filter_intensity_label"};

// ScreenFilterIntensityOptionPercent's steps, as text.
constexpr const char16_t* kScreenFilterIntensityTexts[] = {
    u"5%",  u"10%", u"15%", u"20%", u"25%", u"30%", u"35%", u"40%", u"45%", u"50%",
    u"55%", u"60%", u"65%", u"70%", u"75%", u"80%", u"85%", u"90%", u"95%", u"100%"};

const char16_t* ScreenFilterIntensityText(int32_t value) {
  return static_cast<size_t>(value) < std::size(kScreenFilterIntensityTexts)
             ? kScreenFilterIntensityTexts[value]
             : nullptr;
}

int32_t GetScreenFilterIntensityIndex(const OptionRow&) {
  return ScreenFilterIntensityOptionIndex();
}

void CommitScreenFilterIntensityIndex(const OptionRow& row, int32_t value) {
  if (value == ScreenFilterIntensityOptionIndex())
    return;
  SetScreenFilterIntensityOption(value);
  SaveCvar(row.cvar);
}

// --- Language -------------------------------------------------------------

// `user_language` applies live (see language_switch.h): the next guest frame
// after a change swaps the game's strings and font over and re-texts every
// screen, this one included -- see RefreshNativeOptionsText.
constexpr const char* kLanguageCvar = "user_language";

// The actual language *list* isn't known until mods finish registering via
// settings.h's GetLanguageOptions: the same six built-in entries plus
// whatever mods appended to the curated overlay's Language dropdown (see
// making-mods.md's "Adding a Language dropdown entry"). So this row works like
// the Preset row's "Custom" tail entry: a fixed capacity sized for well more
// than anyone's likely to register, populated once lazily, with
// `live_value_count` telling the row how many of those are in use right now.
constexpr uint32_t kMaxLanguageOptions = 32;

// Populated once by EnsureLanguageOptionsBuilt(). Parallel arrays: index i's
// id and UTF-16 text describe the same option.
std::vector<std::string> g_language_ids;
std::vector<std::u16string> g_language_texts;
bool g_language_options_built = false;

// Mod-supplied labels are plain ASCII display names (see making-mods.md's
// language-option example), so a byte-widening conversion is enough.
std::u16string AsciiToUtf16(const std::string& s) {
  std::u16string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    out.push_back(static_cast<char16_t>(c));
  }
  return out;
}

void EnsureLanguageOptionsBuilt() {
  if (g_language_options_built) {
    return;
  }
  g_language_options_built = true;

  const std::vector<nocturne::LanguageOption> options = nocturne::GetLanguageOptions();
  const auto count =
      std::min<uint32_t>(static_cast<uint32_t>(options.size()), kMaxLanguageOptions);
  if (options.size() > kMaxLanguageOptions) {
    REXLOG_WARN(
        "[native_options] {} language options registered, only the first {} fit the in-game "
        "Language row",
        options.size(), kMaxLanguageOptions);
  }
  for (uint32_t i = 0; i < count; ++i) {
    g_language_ids.push_back(options[i].id);
    g_language_texts.push_back(AsciiToUtf16(options[i].label));
  }
}

uint32_t LanguageOptionCount() {
  EnsureLanguageOptionsBuilt();
  return static_cast<uint32_t>(g_language_ids.size());
}

const char16_t* LanguageText(int32_t index) {
  EnsureLanguageOptionsBuilt();
  return static_cast<size_t>(index) < g_language_texts.size()
             ? g_language_texts[static_cast<size_t>(index)].c_str()
             : nullptr;
}

// 言/語 aren't in font_ja.xpr's glyph atlas, so Japanese uses the katakana
// loanword instead.
constexpr NativeText kLanguageLabel{"language_label"};

int32_t GetLanguageIndex(const OptionRow&) {
  EnsureLanguageOptionsBuilt();
  const std::string current = rex::cvar::GetFlagByName(kLanguageCvar);
  for (size_t i = 0; i < g_language_ids.size(); ++i) {
    if (g_language_ids[i] == current) {
      return static_cast<int32_t>(i);
    }
  }
  // An id the row doesn't know (e.g. a mod-added language whose owning mod
  // got disabled after the cvar was saved) falls back to the first entry
  // rather than clamping to the last.
  return 0;
}

void CommitLanguageIndex(const OptionRow&, int32_t index) {
  EnsureLanguageOptionsBuilt();
  if (index < 0 || static_cast<size_t>(index) >= g_language_ids.size()) {
    return;
  }
  const std::string& value = g_language_ids[static_cast<size_t>(index)];
  if (rex::cvar::GetFlagByName(kLanguageCvar) == value) {
    return;
  }
  rex::cvar::SetFlagByName(kLanguageCvar, value, /*persist=*/true);
  SaveCvar(kLanguageCvar);
  REXLOG_INFO("[native_options] language set to {}", value);
}

// --- Volume (takes over the stock row 1) ------------------------------------

// The stock Volume row drives the game's *own* internal mix, which sits
// upstream of everything the SDK does. Pointing it at the SDK's master
// `audio_volume` instead gives one control that covers every output path
// (XAudio, XMP music, mod SFX) rather than just the guest's own mixer.
//
// The game's internal volume is pinned to its maximum (pin_stock_to_max) so
// the cvar gets the full range: the two gains multiply, so leaving the guest
// mixer at a saved half-volume would cap this row's 100% at 50% of what the
// hardware can do, with no way left to raise it.
constexpr const char* kAudioVolumeCvar = "audio_volume";

// The stock row's own range, kept identical so the row still reads 0-10 and
// the number the game renders is unchanged.
constexpr int32_t kVolumeSteps = 10;

constexpr NativeText kVolumeLabel{"volume_label"};

int32_t GetMasterVolumeStep(const OptionRow&) {
  const double amplitude = std::atof(rex::cvar::GetFlagByName(kAudioVolumeCvar).c_str());
  const int percent = VolumePercentFromAmplitude(amplitude);
  const int32_t step = static_cast<int32_t>(std::lround(percent * kVolumeSteps / 100.0));
  return std::clamp(step, int32_t{0}, kVolumeSteps);
}

void CommitMasterVolumeStep(const OptionRow&, int32_t step) {
  step = std::clamp(step, int32_t{0}, kVolumeSteps);
  const int percent = static_cast<int>(std::lround(step * 100.0 / kVolumeSteps));
  const double amplitude = VolumeAmplitudeFromPercent(percent);
  rex::cvar::SetFlagByName(kAudioVolumeCvar, std::to_string(amplitude), /*persist=*/true);
  SaveCvar(kAudioVolumeCvar);
}

// --- Preset (takes over the stock row 2) ------------------------------------

// The stock row 2 was "Change Screen Size...", the one row that navigated to
// another screen (page 17) instead of cycling a value. That screen's entire
// job was picking a stretch preset -- graphics_settings.cpp already cycled its
// own catalog there with X -- so the row is replaced outright by a normal
// spinner over that same catalog. The screen itself is still one Y away.
//
// Taking over row 2 specifically means neutralising the three places the
// stock screen treats it as "the row that opens a screen" rather than as an
// ordinary row -- see NativeOptions_SettingsEvent (the page-17 jump and the
// prompt-bar hint) and NativeOptions_SettingsUpdate (the hidden arrows).
//
// commit() only *queues* the preset: applying it writes guest memory and
// calls a guest function, which needs a live PPCContext this path doesn't own,
// so graphics_settings applies it on its next per-frame tick. That is also why
// this row is `live` -- the player sees each preset as they cycle onto it, one
// frame later, and B puts the original back the same way.
constexpr NativeText kPresetLabel{"preset_label"};

// The Y prompt's text. All caps to match the prompt bar's own strings (the
// rows are title case, the bar is not), and kept short because a fourth prompt
// has to share the bar's 640px with three others: LayoutPromptBar silently
// leaves the bar alone if the four don't fit, so a long translation would cost
// the prompt entirely rather than overflow.
constexpr NativeText kCustomPromptLabel{"custom_prompt_label"};

// --- Voices and artwork -----------------------------------------------------

// Whose release to use, as the overlay's rows (settings.cpp): only with both
// installed is there a choice. Otherwise the row is locked on the one release
// there is, and the description column says what to do about it.
constexpr const char* kReleaseValues[] = {"auto", "jp", "world"};
constexpr NativeText kReleaseTexts[3] = {NativeText{"release_auto"}, NativeText{"release_jp"},
                                         NativeText{"release_world"}};
constexpr NativeText kVoicesLabel{"voices_label"};
constexpr NativeText kArtworkLabel{"artwork_label"};
constexpr NativeText kPrologueLabel{"prologue_label"};
constexpr NativeText kChoiceNeedsBoth{"choice_needs_both"};

bool ReleaseChoiceAvailable(const OptionRow& row) {
  const std::string_view cvar = row.cvar;
  if (cvar == "voices")
    return JpVoicesAvailable();
  if (cvar == "prologue")
    return JpPrologueAvailable();
  return JpArtworkAvailable();
}

bool ReleaseChoiceLocked(const OptionRow& row) {
  return !ReleaseChoiceAvailable(row);
}

int32_t GetReleaseChoice(const OptionRow& row) {
  const std::string current = ReleaseChoiceAvailable(row)
                                  ? rex::cvar::GetFlagByName(row.cvar)
                                  : (TargetReleaseId() == "xbla-jp" ? "jp" : "world");
  for (int32_t i = 0; i < 3; ++i) {
    if (current == kReleaseValues[i]) {
      return i;
    }
  }
  return 0;
}

void CommitReleaseChoice(const OptionRow& row, int32_t value) {
  if (!ReleaseChoiceAvailable(row) || value < 0 || value > 2 ||
      rex::cvar::GetFlagByName(row.cvar) == kReleaseValues[value]) {
    return;
  }
  rex::cvar::SetFlagByName(row.cvar, kReleaseValues[value], /*persist=*/true);
  SaveCvar(row.cvar);
}

int32_t ResetReleaseChoice(const OptionRow&) {
  return 0;
}

// --- The table itself -------------------------------------------------------

// The game's own settings, in its table: the rows below stand in for them
// wherever they sit (OptionRow::stock_row).
constexpr int32_t kStockGraphicsRow = 0;
constexpr int32_t kStockVolumeRow = 1;
// Android is always fullscreen, so the row is left out there and the ones
// after it move up.
#if REX_PLATFORM_ANDROID
constexpr uint32_t kFullscreenRows = 0;
#else
constexpr uint32_t kFullscreenRows = 1;
#endif
// The picture first, from the window down to the effect laid over it, then the
// sound. The first three lines are the stock rows', taken over.
constexpr uint32_t kFullscreenRow = 0;
constexpr uint32_t kPresetRow = kFullscreenRow + kFullscreenRows;
constexpr uint32_t kGammaRow = kPresetRow + 1;
constexpr uint32_t kScreenFilterRow = kGammaRow + 1;
constexpr uint32_t kScreenFilterIntensityRow = kScreenFilterRow + 1;
constexpr uint32_t kVolumeRow = kScreenFilterIntensityRow + 1;
constexpr uint32_t kSettingsRowCount = kVolumeRow + 1;

// The second page: language, and whose release to take voices, artwork and the
// prologue from. Its rows are all its own.
constexpr uint32_t kLanguageRow = 0;
constexpr uint32_t kVoicesRow = 1;
constexpr uint32_t kArtworkRow = 2;
constexpr uint32_t kPrologueRow = 3;
constexpr uint32_t kLanguagePageRowCount = 4;

constexpr OptionRow kSettingsRows[] = {
#if !REX_PLATFORM_ANDROID
    {
        .list_row = kFullscreenRow,
        .label = kFullscreenLabel,
        .value_count = 2,
        .values = kOffOnTexts,
        .get = &GetCvarBool,
        .commit = &CommitCvarBool,
        .cvar = "fullscreen",
        .live = true,
        .follow = true,
        .tooltip = NativeText{"tip_fullscreen"},
    },
#endif
    {
        .list_row = kPresetRow,
        .label = kPresetLabel,
        .value_count = kGraphicsPresetCount,
        .value_text = [](int32_t value) { return kGraphicsPresetTexts[value]; },
        // "Custom" (the last entry) only joins the cycle once the player has
        // dialled a stretch in by hand on the screen Y opens.
        .live_value_count = &GraphicsPresetChoiceCount,
        .get = [](const OptionRow&) { return GetGraphicsPresetIndex(); },
        .commit = [](const OptionRow&, int32_t value) { RequestGraphicsPreset(value); },
        .live = true,
        .tooltip = NativeText{"tip_preset"},
    },
    {
        .list_row = kGammaRow,
        .label = kGammaLabel,
        .value_count = std::size(kGammaTexts),
        .value_text = &GammaText,
        .get = &GetGammaIndex,
        .commit = &CommitGammaIndex,
        .cvar = "display_gamma",
        .live = true,
        .tooltip = NativeText{"tip_gamma"},
    },
    {
        // Stands in for the stock Graphics setting: Enhanced is one of the
        // filters (Smooth, settings.h). The game's own Graphics value is held
        // at the style the filter picked, so its Accept writes back the same
        // style.
        .list_row = kScreenFilterRow,
        .label = kScreenFilterLabel,
        .value_count = kMaxScreenFilterOptions,
        .value_text = &ScreenFilterText,
        .live_value_count = &ScreenFilterChoiceCount,
        .get = &GetScreenFilterIndex,
        .commit = &CommitScreenFilterIndex,
        .cvar = "screen_filter",
        .live = true,
        .stock_row = kStockGraphicsRow,
        .stock_value = [](const OptionRow&) { return int32_t(ScreenFilterIsEnhanced() ? 1 : 0); },
        .tooltip = NativeText{"tip_screen_filter"},
    },
    {
        .list_row = kScreenFilterIntensityRow,
        .label = kScreenFilterIntensityLabel,
        .value_count = std::size(kScreenFilterIntensityTexts),
        .value_text = &ScreenFilterIntensityText,
        .get = &GetScreenFilterIntensityIndex,
        .commit = &CommitScreenFilterIntensityIndex,
        .cvar = "screen_filter_intensity",
        .live = true,
        .tooltip = NativeText{"tip_screen_filter_intensity"},
    },
    {
        // The stock Volume setting's 0-10 spinner, backed by audio_volume; the
        // game's own volume is held at its maximum underneath.
        .list_row = kVolumeRow,
        .label = kVolumeLabel,
        .min = 0,
        .max = kVolumeSteps,
        .get = &GetMasterVolumeStep,
        .commit = &CommitMasterVolumeStep,
        .live = true,
        .stock_row = kStockVolumeRow,
        .pin_stock_to_max = true,
        .tooltip = NativeText{"tip_volume"},
    },
};

constexpr OptionRow kLanguagePageRows[] = {
    {
        // `live`: each left/right switches the whole game's language on the
        // next frame, and B puts the original back the same way.
        .list_row = kLanguageRow,
        .label = kLanguageLabel,
        .value_count = kMaxLanguageOptions,
        .value_text = &LanguageText,
        .live_value_count = &LanguageOptionCount,
        .get = &GetLanguageIndex,
        .commit = &CommitLanguageIndex,
        .live = true,
        .tooltip = NativeText{"tip_language"},
    },
    {
        .list_row = kVoicesRow,
        .label = kVoicesLabel,
        .value_count = 3,
        .values = kReleaseTexts,
        .get = &GetReleaseChoice,
        .commit = &CommitReleaseChoice,
        .reset = &ResetReleaseChoice,
        .cvar = "voices",
        .live = true,
        .locked = &ReleaseChoiceLocked,
        .tooltip = NativeText{"tip_voices"},
    },
    {
        .list_row = kArtworkRow,
        .label = kArtworkLabel,
        .value_count = 3,
        .values = kReleaseTexts,
        .get = &GetReleaseChoice,
        .commit = &CommitReleaseChoice,
        .reset = &ResetReleaseChoice,
        .cvar = "artwork",
        .live = true,
        .locked = &ReleaseChoiceLocked,
        .tooltip = NativeText{"tip_artwork"},
    },
    {
        .list_row = kPrologueRow,
        .label = kPrologueLabel,
        .value_count = 3,
        .values = kReleaseTexts,
        .get = &GetReleaseChoice,
        .commit = &CommitReleaseChoice,
        .reset = &ResetReleaseChoice,
        .cvar = "prologue",
        .live = true,
        .locked = &ReleaseChoiceLocked,
        .tooltip = NativeText{"tip_prologue"},
    },
};

static_assert(RowsAreWellFormed(kSettingsRows, kSettingsRowCount, /*owns_all_rows=*/false),
              "every settings row needs a unique in-range list_row; appended rows must run "
              "contiguously up from kVanillaRowCount; a row without `get` is label-only and "
              "must supply a `label` and target a stock row");
static_assert(RowsAreWellFormed(kLanguagePageRows, kLanguagePageRowCount, /*owns_all_rows=*/true));

// What fits on the screen's backdrop, as on the enhancements pages: the
// settings pages switch with the shoulder buttons rather than scroll.
constexpr uint32_t kSettingsVisibleRows = 7;
static_assert(kSettingsRowCount <= kSettingsVisibleRows &&
                  kLanguagePageRowCount <= kSettingsVisibleRows,
              "each settings page fits the screen without scrolling");

template <const auto& Rows>
PageDescription SettingsDescription(uint32_t list_row, int32_t) {
  for (const OptionRow& row : Rows) {
    if (row.list_row != list_row) {
      continue;
    }
    const bool locked = row.locked && row.locked(row);
    return {locked ? kChoiceNeedsBoth.Get() : u"", row.tooltip.Get() ? row.tooltip.Get() : u""};
  }
  return {};
}

// The glyph name "BTN_LB" in the guest image (as native_enhancements.cpp).
constexpr uint32_t kLeftBumperGlyph = 0x82012F50u;
constexpr NativeText kChangePagePrompt{"change_page_prompt"};

bool RelayoutSettingsPage();
PageDescription SettingsPageDescription(uint32_t list_row, int32_t value);

// Both pages carry a title of their own, so that coming back to the first one
// puts its title back: a null key would leave the second page's in place. The
// first has the stretch screen's Y prompt as well as the page one.
//
// The first lays its rows out from kSettingsRows (RelayoutSettingsPage), which
// it starts out showing all of.
OptionsPage g_settings_page = {
    .rows = kSettingsRows,
    .row_count = static_cast<uint32_t>(std::size(kSettingsRows)),
    .total_rows = kSettingsRowCount,
    .visible_rows = kSettingsVisibleRows,
    .title = NativeText{"settings_title_general"},
    .title_arrows = true,
    .stretch_prompt = true,
    .prompt_text = kCustomPromptLabel,
    .prompt_glyph = kYGlyphNameAddr,
    // Y sets the stretch by hand, so it only means something on Preset.
    .prompt_row = static_cast<int32_t>(kPresetRow),
    .page_prompt = true,
    .relayout = &RelayoutSettingsPage,
    .row_description = &SettingsPageDescription,
};

// The first page's rows as laid out, and what they were laid out for.
std::array<OptionRow, std::size(kSettingsRows)> g_settings_page_rows = {};
bool g_settings_laid_out = false;
bool g_settings_intensity_shown = false;

// OptionsPage::relayout for the first page: Filter Intensity only while the
// filter has an intensity to set (Scanlines, LCD Grid), the rows below it
// moving up a line while it is gone.
bool RelayoutSettingsPage() {
  const bool shown = CurrentScreenFilter().kind != ScreenFilterKind::kNone;
  if (g_settings_laid_out && shown == g_settings_intensity_shown) {
    return false;
  }
  g_settings_laid_out = true;
  g_settings_intensity_shown = shown;
  uint32_t count = 0;
  for (const OptionRow& row : kSettingsRows) {
    if (!shown && row.list_row == kScreenFilterIntensityRow) {
      continue;
    }
    OptionRow& out = g_settings_page_rows[count++];
    out = row;
    if (!shown && row.list_row > kScreenFilterIntensityRow) {
      --out.list_row;
    }
  }
  g_settings_page.rows = g_settings_page_rows.data();
  g_settings_page.row_count = g_settings_page.total_rows = count;
  return true;
}

PageDescription SettingsPageDescription(uint32_t list_row, int32_t) {
  for (uint32_t i = 0; i < g_settings_page.row_count; ++i) {
    const OptionRow& row = g_settings_page.rows[i];
    if (row.list_row == list_row) {
      return {u"", row.tooltip.Get() ? row.tooltip.Get() : u""};
    }
  }
  return {};
}

constexpr OptionsPage kLanguagePage = {
    .rows = kLanguagePageRows,
    .row_count = static_cast<uint32_t>(std::size(kLanguagePageRows)),
    .total_rows = kLanguagePageRowCount,
    .visible_rows = kSettingsVisibleRows,
    .title = NativeText{"settings_title_language"},
    .title_arrows = true,
    .owns_all_rows = true,
    .page_prompt = true,
    .row_description = &SettingsDescription<kLanguagePageRows>,
};

const OptionsPage* const kSettingsPages[] = {&g_settings_page, &kLanguagePage};

// ---------------------------------------------------------------------------
// The current page
// ---------------------------------------------------------------------------

// The page the screen shows, picked by Help & Options (SelectSettingsPage)
// before it opens the screen, and the page the list was last bound with. The
// two differ for exactly the bind that switches pages.
const OptionsPage* g_page = &g_settings_page;
const OptionsPage* g_bound_page = nullptr;

// How many of the page's rows are scrolled off the top of the list. Always 0
// on a page whose rows all fit.
uint32_t g_scroll = 0;

const OptionsPage& Page() { return *g_page; }

uint32_t VisibleRowCount(const OptionsPage& page) {
  return std::min({page.visible_rows, page.total_rows, kListMaxRows});
}

uint32_t VisibleRowCount() { return VisibleRowCount(Page()); }

// How many rows the list is built with: enough for whichever page shows the
// most. A page with fewer binds fewer, and the list hides the rest.
uint32_t ListCapacity() {
  // The first page's full table, whatever it is laid out as right now.
  return std::max({std::min(kSettingsRowCount, kSettingsVisibleRows), VisibleRowCount(kLanguagePage),
                   VisibleRowCount(EnhancementsOptionsPage()),
                   VisibleRowCount(RandomizerOptionsPage())});
}

// The page after or before `current` in its group (the shoulder buttons), or
// null when it has none.
const OptionsPage* NeighbourPage(const OptionsPage& current, int direction) {
  const auto count = static_cast<int>(std::size(kSettingsPages));
  for (int i = 0; i < count; ++i) {
    if (kSettingsPages[i] == &current) {
      return kSettingsPages[(i + (direction > 0 ? 1 : count - 1)) % count];
    }
  }
  if (const OptionsPage* page = NeighbourEnhancementsPage(current, direction)) {
    return page;
  }
  return NeighbourRandomizerPage(current, direction);
}

bool IsEnumRow(const OptionRow& row) { return row.value_count > 0; }

// A row that only replaces a stock row's text, leaving its value alone.
bool IsLabelOnly(const OptionRow& row) { return row.get == nullptr; }

// An override row whose `available` says no is inert: the stock row keeps its
// vanilla behaviour, model copy-back included.
bool IsRowActive(const OptionRow& row) { return row.available == nullptr || row.available(); }

// Whether a row is scrolled into view, and which list row shows it if so.
bool OnScreen(const OptionRow& row) {
  return row.list_row >= g_scroll && row.list_row < g_scroll + VisibleRowCount();
}

uint32_t ScreenRow(const OptionRow& row) { return row.list_row - g_scroll; }

// Index into the page's rows for the row on `list_row` of the page, or -1 if
// that row is stock (or an override that has opted out).
int32_t FindActiveRow(uint32_t list_row) {
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    if (Page().rows[i].list_row == list_row && IsRowActive(Page().rows[i])) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

// As above, but ignoring label-only rows -- for the paths that care about who
// *owns a value* (model copy-back, cycling) rather than who paints text.
int32_t FindActiveValueRow(uint32_t list_row) {
  const int32_t index = FindActiveRow(list_row);
  return (index >= 0 && IsLabelOnly(Page().rows[index])) ? -1 : index;
}

int32_t RowMin(const OptionRow& row) { return IsEnumRow(row) ? 0 : row.min; }

int32_t RowMax(const OptionRow& row) {
  if (!IsEnumRow(row)) {
    return row.max;
  }
  uint32_t count = row.value_count;
  if (row.live_value_count) {
    count = std::clamp(row.live_value_count(), uint32_t{1}, row.value_count);
  }
  return static_cast<int32_t>(count) - 1;
}

int32_t RowStep(const OptionRow& row) { return IsEnumRow(row) ? 1 : row.step; }

int32_t RowMultiplier(const OptionRow& row) { return IsEnumRow(row) ? 1 : row.multiplier; }

const char16_t* RowValueText(const OptionRow& row, int32_t value) {
  if (value < 0 || static_cast<uint32_t>(value) >= row.value_count) {
    return nullptr;
  }
  return row.values ? row.values[value].Get() : row.value_text(value);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

PPCFunc* g_original_list_setup_fn = NOCTURNE_ORIGINAL_FN(kListSetupFnAddr);
PPCFunc* g_original_bind_rows_fn = NOCTURNE_ORIGINAL_FN(kBindRowsFnAddr);
PPCFunc* g_original_cycle_row_fn = NOCTURNE_ORIGINAL_FN(kCycleRowFnAddr);
PPCFunc* g_original_move_selection_fn = NOCTURNE_ORIGINAL_FN(kMoveSelectionFnAddr);
PPCFunc* g_original_settings_event_fn = NOCTURNE_ORIGINAL_FN(kSettingsEventFnAddr);
PPCFunc* g_original_settings_activate_fn = NOCTURNE_ORIGINAL_FN(kSettingsActivateFnAddr);
PPCFunc* g_enable_row_fn = nullptr;
PPCFunc* g_original_list_update_fn = NOCTURNE_ORIGINAL_FN(kListUpdateFnAddr);
PPCFunc* g_set_widget_colour_fn = nullptr;
PPCFunc* g_set_widget_text_by_id_fn = nullptr;
PPCFunc* g_alloc_fn = nullptr;
PPCFunc* g_original_settings_update_fn = NOCTURNE_ORIGINAL_FN(kSettingsUpdateFnAddr);
PPCFunc* g_original_post_event_fn = NOCTURNE_ORIGINAL_FN(kPostEventFnAddr);
PPCFunc* g_original_prompt_bar_layout_fn = NOCTURNE_ORIGINAL_FN(kPromptBarLayoutFnAddr);
PPCFunc* g_original_settings_build_fn = NOCTURNE_ORIGINAL_FN(kSettingsBuildFnAddr);
PPCFunc* g_prompt_ctor_fn = nullptr;
PPCFunc* g_prompt_set_glyph_fn = nullptr;
PPCFunc* g_prompt_show_glyph_fn = nullptr;
PPCFunc* g_prompt_text_offset_fn = nullptr;
PPCFunc* g_prompt_set_pos_fn = nullptr;
PPCFunc* g_text_width_fn = nullptr;
PPCFunc* g_find_image_fn = nullptr;
PPCFunc* g_image_widget_ctor_fn = nullptr;

// The settings screen's three stock prompts, left to right (X, A, B), captured
// during the build; and the fourth ("Custom", for Y) this file adds, with the
// screen it belongs to. See NativeOptions_PromptBarLayout.
std::array<uint32_t, 3> g_stock_prompts = {};
uint32_t g_custom_prompt = 0;
// The page switch prompt (OptionsPage::page_prompt), after it. Then the text
// scale the bar's prompts are drawn at when they all fit, which is the stock
// prompts' own for the language in use, and the scale LayoutPromptBar last
// gave them: a stock prompt found at anything else was re-texted by the game,
// which put its own scale back.
uint32_t g_page_prompt = 0;
// Whether LayoutPromptBar last showed the page's own prompt.
bool g_custom_prompt_shown = false;
float g_prompt_scale = 0.0f;
float g_prompt_applied_scale = 0.0f;
// A text-only prompt widget next to the title, for the page's credit.
uint32_t g_credit = 0;
// The arrows either side of a paged title (OptionsPage::title_arrows), left
// then right, on the same screen as g_credit.
std::array<uint32_t, 2> g_title_arrows = {};
// Text-only prompt widgets, one per line, for the page's
// OptionsPage::description, on the same screen as g_credit; and the text they
// show while shown.
constexpr uint32_t kDescriptionMaxLines = 40;
std::array<uint32_t, kDescriptionMaxLines> g_description_lines = {};
bool g_description_shown = false;
std::u16string g_description_warning;
std::u16string g_description_text;
uint32_t g_custom_prompt_screen = 0;

// The settings screen and the user who last opened it, captured by
// NativeOptions_SettingsActivate so a language switch can redo its title.
uint32_t g_settings_screen = 0;
uint32_t g_settings_user = 0;
// Guest buffer for the formatted title and the gamertag, allocated on first use.
uint32_t g_title_scratch = 0;
// Guest buffer for RefreshStockRowValues, allocated on first use.
uint32_t g_value_scratch = 0;

// The page we opened with no save loaded, from the moment we open it until we
// leave it again, or 0 when we are not inside one. Scopes both the watchdog
// suppression and the latch cleanup in NativeOptions_PostEvent to exactly that
// window.
//
// Not stretch-screen-specific: any screen whose Activate clears the UI
// manager's `mgr+332` settled flag trips the same pre-game watchdog, and
// CScreenAchievement (page 15, see achievements_screen.cpp) does exactly what
// the stretch screen's does. EnterPregameScreen is how that file arms this.
uint32_t g_pregame_screen_page = 0;

// Set while the settings screen's own handler runs on a randomizer page, for
// NativeOptions_PostEvent to send its way out (back to Help & Options, with
// no save loaded) to the main menu instead. Guest thread only.
bool g_leaving_for_main_menu = false;

// Needed to resolve the app object's virtual "signed in" through its vtable at
// runtime (see RefreshSettingsTitle): there is no static address to hardcode.
rex::runtime::FunctionDispatcher* g_dispatcher = nullptr;

// Needed to close this process's window after a successful
// rex::platform::process::Relaunch() -- see the auto-restart-on-accept logic
// in NativeOptions_SettingsEvent. Runtime::display_window() is read lazily at
// the point of use rather than cached, since Bind() runs before a display
// window necessarily exists.
rex::Runtime* g_runtime = nullptr;

// The settings screen's option list, claimed in the list-setup hook. Every
// other hook keys off this so the controls screen's identically-shaped list
// is never touched. Only ever read/written from the guest thread that runs
// the front-end, so no synchronisation is needed.
uint32_t g_list = 0;

// The entry table the list is bound with: one entry per list row, so the
// visible window of the page. Allocated once, on the first bind, from the
// game's own heap.
uint32_t g_entries = 0;
constexpr uint32_t kArenaSize = kListMaxRows * kEntryStride;

// The game's own 3-entry table (model+12). The stock rows of our copy are
// mirrored back into it whenever they change, because the screen's "apply"
// path (sub_8257F9E0 on A) reads the model, not the list -- without the
// copy-back, cycling Graphics would render correctly and then apply nothing.
uint32_t g_vanilla_entries = 0;

// Each page row's current value, kept here rather than in the entry table
// because a scrolling page only has entries for the rows in view; and its
// value as of the last bind, so B can undo a live row's running changes.
int32_t g_values[kMaxPageRows] = {};
int32_t g_seeded_values[kMaxPageRows] = {};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint8_t Read8(const uint8_t* base, uint32_t guest_address) { return base[guest_address]; }

void Write8(uint8_t* base, uint32_t guest_address, uint8_t value) { base[guest_address] = value; }

// Queues a front-end page switch, exactly as the screens themselves do.
//
// Goes to the *original* post rather than our own override: our filter reads
// the caller's return address to recognise the watchdog's post, and the lr in
// this context belongs to whatever guest call we are wrapping, not to a real
// call site.
void CallGuestPageSwitch(PPCContext& ctx, uint8_t* base, uint32_t page, uint32_t controller) {
  CallGuest(ctx, base, g_original_post_event_fn, kEventClassPageSwitch, 0, page, controller);
}

// Allocates the entry table from the game's own heap, once. Both the row
// binding and the prompt-bar builder need it to exist (the builder runs when
// the screen object is created, the bind when it opens), so neither owns the
// allocation.
bool EnsureArena(PPCContext& ctx, uint8_t* base) {
  if (g_entries) {
    return true;
  }
  g_entries = CallGuest(ctx, base, g_alloc_fn, kArenaSize);
  if (!g_entries) {
    REXLOG_WARN("[native_options] guest allocation failed; no extra rows");
    return false;
  }
  return true;
}

uint32_t RowLabelWidget(const uint8_t* base, uint32_t screen_row) {
  return Read32BE(base, g_list + kListRowLabels + 4 * screen_row);
}

uint32_t RowValueWidget(const uint8_t* base, uint32_t screen_row) {
  return Read32BE(base, g_list + kListRowValues + 4 * screen_row);
}

// The list's normal row text scale, read live rather than hardcoded.
float ListTextScale(const uint8_t* base) {
  if (!g_list) {
    return kFallbackTextScale;
  }
  const float scale = std::bit_cast<float>(Read32BE(base, g_list + kListTextScale));
  return (scale > 0.0f && scale <= 4.0f) ? scale : kFallbackTextScale;
}

// Replaces a row widget's text outright, then reasserts the row text scale.
//
// The scale matters because the two text setters are not equivalent. The
// stock rows go through sub_825D1EE8 -> sub_825CFC68, which looks the string
// up in the shared table and copies *that entry's own scale* into the
// widget's +536/+540 alongside the text. Literal text goes through
// sub_825D1EE0 -> sub_825CEE40, which writes only the text and leaves the
// scale at the widget's construction default -- noticeably larger than every
// stock row. So anything setting literal text here has to supply the scale
// itself; sub_825D20C0 is the same setter sub_825D36B8 uses to re-scale the
// highlighted row.
void SetRowText(PPCContext& ctx, uint8_t* base, uint32_t widget, const char16_t* text,
                bool preserve_scale = false) {
  if (!widget || !text) {
    return;
  }

  // Relabelling a stock row means the widget already carries the scale
  // sub_825CFC68 copied out of the string table for its original text --
  // which is the scale it should keep. Our own rows have no such history:
  // theirs is the widget's construction default (visibly oversized), or
  // whatever the row it last showed left behind, so they take the list's
  // normal scale instead.
  float scale = ListTextScale(base);
  if (preserve_scale) {
    const uint32_t inner = Read32BE(base, widget + kWidgetInnerTextOffset);
    if (inner) {
      const float existing = std::bit_cast<float>(Read32BE(base, inner + kWidgetTextScaleOffset));
      if (existing > 0.0f && existing <= 4.0f) {
        scale = existing;
      }
    }
  }

  SetWidgetText(ctx, base, widget, text);
  SetWidgetScale(ctx, base, widget, scale);
}

// Seeds a row's value from its `get`, as of the screen opening.
void SeedRow(uint32_t index) {
  const OptionRow& row = Page().rows[index];
  const int32_t value = row.get ? row.get(row) : RowMin(row);
  g_values[index] = std::clamp(value, RowMin(row), RowMax(row));
  // Remembered so B (cancel) can put a live row's real value back: see
  // NativeOptions_SettingsEvent.
  g_seeded_values[index] = g_values[index];
}

// Lays a row in view out as a stock spinner entry. Enumerated rows carry no
// inline value string ids, so the vanilla renderer prints the raw number,
// which RenderRow then paints over with the row's own text.
void WriteEntry(uint8_t* base, uint32_t index) {
  const OptionRow& row = Page().rows[index];
  if (!OnScreen(row)) {
    return;
  }
  const uint32_t entry = g_entries + ScreenRow(row) * kEntryStride;

  // A row with no `label` of its own keeps the game's, which means keeping
  // the entry's label string id: sub_825D36B8 feeds that field straight to
  // sub_825D1EE8, so zeroing it along with the rest of the entry blanks the
  // row's label. On an override row the id is the one just copied out of the
  // vanilla table; on the X-reset path (no preceding copy) it is the one this
  // function itself preserved last time.
  const uint32_t inherited_label_id = Read32BE(base, entry + kEntryLabelId);

  for (uint32_t offset = 0; offset < kEntryStride; offset += 4) {
    Write32BE(base, entry + offset, 0);
  }
  if (!row.label.key) {
    Write32BE(base, entry + kEntryLabelId, inherited_label_id);
  }

  Write32BE(base, entry + kEntryValue, static_cast<uint32_t>(g_values[index]));
  Write32BE(base, entry + kEntryMin, static_cast<uint32_t>(RowMin(row)));
  Write32BE(base, entry + kEntryMax, static_cast<uint32_t>(RowMax(row)));
  Write32BE(base, entry + kEntryStep, static_cast<uint32_t>(RowStep(row)));
  Write32BE(base, entry + kEntryMultiplier, static_cast<uint32_t>(RowMultiplier(row)));
}

// Writes the entries of every row of ours in view.
void WriteEntries(uint8_t* base) {
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    if (IsRowActive(Page().rows[i]) && !IsLabelOnly(Page().rows[i])) {
      WriteEntry(base, i);
    }
  }
}

// The value the list itself holds for a row in view, i.e. after a left/right.
int32_t EntryValue(const uint8_t* base, const OptionRow& row) {
  return static_cast<int32_t>(
      Read32BE(base, g_entries + ScreenRow(row) * kEntryStride + kEntryValue));
}

// Paints a row's label, and its value too when the row is enumerated. Numeric
// rows keep the number the game itself just rendered, and a row with no
// `label` keeps the game's own.
void RenderRow(PPCContext& ctx, uint8_t* base, uint32_t index) {
  const OptionRow& row = Page().rows[index];
  if (!g_list || !OnScreen(row)) {
    return;
  }
  const uint32_t screen_row = ScreenRow(row);

  if (row.label.key) {
    SetRowText(ctx, base, RowLabelWidget(base, screen_row), row.label.Get(),
               /*preserve_scale=*/IsLabelOnly(row));
  }
  if (IsEnumRow(row)) {
    SetRowText(ctx, base, RowValueWidget(base, screen_row), RowValueText(row, g_values[index]));
  }
}

void RenderRows(PPCContext& ctx, uint8_t* base) {
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    if (IsRowActive(Page().rows[i])) {
      RenderRow(ctx, base, i);
    }
  }
}

// Mirrors one stock row back into the game's own table. See g_vanilla_entries.
// Rows an active override has taken over are deliberately *not* mirrored:
// leaving the model's original entry alone is what keeps whatever that stock
// row used to drive from following our value around. A page that owns all its
// rows never shows a stock row, so it never mirrors anything.
void CopyEntryToVanilla(uint8_t* base, uint32_t row) {
  if (!g_vanilla_entries || !g_entries || row >= kVanillaRowCount || Page().owns_all_rows) {
    return;
  }
  if (FindActiveValueRow(row) >= 0) {
    return;
  }
  const uint32_t offset = row * kEntryStride;
  for (uint32_t i = 0; i < kEntryStride; i += 4) {
    Write32BE(base, g_vanilla_entries + offset + i, Read32BE(base, g_entries + offset + i));
  }
}

// The stock setting a row stands in for. See OptionRow::stock_row.
uint32_t StockRow(const OptionRow& row) {
  return row.stock_row >= 0 ? static_cast<uint32_t>(row.stock_row) : row.list_row;
}

// Writes a row's stock_value into the game's own table. See
// OptionRow::stock_value.
void SyncStockValue(uint8_t* base, const OptionRow& row) {
  const uint32_t stock = StockRow(row);
  if (!g_vanilla_entries || Page().owns_all_rows || !row.stock_value || stock >= kVanillaRowCount) {
    return;
  }
  Write32BE(base, g_vanilla_entries + stock * kEntryStride + kEntryValue,
            static_cast<uint32_t>(row.stock_value(row)));
}

// Rebinds the list to the rows in view after a scroll or a relayout: the same
// bind the screen does, minus the re-seeding our bind hook adds, since the
// values have to survive scrolling out of view and back. Rows a shorter
// layout hid are shown again, and the rows past a layout's end hidden.
void BindVisibleRows(PPCContext& ctx, uint8_t* base) {
  WriteEntries(base);
  CallGuest(ctx, base, g_original_bind_rows_fn, g_list, g_entries, VisibleRowCount());
  for (uint32_t row = 0; row < ListCapacity(); ++row) {
    const bool shown = row < VisibleRowCount();
    SetWidgetVisible(ctx, base, RowLabelWidget(base, row), shown);
    SetWidgetVisible(ctx, base, RowValueWidget(base, row), shown);
  }
  // The rows the screen's Activate disables and only re-enables when the page
  // it opened on uses them (NativeOptions_SettingsActivate): a layout that
  // grew into them needs them cyclable again.
  if (g_enable_row_fn) {
    for (uint32_t row = kGatedFirstRow; row <= kGatedLastRow && row < VisibleRowCount(); ++row) {
      CallGuest(ctx, base, g_enable_row_fn, g_list, row);
    }
  }
  RenderRows(ctx, base);
}

void ApplyPageChrome(PPCContext& ctx, uint8_t* base);

// Lays out a page whose rows depend on its values (OptionsPage::relayout)
// again, and when its rows changed, reseeds them from their `get` and
// rebinds. The selection stays on its list row while that row still exists.
// Returns whether the rows changed.
bool RelayoutPage(PPCContext& ctx, uint8_t* base) {
  if (!Page().relayout || !Page().relayout()) {
    return false;
  }
  const uint32_t visible = VisibleRowCount();
  g_scroll = std::min(g_scroll, Page().total_rows - visible);
  const uint32_t selected = Read32BE(base, g_list + kListSelectedRow);
  if (visible > 0 && selected >= visible) {
    Write32BE(base, g_list + kListSelectedRow, visible - 1);
  }
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    if (IsRowActive(Page().rows[i]) && !IsLabelOnly(Page().rows[i])) {
      SeedRow(i);
    }
  }
  BindVisibleRows(ctx, base);
  ApplyPageChrome(ctx, base);
  return true;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// Claims the settings screen's option list and grows it to ListCapacity().
// Doing it here rather than after the fact means the list creates each extra
// row's label widget, value widget, cyclable flag and layout slot itself,
// exactly like the three it already makes. A page with fewer rows binds fewer
// and the list hides the rest (see NativeOptions_BindRows).
extern "C" void NativeOptions_ListSetup(PPCContext& ctx, uint8_t* base) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const bool from_settings_screen =
      lr > kSettingsBuildFnAddr && lr <= kSettingsBuildFnAddr + kSettingsBuildFnSize;
  if (from_settings_screen && ctx.r6.u32 == kVanillaRowCount) {
    g_list = ctx.r3.u32;
    ctx.r6.u32 = ListCapacity();
  }
  if (g_original_list_setup_fn) {
    g_original_list_setup_fn(ctx, base);
  }
}

// Substitutes our table for the game's 3-entry one, then paints our rows.
// Runs again on every re-entry to the screen (its activate and its B/X paths
// all re-bind), which is also what re-seeds each row from its `get`: so
// cancelling out of the screen discards an uncommitted change for free, the
// same way it does for the stock rows.
//
// The first bind after SelectSettingsPage picked another page is where the
// screen switches pages: back to the top of the new page, with its own row
// count. The list only ever hides rows past the count it is bound with, so
// the rows a shorter page hid are shown again here.
extern "C" void NativeOptions_BindRows(PPCContext& ctx, uint8_t* base) {
  bool ours = g_list != 0 && ctx.r3.u32 == g_list && ctx.r5.u32 == kVanillaRowCount;

  if (ours) {
    EnsureArena(ctx, base);
    if (g_entries) {
      // A page laid out from its values, against the values it opens with.
      if (Page().relayout) {
        Page().relayout();
      }
      if (g_bound_page != g_page) {
        g_bound_page = g_page;
        g_scroll = 0;
        Write32BE(base, g_list + kListSelectedRow, 0);
        // Whichever row was selected on the old page still has the list's
        // larger selected scale on its value.
        for (uint32_t row = 0; row < ListCapacity(); ++row) {
          SetWidgetScale(ctx, base, RowValueWidget(base, row), ListTextScale(base));
        }
      }

      g_vanilla_entries = ctx.r4.u32;
      if (!Page().owns_all_rows) {
        for (uint32_t i = 0; i < kVanillaRowCount * kEntryStride; i += 4) {
          Write32BE(base, g_entries + i, Read32BE(base, g_vanilla_entries + i));
        }
      }
      for (uint32_t i = 0; i < Page().row_count; ++i) {
        if (IsRowActive(Page().rows[i]) && !IsLabelOnly(Page().rows[i])) {
          SeedRow(i);
        }
      }
      WriteEntries(base);

      // Pin displaced stock settings to their maximum, in the game's *own*
      // table -- that is the copy its apply path (sub_8257F9E0) and its live
      // on-change apply both read, and the one we otherwise leave frozen at
      // whatever the row held before it was taken over.
      for (uint32_t i = 0; i < Page().row_count; ++i) {
        const OptionRow& row = Page().rows[i];
        if (!IsRowActive(row) || !row.pin_stock_to_max || StockRow(row) >= kVanillaRowCount) {
          continue;
        }
        const uint32_t stock = g_vanilla_entries + StockRow(row) * kEntryStride;
        Write32BE(base, stock + kEntryValue, Read32BE(base, stock + kEntryMax));
      }
      for (uint32_t i = 0; i < Page().row_count; ++i) {
        if (IsRowActive(Page().rows[i])) {
          SyncStockValue(base, Page().rows[i]);
        }
      }

      ctx.r4.u32 = g_entries;
      ctx.r5.u32 = VisibleRowCount();
    } else {
      ours = false;
    }
  }

  if (g_original_bind_rows_fn) {
    g_original_bind_rows_fn(ctx, base);
  }

  if (ours) {
    for (uint32_t row = 0; row < VisibleRowCount(); ++row) {
      SetWidgetVisible(ctx, base, RowLabelWidget(base, row), true);
      SetWidgetVisible(ctx, base, RowValueWidget(base, row), true);
    }
    RenderRows(ctx, base);
  }
}

// Rows marked `follow` track their setting when something else changes it.
void FollowRows(PPCContext& ctx, uint8_t* base) {
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    const OptionRow& row = Page().rows[i];
    if (!row.follow || !row.get || !IsRowActive(row) || !OnScreen(row)) {
      continue;
    }
    const int32_t value = std::clamp(row.get(row), RowMin(row), RowMax(row));
    if (value == g_values[i]) {
      continue;
    }
    g_values[i] = g_seeded_values[i] = value;
    Write32BE(base, g_entries + ScreenRow(row) * kEntryStride + kEntryValue,
              static_cast<uint32_t>(value));
    RenderRow(ctx, base, i);
  }
}

// Left/right on a row. The vanilla path already advances and wraps every row
// correctly (ours are stock spinner entries), so all this adds is taking the
// new value, repainting an enumerated row with its own text instead of the raw
// number, and keeping the stock rows mirrored back into the game's own table.
extern "C" void NativeOptions_CycleRow(PPCContext& ctx, uint8_t* base) {
  const bool ours = g_list != 0 && ctx.r3.u32 == g_list && g_entries != 0;
  const uint32_t screen_row = ours ? Read32BE(base, g_list + kListSelectedRow) : 0;

  if (g_original_cycle_row_fn) {
    g_original_cycle_row_fn(ctx, base);
  }

  if (!ours || screen_row >= VisibleRowCount()) {
    return;
  }

  const int32_t index = FindActiveValueRow(screen_row + g_scroll);
  if (index < 0) {
    CopyEntryToVanilla(base, screen_row);
    return;
  }

  const OptionRow& row = Page().rows[index];
  if (row.locked && row.locked(row)) {
    // The vanilla path already stepped the entry: put the held value back.
    Write32BE(base, g_entries + screen_row * kEntryStride + kEntryValue,
              static_cast<uint32_t>(g_values[index]));
    RenderRow(ctx, base, static_cast<uint32_t>(index));
    return;
  }
  g_values[index] = EntryValue(base, row);
  // Commit before painting, so the paint sees this keypress's change rather
  // than the previous one.
  if (row.live && row.commit) {
    row.commit(row, g_values[index]);
    SyncStockValue(base, row);
  }
  RenderRow(ctx, base, static_cast<uint32_t>(index));
  // Rows that only matter for some values of this one come and go.
  RelayoutPage(ctx, base);
}

// Up/down past the first or last row in view of a page that doesn't fit.
//
// The list itself only knows the rows it was bound with, so at its edges it
// would stop or wrap. Where the page has more rows past that edge, this
// scrolls them in instead: the selection stays on the same list row and the
// list is rebound one page row further along. The caller (sub_825D48D0)
// still plays the move sound. Wrapping, where the list does it, takes the
// page along: from the bottom back to the top of the page and the other way.
extern "C" void NativeOptions_MoveSelection(PPCContext& ctx, uint8_t* base) {
  const auto direction = static_cast<int32_t>(ctx.r4.u32);
  const uint32_t visible = VisibleRowCount();
  const uint32_t total = Page().total_rows;
  const bool ours = g_list != 0 && ctx.r3.u32 == g_list && g_entries != 0 && total > visible;
  const uint32_t selected = ours ? Read32BE(base, g_list + kListSelectedRow) : 0;

  if (ours && direction > 0 && selected + 1 >= visible && g_scroll + visible < total) {
    ++g_scroll;
    BindVisibleRows(ctx, base);
    return;
  }
  if (ours && direction < 0 && selected == 0 && g_scroll > 0) {
    --g_scroll;
    BindVisibleRows(ctx, base);
    return;
  }

  if (g_original_move_selection_fn) {
    g_original_move_selection_fn(ctx, base);
  }

  if (!ours) {
    return;
  }
  const uint32_t moved_to = Read32BE(base, g_list + kListSelectedRow);
  if (direction > 0 && moved_to < selected && g_scroll != 0) {
    g_scroll = 0;
    BindVisibleRows(ctx, base);
  } else if (direction < 0 && moved_to > selected && g_scroll != total - visible) {
    g_scroll = total - visible;
    BindVisibleRows(ctx, base);
  }
}

// Extends the list Update's highlight reset to the appended rows.
//
// That Update repaints the selected row's label with a pulsing colour every
// frame, and clears the previous frame's pulse by resetting the labels back to
// the normal colour first. The reset loop is hardcoded to three labels
// (list+648..+656), so an appended row gets pulsed while selected and then
// never restored -- it keeps whatever colour the pulse last left it on.
//
// Resetting the extra labels *before* the original runs is what the loop
// itself would have done had it been written for a variable row count: the
// original then re-pulses whichever row is selected, so there is no need to
// special-case the selected one here. Unconditional normal colour matches the
// stock loop exactly, disabled rows included -- those are hidden outright, so
// their colour never shows.
extern "C" void NativeOptions_ListUpdate(PPCContext& ctx, uint8_t* base) {
  const uint32_t list = ctx.r3.u32;

  if (g_list != 0 && list == g_list && g_set_widget_colour_fn) {
    const uint32_t colour = Read32BE(base, list + kListNormalColour);
    for (uint32_t row = kStockLabelResetCount; row < VisibleRowCount(); ++row) {
      const uint32_t label = RowLabelWidget(base, row);
      if (label) {
        CallGuest(ctx, base, g_set_widget_colour_fn, label, colour);
      }
    }
  }

  if (g_original_list_update_fn) {
    g_original_list_update_fn(ctx, base);
  }

  if (g_list != 0 && list == g_list && g_entries) {
    FollowRows(ctx, base);
  }

  // Repaint list row 2's value, which that same Update just blanked.
  //
  // It opens with `set_text(list+768, "")` -- value widget 2, hardcoded, no
  // condition -- because the row it was written for ("Change Screen Size...")
  // navigated instead of holding a value. Painting once at bind time is
  // therefore not enough for row 2 the way it is for every other row: the text
  // survives exactly until the next frame. The preset still applied, which is
  // why the row looked empty rather than broken.
  //
  // The scale is preserved rather than reasserted: the blanking doesn't touch
  // it, so whatever the widget carries is already right, including the larger
  // scale the list gives the selected row.
  if (g_list == 0 || list != g_list || !g_entries) {
    return;
  }
  const int32_t index = FindActiveValueRow(kSubmenuRow + g_scroll);
  if (index < 0 || !IsEnumRow(Page().rows[index])) {
    return;
  }
  SetRowText(ctx, base, RowValueWidget(base, kSubmenuRow),
             RowValueText(Page().rows[index], g_values[index]), /*preserve_scale=*/true);
}

// Keeps every stock row usable regardless of whether a save is loaded.
//
// The stock Activate greys out rows 1 and 2 (Volume, "Change Screen Size...")
// whenever the save-loaded flag at 0x82E4F808+28 is clear, leaving the
// pre-game/main-menu version of this screen showing only "Graphics". It does
// that with the list's own per-row enable/disable virtuals: slot 14
// (sub_825D4690) to disable, slot 15 (sub_825D3530) to re-enable. Calling the
// enable side back over rows 1-2 once Activate has finished undoes the gating
// and restores the enabled colours, the shown widget and the row's cyclable
// flag (list+584+row) in one go -- the same call the in-game path makes, so
// there is no separate "half enabled" state to get wrong.
//
// Ordering: Activate disables near its top and re-binds the rows further
// down, and nothing after that point re-disables, so running this once the
// original has returned is enough.
void ApplyPageChrome(PPCContext& ctx, uint8_t* base);

extern "C" void NativeOptions_SettingsActivate(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  g_settings_screen = screen;
  g_settings_user = ctx.r5.u32;

  if (g_original_settings_activate_fn) {
    g_original_settings_activate_fn(ctx, base);
  }

  if (!screen || !g_enable_row_fn) {
    return;
  }
  const uint32_t list = Read32BE(base, screen + kScreenListOffset);
  if (!list) {
    return;
  }
  for (uint32_t row = kGatedFirstRow; row <= kGatedLastRow; ++row) {
    if (list != g_list || row < VisibleRowCount()) {
      CallGuest(ctx, base, g_enable_row_fn, list, row);
    } else {
      // A page this short (a randomizer page with only the Game row) didn't
      // bind the row: enabling it would show it, blank and out of reach.
      SetWidgetVisible(ctx, base, RowLabelWidget(base, row), false);
      SetWidgetVisible(ctx, base, RowValueWidget(base, row), false);
    }
  }
  ApplyPageChrome(ctx, base);
}

// Captures the settings screen's three prompt widgets.
//
// The bar layout (sub_825CA820) is the only place all three are visible at
// once: the builder keeps just the A prompt on the screen object (+584) and
// drops the X and B ones as locals the moment it has spread them. That
// function also takes exactly three widgets and spreads them across the bar's
// width -- gaps of (width - total)/4 -- so a fourth prompt means re-laying the
// row out by hand afterwards, with the three captured here.
extern "C" void NativeOptions_PromptBarLayout(PPCContext& ctx, uint8_t* base) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (lr > kSettingsBuildFnAddr && lr <= kSettingsBuildFnAddr + kSettingsBuildFnSize) {
    // (x, y, width, X, A, B) -- the three widgets are the *last* three
    // arguments, r6/r7/r8; r3-r5 are the bar's own geometry.
    g_stock_prompts[0] = ctx.r6.u32;  // X, leftmost
    g_stock_prompts[1] = ctx.r7.u32;  // A
    g_stock_prompts[2] = ctx.r8.u32;  // B
  }

  if (g_original_prompt_bar_layout_fn) {
    g_original_prompt_bar_layout_fn(ctx, base);
  }
}

// One prompt's width on the bar: its glyph plus its text, the same sum the
// stock layout uses.
uint32_t PromptWidth(PPCContext& ctx, uint8_t* base, uint32_t prompt) {
  const uint32_t glyph = Read32BE(base, prompt + kPromptGlyphOffset);
  const uint32_t text = Read32BE(base, prompt + kPromptTextOffset);
  const uint32_t glyph_width = glyph ? Read32BE(base, glyph + kPromptGlyphWidthOffset) : 0;
  const uint32_t text_width =
      text ? CallGuest(ctx, base, g_text_width_fn, text, 0) : 0;
  return glyph_width + text_width;
}

// Makes an image widget on `screen` showing the named image, the way the
// builder makes the row arrows.
uint32_t CreateImageWidget(PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t image_name) {
  if (!g_alloc_fn || !g_find_image_fn || !g_image_widget_ctor_fn) {
    return 0;
  }
  const uint32_t image =
      CallGuest(ctx, base, g_find_image_fn, Read32BE(base, kImageBankPtrAddr), image_name);
  const uint32_t memory = image ? CallGuest(ctx, base, g_alloc_fn, kImageWidgetSize, 0) : 0;
  if (!memory) {
    return 0;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = memory;
  ctx.r4.u32 = screen;
  ctx.r5.u32 = 0;
  ctx.r6.u32 = 0;
  ctx.r7.u32 = image;
  g_image_widget_ctor_fn(ctx, base);
  const uint32_t widget = ctx.r3.u32;
  ctx = saved;
  return widget;
}

// Builds the "Custom" prompt, mirroring the builder's own recipe for the three
// stock ones: allocate, construct, point it at a glyph from the image bank,
// set the text, offset the text from the glyph, colour it. The only departure
// is the text itself -- the stock prompts take a string-table id, and there is
// no id for this -- so it goes in as literal UTF-16 and the text scale is
// copied off a stock prompt, which the by-id setter would have supplied.
uint32_t CreateCustomPrompt(PPCContext& ctx, uint8_t* base, uint32_t screen,
                            const char16_t* text, bool with_glyph) {
  if (!g_alloc_fn || !g_prompt_ctor_fn || !g_find_image_fn) {
    return 0;
  }
  const uint32_t memory = CallGuest(ctx, base, g_alloc_fn, kPromptSize, 0);
  if (!memory) {
    return 0;
  }

  PPCContext saved = ctx;
  ctx.r3.u32 = memory;
  ctx.r4.u32 = screen;
  ctx.r5.u32 = 0;
  g_prompt_ctor_fn(ctx, base);
  const uint32_t prompt = ctx.r3.u32;
  ctx = saved;
  if (!prompt) {
    return 0;
  }

  const uint32_t bank = Read32BE(base, kImageBankPtrAddr);
  const uint32_t image =
      with_glyph ? CallGuest(ctx, base, g_find_image_fn, bank, kYGlyphNameAddr) : 0;
  if (image && g_prompt_set_glyph_fn) {
    CallGuest(ctx, base, g_prompt_set_glyph_fn, prompt, image);
  }
  if (g_prompt_show_glyph_fn) {
    CallGuest(ctx, base, g_prompt_show_glyph_fn, prompt, 0);
  }

  SetWidgetText(ctx, base, prompt, text);

  if (g_prompt_text_offset_fn) {
    saved = ctx;
    ctx.r3.u32 = prompt;
    ctx.r4.u32 = with_glyph ? kPromptTextDx : 0;
    ctx.r5.u32 = with_glyph ? kPromptTextDy : 0;
    g_prompt_text_offset_fn(ctx, base);
    ctx = saved;
  }

  // Match a stock prompt's text scale, which its string-table lookup set.
  const uint32_t stock_text = Read32BE(base, g_stock_prompts[0] + kPromptTextOffset);
  if (stock_text) {
    const float scale = std::bit_cast<float>(Read32BE(base, stock_text + kWidgetTextScaleOffset));
    if (scale > 0.0f && scale <= 4.0f) {
      SetWidgetScale(ctx, base, prompt, scale);
    }
  }

  if (g_set_widget_colour_fn) {
    CallGuest(ctx, base, g_set_widget_colour_fn, prompt, Read32BE(base, kPromptColourAddr));
  }
  return prompt;
}

// Spreads the three stock prompts, then the page's own prompt and its page
// switch prompt where it has them, evenly across the (widened) bar, hiding the ones it
// doesn't. The spacing is the stock layout's own formula for any count: n+1
// equal gaps around n prompts. When they don't all fit with a minimum gap
// between them, every prompt's text is shrunk until they do. Runs again on
// every page switch and after a language switch, which changes every prompt's
// width (and puts the stock prompts' scale back).
constexpr uint32_t kPromptMinGap = 24;
constexpr float kPromptShrinkStep = 0.92f;
constexpr int kPromptMaxShrinks = 8;

// Whether the page's own prompt applies right now: it has one, and the row it
// belongs to (OptionsPage::prompt_row), if any, is selected.
bool PagePromptActive(const uint8_t* base) {
  if (Page().prompt_text.key == nullptr) {
    return false;
  }
  if (Page().prompt_row < 0) {
    return true;
  }
  return g_list != 0 && Read32BE(base, g_list + kListSelectedRow) + g_scroll ==
                            static_cast<uint32_t>(Page().prompt_row);
}

void LayoutPromptBar(PPCContext& ctx, uint8_t* base) {
  if (!g_custom_prompt || !g_prompt_set_pos_fn || g_stock_prompts[0] == 0 ||
      g_stock_prompts[1] == 0 || g_stock_prompts[2] == 0) {
    return;
  }

  const bool with_custom = PagePromptActive(base);
  g_custom_prompt_shown = with_custom;
  SetWidgetVisible(ctx, base, g_custom_prompt, with_custom);
  if (with_custom) {
    // The page's own glyph and text on the shared prompt.
    const uint32_t bank = Read32BE(base, kImageBankPtrAddr);
    const uint32_t image = Page().prompt_glyph && g_find_image_fn
                               ? CallGuest(ctx, base, g_find_image_fn, bank, Page().prompt_glyph)
                               : 0;
    if (image && g_prompt_set_glyph_fn) {
      CallGuest(ctx, base, g_prompt_set_glyph_fn, g_custom_prompt, image);
    }
    SetWidgetText(ctx, base, g_custom_prompt, Page().prompt_text.Get());
  }
  const bool with_page = Page().page_prompt && g_page_prompt != 0;
  if (g_page_prompt) {
    SetWidgetVisible(ctx, base, g_page_prompt, with_page);
    if (with_page) {
      SetWidgetText(ctx, base, g_page_prompt, kChangePagePrompt.Get());
    }
  }

  std::array<uint32_t, 5> prompts = {g_stock_prompts[0], g_stock_prompts[1], g_stock_prompts[2]};
  size_t count = 3;
  if (with_custom) {
    prompts[count++] = g_custom_prompt;
  }
  if (with_page) {
    prompts[count++] = g_page_prompt;
  }

  std::array<uint32_t, 5> widths = {};
  uint32_t total = 0;
  const auto measure = [&](float scale) {
    total = 0;
    for (size_t i = 0; i < count; ++i) {
      if (scale > 0.0f) {
        SetWidgetScale(ctx, base, prompts[i], scale);
      }
      widths[i] = PromptWidth(ctx, base, prompts[i]);
      total += widths[i];
    }
  };
  if (const uint32_t text = Read32BE(base, g_stock_prompts[0] + kPromptTextOffset)) {
    const float stock = std::bit_cast<float>(Read32BE(base, text + kWidgetTextScaleOffset));
    if (stock > 0.0f && stock <= 4.0f && stock != g_prompt_applied_scale) {
      g_prompt_scale = stock;
    }
  }
  const uint32_t min_total = kPromptMinGap * (static_cast<uint32_t>(count) + 1);
  float scale = g_prompt_scale;
  measure(scale);
  for (int i = 0; i < kPromptMaxShrinks && scale > 0.0f && total + min_total > kPromptBarWidth;
       ++i) {
    scale *= kPromptShrinkStep;
    measure(scale);
  }
  g_prompt_applied_scale = scale;
  if (total >= kPromptBarWidth) {
    return;  // nothing sensible to space; leave the stock three where they are
  }

  const uint32_t gap = (kPromptBarWidth - total) / (static_cast<uint32_t>(count) + 1);
  int32_t x = kPromptBarX + static_cast<int32_t>(gap);
  for (size_t i = 0; i < count; ++i) {
    PPCContext saved = ctx;
    ctx.r3.u32 = prompts[i];
    ctx.r4.u32 = static_cast<uint32_t>(x);
    ctx.r5.u32 = kPromptBarY;
    g_prompt_set_pos_fn(ctx, base);
    ctx = saved;
    x += static_cast<int32_t>(widths[i] + gap);
  }
}

// The page's description (OptionsPage::description), in a column left
// of the list: the 4:3 panel the screen is laid out in leaves the sides of the
// widescreen frame free (it spans about x=-105..745), and the column takes the
// left one, from the list's top down to the stone border above the prompt
// bar (y=367). The text is word-wrapped to the column by measuring it, and
// shrunk until it all fits.
constexpr int32_t kDescriptionLeft = -90;
constexpr int32_t kDescriptionRight = 100;
constexpr int32_t kDescriptionTop = 150;
constexpr int32_t kDescriptionBottom = 360;
constexpr float kDescriptionMaxScale = 0.6f;
constexpr float kDescriptionMinScale = 0.35f;
// Line advance per unit of text scale, and the extra gap between paragraphs
// as a fraction of a line.
constexpr float kDescriptionLineHeight = 34.0f;
constexpr float kDescriptionParagraphGap = 0.4f;
constexpr uint32_t kDescriptionWarningColour = 0xFFFF5040u;

struct DescriptionLine {
  std::u16string text;
  bool warning = false;
  bool paragraph_start = false;
};

// How wide `text` draws at `scale`, measured on `widget`.
int32_t MeasureText(PPCContext& ctx, uint8_t* base, uint32_t widget, const std::u16string& text,
                    float scale) {
  SetWidgetText(ctx, base, widget, text.c_str());
  SetWidgetScale(ctx, base, widget, scale);
  return static_cast<int32_t>(PromptWidth(ctx, base, widget));
}

// Breaks one paragraph into lines no wider than `width`: at spaces, or
// anywhere in a word too long for a line of its own (Japanese has no spaces).
void WrapParagraph(PPCContext& ctx, uint8_t* base, uint32_t scratch, std::u16string_view paragraph,
                   float scale, int32_t width, bool warning, std::vector<DescriptionLine>& out) {
  auto fits = [&](const std::u16string& text) {
    return MeasureText(ctx, base, scratch, text, scale) <= width;
  };
  bool first = true;
  std::u16string line;
  auto flush = [&] {
    out.push_back({std::move(line), warning, first});
    line.clear();
    first = false;
  };
  size_t at = 0;
  while (at < paragraph.size()) {
    const size_t space = paragraph.find(u' ', at);
    const size_t end = space == std::u16string_view::npos ? paragraph.size() : space;
    const std::u16string word(paragraph.substr(at, end - at));
    at = end + 1;
    if (word.empty()) {
      continue;
    }
    std::u16string candidate = line.empty() ? word : line + u' ' + word;
    if (fits(candidate)) {
      line = std::move(candidate);
      continue;
    }
    if (!line.empty()) {
      flush();
    }
    if (fits(word)) {
      line = word;
      continue;
    }
    for (char16_t c : word) {
      if (!line.empty() && !fits(line + c)) {
        flush();
      }
      line += c;
    }
  }
  if (!line.empty() || first) {
    flush();
  }
}

std::vector<DescriptionLine> WrapDescription(PPCContext& ctx, uint8_t* base, float scale) {
  std::vector<DescriptionLine> lines;
  const uint32_t scratch = g_description_lines[0];
  const int32_t width = kDescriptionRight - kDescriptionLeft;
  if (!g_description_warning.empty()) {
    WrapParagraph(ctx, base, scratch, g_description_warning, scale, width, true, lines);
  }
  std::u16string_view text = g_description_text;
  while (!text.empty()) {
    const size_t end = std::min(text.find(u'\n'), text.size());
    WrapParagraph(ctx, base, scratch, text.substr(0, end), scale, width, false, lines);
    text.remove_prefix(std::min(end + 1, text.size()));
  }
  return lines;
}

// How far down line `i` goes from the line before it.
float DescriptionAdvance(const std::vector<DescriptionLine>& lines, size_t i, float scale) {
  return kDescriptionLineHeight * scale *
         (lines[i].paragraph_start ? 1.0f + kDescriptionParagraphGap : 1.0f);
}

float DescriptionHeight(const std::vector<DescriptionLine>& lines, float scale) {
  float height = lines.empty() ? 0.0f : kDescriptionLineHeight * scale;
  for (size_t i = 1; i < lines.size(); ++i) {
    height += DescriptionAdvance(lines, i, scale);
  }
  return height;
}

void HideDescription(PPCContext& ctx, uint8_t* base) {
  for (uint32_t line : g_description_lines) {
    SetWidgetVisible(ctx, base, line, false);
  }
  g_description_shown = false;
}

// Lays g_description_warning and g_description_text out on the line widgets.
void ShowDescription(PPCContext& ctx, uint8_t* base) {
  float scale = kDescriptionMaxScale;
  std::vector<DescriptionLine> lines = WrapDescription(ctx, base, scale);
  while ((DescriptionHeight(lines, scale) > kDescriptionBottom - kDescriptionTop ||
          lines.size() > kDescriptionMaxLines) &&
         scale * 0.9f >= kDescriptionMinScale) {
    scale *= 0.9f;
    lines = WrapDescription(ctx, base, scale);
  }
  const uint32_t normal_colour = Read32BE(base, kPromptColourAddr);
  float y = kDescriptionTop;
  for (size_t i = 0; i < g_description_lines.size(); ++i) {
    const uint32_t widget = g_description_lines[i];
    if (i >= lines.size()) {
      SetWidgetVisible(ctx, base, widget, false);
      continue;
    }
    if (i > 0) {
      y += DescriptionAdvance(lines, i, scale);
    }
    SetWidgetText(ctx, base, widget, lines[i].text.c_str());
    SetWidgetScale(ctx, base, widget, scale);
    if (g_set_widget_colour_fn) {
      CallGuest(ctx, base, g_set_widget_colour_fn, widget,
                lines[i].warning ? kDescriptionWarningColour : normal_colour);
    }
    PPCContext saved = ctx;
    ctx.r3.u32 = widget;
    ctx.r4.u32 = static_cast<uint32_t>(kDescriptionLeft);
    ctx.r5.u32 = static_cast<uint32_t>(std::lround(y));
    g_prompt_set_pos_fn(ctx, base);
    ctx = saved;
    SetWidgetVisible(ctx, base, widget, true);
  }
  g_description_shown = true;
}

// Every frame: the page's description while it has one, laid out again only
// when its text changes (a new value, a language switch).
void UpdateDescription(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  if (screen != g_custom_prompt_screen || !g_description_lines[0] || !g_prompt_set_pos_fn ||
      !g_text_width_fn) {
    return;
  }
  PageDescription description = Page().description ? Page().description() : PageDescription{};
  if (Page().row_description && g_list) {
    const uint32_t selected = Read32BE(base, g_list + kListSelectedRow) + g_scroll;
    const int32_t index = FindActiveValueRow(selected);
    description = Page().row_description(selected, index >= 0 ? g_values[index] : 0);
  }
  if (description.warning.empty() && description.text.empty()) {
    if (g_description_shown) {
      HideDescription(ctx, base);
    }
    return;
  }
  if (g_description_shown && description.warning == g_description_warning &&
      description.text == g_description_text) {
    return;
  }
  g_description_warning = description.warning;
  g_description_text = description.text;
  ShowDescription(ctx, base);
}

// Adds the "Custom" prompt for Y and re-spaces the bar to fit four.
//
// Runs after the builder rather than inside it because the builder is where
// the screen pointer (the prompt's parent, which is what puts it in the draw
// list) and the finished stock prompts both exist.
extern "C" void NativeOptions_SettingsBuild(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;

  g_stock_prompts = {};
  if (g_original_settings_build_fn) {
    g_original_settings_build_fn(ctx, base);
  }

  if (!screen || g_stock_prompts[0] == 0 || g_stock_prompts[1] == 0 || g_stock_prompts[2] == 0) {
    return;
  }
  if (!EnsureArena(ctx, base)) {
    return;
  }

  // The builder runs once per screen object, but guard anyway: a second prompt
  // on the same screen would be invisible extra work at best.
  if (g_custom_prompt_screen != screen) {
    g_custom_prompt =
        CreateCustomPrompt(ctx, base, screen, kCustomPromptLabel.Get(), /*with_glyph=*/true);
    g_page_prompt =
        CreateCustomPrompt(ctx, base, screen, kChangePagePrompt.Get(), /*with_glyph=*/true);
    if (g_page_prompt && g_find_image_fn && g_prompt_set_glyph_fn) {
      const uint32_t image =
          CallGuest(ctx, base, g_find_image_fn, Read32BE(base, kImageBankPtrAddr), kLeftBumperGlyph);
      if (image) {
        CallGuest(ctx, base, g_prompt_set_glyph_fn, g_page_prompt, image);
      }
    }
    SetWidgetVisible(ctx, base, g_page_prompt, false);
    g_prompt_applied_scale = 0.0f;
    g_credit = CreateCustomPrompt(ctx, base, screen, u"", /*with_glyph=*/false);
    SetWidgetVisible(ctx, base, g_credit, false);
    g_title_arrows = {CreateImageWidget(ctx, base, screen, kLeftArrowImageName),
                      CreateImageWidget(ctx, base, screen, kRightArrowImageName)};
    for (uint32_t arrow : g_title_arrows) {
      SetWidgetVisible(ctx, base, arrow, false);
    }
    for (uint32_t& line : g_description_lines) {
      line = CreateCustomPrompt(ctx, base, screen, u"", /*with_glyph=*/false);
      SetWidgetVisible(ctx, base, line, false);
    }
    g_description_shown = false;
    g_custom_prompt_screen = g_custom_prompt ? screen : 0;
  }
  LayoutPromptBar(ctx, base);
}

// Keeps the pre-game stretch screen from being yanked away the frame after it
// opens.
//
// With no game running, the UI manager's per-frame update (sub_825AAE90) runs
// a watchdog: "game state settled && no page switch pending && not on page 0"
// makes it clear the save-loaded latch and post its own switch back to
// mPrevScreen. The stretch screen trips it every time, because its own
// Activate (sub_825BACC0) sets `mgr+332 = 0` -- so there is no winning the
// fight over that flag from outside; whatever we write, Activate clears it a
// frame later. In-game the whole block is disarmed (the state comparison is
// false while a game runs), which is why the stock path never meets any of
// this.
//
// Suppressing the resulting switch is the narrow fix: drop that one queued
// event, identified by its call site, and put back the latch the watchdog
// cleared on its way out. The watchdog sets the pending flag to 1 before
// posting, so having swallowed its post once it stays disarmed -- this fires
// exactly once per visit, not every frame.
extern "C" void NativeOptions_PostEvent(PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 == kEventClassPageSwitch && g_leaving_for_main_menu &&
      ctx.r5.u32 == kMainHelpOptionsPage) {
    ctx.r5.u32 = kMainMenuPage;
  }
  if (ctx.r3.u32 == kEventClassPageSwitch && g_pregame_screen_page != 0) {
    const uint32_t manager = Read32BE(base, kUiManagerPtrAddr);
    const uint32_t current_page = manager ? Read32BE(base, manager + kCurrentPageOffset) : 0;
    const uint32_t latch = InGameLatchAddress(base);

    if (static_cast<uint32_t>(ctx.lr) == kWatchdogPostLr && current_page == g_pregame_screen_page) {
      if (latch) {
        Write8(base, latch, 1);
      }
      return;  // swallow it -- deliberately not forwarded to the original
    }

    if (ctx.r5.u32 != g_pregame_screen_page) {
      // Leaving the screen under our own steam (A/B/X). Its Activate raised
      // the latch unconditionally and nothing lowers it again, so without
      // this the front-end would keep believing a save was loaded -- and the
      // settings screen we are returning to would then try to exit to the
      // pause menu instead of the main menu.
      g_pregame_screen_page = 0;
      if (latch) {
        Write8(base, latch, 0);
      }
    }
  }

  if (g_original_post_event_fn) {
    g_original_post_event_fn(ctx, base);
  }
}

// Moves the right arrow past the end of the selected row's value when the
// value is too long for the stock gap (a randomizer seed name runs well past
// it, and the arrow then lands on top of the text). Runs after the screen's
// update, which puts the arrow back at its fixed x every frame, so a short
// value gets the stock position again.
constexpr int32_t kRightArrowGap = 6;
void PushRightArrowPastValue(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  if (!g_text_width_fn) {
    return;
  }
  const uint32_t selected = Read32BE(base, g_list + kListSelectedRow);
  if (selected >= VisibleRowCount() || FindActiveValueRow(selected + g_scroll) < 0) {
    return;
  }
  const uint32_t arrow = Read32BE(base, screen + kScreenRightArrowOffset);
  const uint32_t value_widget = RowValueWidget(base, selected);
  if (!arrow || !value_widget) {
    return;
  }
  const auto value_x = static_cast<int32_t>(Read32BE(base, value_widget + kWidgetXOffset));
  // The value widget holds its text in an inner text widget, which is what
  // gets measured.
  const uint32_t text = Read32BE(base, value_widget + kWidgetInnerTextOffset);
  if (!text) {
    return;
  }
  const auto width = static_cast<int32_t>(CallGuest(ctx, base, g_text_width_fn, text));
  const int32_t x = value_x + width + kRightArrowGap;
  if (x > static_cast<int32_t>(Read32BE(base, arrow + kWidgetXOffset))) {
    Write32BE(base, arrow + kWidgetXOffset, static_cast<uint32_t>(x));
  }
}

// Holding left or right keeps cycling the selected row.
//
// The screen gets one left/right message per press: the list's handler
// (sub_825D4A58) steps the row once for message button 0 (left) or 1
// (right), and nothing repeats while the button stays down. So while one is
// held past kRepeatDelay, this posts the same message to the screen every
// kRepeatInterval, through the screen's own event handler, which plays the
// sound and runs NativeOptions_CycleRow exactly as a press would.
//
// Only a hold that started while the screen was updating counts, so one
// carried over from the screen before (or a dialog over this one) doesn't
// start cycling the moment it opens.
constexpr uint32_t kMsgButtonLeft = 0;
constexpr uint32_t kMsgButtonRight = 1;
constexpr auto kRepeatDelay = std::chrono::milliseconds(400);
constexpr auto kRepeatInterval = std::chrono::milliseconds(90);
constexpr int16_t kStickThreshold = 16384;
constexpr uint32_t kRepeatMessageSize = 32;

struct HeldDirection {
  int direction = 0;  // -1 left, +1 right, 0 neither
  uint32_t controller = 0;
};

// The first controller holding left or right, d-pad or left stick (the
// keyboard's bindings and the touch controls land in the same state).
HeldDirection ReadHeldDirection() {
  rex::Runtime* runtime = rex::Runtime::instance();
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (input == nullptr) {
    return {};
  }
  for (uint32_t user = 0; user < 4; ++user) {
    rex::input::X_INPUT_STATE state{};
    if (input->GetState(user, &state) != 0) {
      continue;
    }
    const uint16_t buttons = state.gamepad.buttons;
    const int16_t x = state.gamepad.thumb_lx;
    const bool left = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) != 0 || x < -kStickThreshold;
    const bool right = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) != 0 || x > kStickThreshold;
    if (left != right) {
      return {left ? -1 : 1, user};
    }
  }
  return {};
}

int g_repeat_direction = 0;
bool g_repeat_armed = false;
std::chrono::steady_clock::time_point g_repeat_next;
std::chrono::steady_clock::time_point g_repeat_last_update;
constexpr auto kRepeatGap = std::chrono::milliseconds(250);
uint32_t g_repeat_message = 0;

void RepeatHeldDirection(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  const HeldDirection held = ReadHeldDirection();
  const auto now = std::chrono::steady_clock::now();
  // The screen wasn't updating in between (closed, or covered): whatever is
  // held now was held before it came back.
  const bool resumed = now - g_repeat_last_update > kRepeatGap;
  g_repeat_last_update = now;
  if (resumed) {
    g_repeat_direction = held.direction;
    g_repeat_armed = false;
    return;
  }
  if (held.direction != g_repeat_direction) {
    // A new hold (or a release): armed only if it started here, from nothing.
    g_repeat_armed = g_repeat_direction == 0 && held.direction != 0;
    g_repeat_direction = held.direction;
    g_repeat_next = now + kRepeatDelay;
    return;
  }
  if (!g_repeat_armed || held.direction == 0 || now < g_repeat_next) {
    return;
  }
  g_repeat_next = now + kRepeatInterval;

  if (g_repeat_message == 0 && g_alloc_fn) {
    g_repeat_message = CallGuest(ctx, base, g_alloc_fn, kRepeatMessageSize);
  }
  if (g_repeat_message == 0) {
    return;
  }
  std::memset(base + g_repeat_message, 0, kRepeatMessageSize);
  Write32BE(base, g_repeat_message + kMsgClass, kMsgClassButton);
  Write32BE(base, g_repeat_message + kMsgButton,
            held.direction < 0 ? kMsgButtonLeft : kMsgButtonRight);
  Write32BE(base, g_repeat_message + kMsgController, held.controller);
  CallGuest(ctx, base, kSettingsEventFnAddr, screen, g_repeat_message);
}

// Gives list row 2 the left/right arrows every other row gets.
//
// The screen's per-frame update (sub_825B4CC0) hides both arrow widgets
// whenever row 2 is selected -- correct when that row opened a submenu, wrong
// now that it cycles a value. Showing them back afterwards, rather than
// skipping the original, keeps everything else that update does (including the
// base-class update it tails into) exactly as it was; the position is the same
// one the stock path computes for an ordinary row, taken from the selected
// row's own value widget.
extern "C" void NativeOptions_SettingsUpdate(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  const bool ours = screen != 0 && g_list != 0 && Read32BE(base, screen + kScreenListOffset) == g_list;
  const bool submenu_row_selected = ours &&
                                    Read32BE(base, g_list + kListSelectedRow) == kSubmenuRow &&
                                    FindActiveValueRow(kSubmenuRow + g_scroll) >= 0;

  if (g_original_settings_update_fn) {
    g_original_settings_update_fn(ctx, base);
  }
  if (ours) {
    RepeatHeldDirection(ctx, base, screen);
    UpdateDescription(ctx, base, screen);
    // The page's own prompt follows the selection onto and off its row.
    if (g_custom_prompt && PagePromptActive(base) != g_custom_prompt_shown) {
      LayoutPromptBar(ctx, base);
    }
  }

  // A locked row (OptionRow::locked) can't be cycled, so it gets no arrows.
  if (ours) {
    const int32_t index = FindActiveValueRow(Read32BE(base, g_list + kListSelectedRow) + g_scroll);
    if (index >= 0) {
      const OptionRow& row = Page().rows[index];
      if (row.locked && row.locked(row)) {
        for (uint32_t offset : {kScreenLeftArrowOffset, kScreenRightArrowOffset}) {
          if (const uint32_t arrow = Read32BE(base, screen + offset)) {
            SetWidgetVisible(ctx, base, arrow, false);
          }
        }
        return;
      }
    }
  }

  if (!submenu_row_selected) {
    if (ours) {
      PushRightArrowPastValue(ctx, base, screen);
    }
    return;
  }

  const uint32_t value_widget = RowValueWidget(base, kSubmenuRow);
  if (!value_widget) {
    return;
  }
  const uint32_t y = Read32BE(base, value_widget + kWidgetYOffset) + kArrowYBias;

  const uint32_t arrows[2] = {Read32BE(base, screen + kScreenLeftArrowOffset),
                              Read32BE(base, screen + kScreenRightArrowOffset)};
  const uint32_t xs[2] = {kArrowLeftX, kArrowRightX};
  for (int i = 0; i < 2; ++i) {
    if (!arrows[i]) {
      continue;
    }
    SetWidgetVisible(ctx, base, arrows[i], true);
    Write32BE(base, arrows[i] + kWidgetXOffset, xs[i]);
    Write32BE(base, arrows[i] + kWidgetYOffset, y);
  }
  PushRightArrowPastValue(ctx, base, screen);
}

// Puts every live row back to the value the screen opened with.
void RollBackLiveRows() {
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    const OptionRow& row = Page().rows[i];
    if (IsRowActive(row) && row.live && row.commit) {
      row.commit(row, g_seeded_values[i]);
    }
  }
}

// X on a page that owns all its rows: every row back to its default, shown
// but not applied until A, like any other change. The stock handler would
// reset the game's own Graphics/Volume model instead, which this page doesn't
// show, so it never sees the press.
void ResetPageToDefaults(PPCContext& ctx, uint8_t* base) {
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    const OptionRow& row = Page().rows[i];
    if (row.locked && row.locked(row)) {
      continue;
    }
    const int32_t value = row.reset ? row.reset(row) : g_seeded_values[i];
    g_values[i] = std::clamp(value, RowMin(row), RowMax(row));
    if (row.live && row.commit) {
      row.commit(row, g_values[i]);
    }
  }
  if (!RelayoutPage(ctx, base)) {
    BindVisibleRows(ctx, base);
  }
}

// A commits every row of ours. B rolls back the live ones: they have been
// applying as the player cycled, so cancelling has to undo that; the non-live
// ones need nothing, since the re-bind on the way out reloads them from their
// own `get`. X ("defaults") does the same rollback and repaints, or on a page
// that owns all its rows, resets them (ResetPageToDefaults).
void SwitchPage(PPCContext& ctx, uint8_t* base, const OptionsPage& page);

extern "C" void NativeOptions_SettingsEvent(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t message = ctx.r4.u32;
  bool reset_rows = false;
  bool restore_selected_row = false;

  if (message && g_list) {
    const uint32_t message_class = Read32BE(base, message + kMsgClass);
    const uint32_t button = Read32BE(base, message + kMsgButton);

    // Stop A on list row 2 from opening the old resolution screen.
    //
    // The handler navigates on `selected_row == 2 && save_loaded`, so with a
    // save loaded A on this row would leave the settings screen entirely
    // instead of accepting like every other row. The narrow fix is to hide
    // the row number across that one call: with anything else selected the
    // handler takes its ordinary accept-and-close path, which is exactly what
    // this row wants. Restricted to the A message on purpose -- the messages
    // that fall through to the base list handler (sub_825D1410) *do* read and
    // write the selected row, and lying to those would break navigation.
    //
    // This also settles the prompt-bar hint the same function sets from the
    // same comparison at its top (id 9, "opens a submenu", vs id 4 for an
    // ordinary row) -- but only for this one message, hence the unconditional
    // correction further down.
    if (message_class == kMsgClassButton && button == kButtonAccept &&
        Read32BE(base, g_list + kListSelectedRow) == kSubmenuRow &&
        FindActiveValueRow(kSubmenuRow + g_scroll) >= 0) {
      Write32BE(base, g_list + kListSelectedRow, 0);
      restore_selected_row = true;
    }

    // Y opens the stretch screen the Preset row displaced, for the finer
    // control it offers (arbitrary percentages rather than the 7 presets).
    //
    // Posting the page switch here, instead of letting the handler do it,
    // means not having to borrow the save-loaded latch to reach the branch
    // that would: the only thing the latch is still needed for is the
    // pre-game watchdog (see NativeOptions_PostEvent). Returning 1 without
    // running the original is what the stock navigation path does too.
    // kButtonStretchScreen is Y by the same A=4/B=5/X=6 ordering the screen's
    // own prompts confirm, which is as far as static RE got: the code that
    // turns pad bits into these ids couldn't be pinned down (the button tests
    // compile to rlwinm., not a searchable mask constant). If Y turns out to
    // arrive as something else, this logs the real id on the first press.
    if (message_class == kMsgClassButton && g_entries &&
        (button == kButtonPreviousPage || button == kButtonNextPage)) {
      if (const OptionsPage* neighbour =
              NeighbourPage(Page(), button == kButtonNextPage ? 1 : -1)) {
        SwitchPage(ctx, base, *neighbour);
        ctx.r3.u32 = 1;
        return;
      }
    }

    if (message_class == kMsgClassButton && button > kButtonStretchScreen) {
      REXLOG_INFO("[native_options] settings screen: unhandled button id {}", button);
    }

    if (message_class == kMsgClassButton && button == kButtonStretchScreen &&
        Page().stretch_prompt && PagePromptActive(base)) {
      const uint32_t controller = Read32BE(base, message + kMsgController);
      const uint32_t latch = InGameLatchAddress(base);
      g_pregame_screen_page = (latch != 0 && Read8(base, latch) == 0) ? kResolutionPage : 0;
      CallGuestPageSwitch(ctx, base, kResolutionPage, controller);
      ctx.r3.u32 = 1;
      return;
    }

    if (message_class == kMsgClassButton && button == kButtonDefaults && g_entries &&
        Page().owns_all_rows) {
      ResetPageToDefaults(ctx, base);
      ctx.r3.u32 = 1;
      return;
    }

    if (message_class == kMsgClassButton && g_entries) {
      if (button == kButtonAccept) {
        for (uint32_t i = 0; i < Page().row_count; ++i) {
          const OptionRow& row = Page().rows[i];
          if (IsRowActive(row) && row.commit) {
            row.commit(row, g_values[i]);
          }
        }
        if (Page().accept) {
          Page().accept();
        }

        // Auto-restart on accept, but only from the main-menu entry point,
        // never the pause-menu one: a restart there would silently discard
        // whatever run is in progress, with no save prompt (this screen has
        // no way to warn the player first). The main-menu entry has nothing
        // to lose, so there's no reason to make the player find and press
        // the ImGui overlay's own "Restart Now" button separately -- accept
        // already means "I'm done changing settings."
        //
        // Same in-game latch this screen already reads for the pregame
        // watchdog/stretch-screen tracking above (0 = no save loaded = this
        // is the main-menu instance). A null latch (app object not resolved
        // yet) is treated as "not main menu" -- conservatively skip rather
        // than restart when that can't be determined.
        const uint32_t latch = InGameLatchAddress(base);
        const bool main_menu_instance = latch != 0 && Read8(base, latch) == 0;
        if (main_menu_instance && AnyPendingRestart()) {
          // window->RequestClose() ends up in WindowSDL::RequestCloseImpl,
          // which calls SDL_DestroyWindow directly (not through SDL's event
          // queue, unlike Window::RequestPaint -- see its own "callable from
          // non-UI threads" comment, which RequestClose conspicuously lacks).
          // This handler runs on the guest CPU thread ("Main XThread"), a
          // different OS thread from the one that owns the window -- calling
          // RequestClose from here directly left the old process's window
          // never actually closing (confirmed live: still running, playing
          // audio, after the relaunched process had already opened -- lldb
          // showed the new process's own boot then stalling too, almost
          // certainly contending with the old process for exclusive
          // GPU/audio device ownership). CallInUIThread marshals the actual
          // close onto the thread that owns the window, the same mechanism
          // window.h documents "functions requested to be executed in the UI
          // thread... are also allowed to destroy windows" for.
          // RestartNow marshals the close onto the UI thread: this handler
          // runs on the guest CPU thread, and closing the window from here left
          // the old process running next to the relaunched one.
          const std::string pending = fmt::format("{}", fmt::join(rex::cvar::GetPendingRestartFlags(), ", "));
          if (RestartNow()) {
            REXLOG_INFO("[native_options] restart-pending change(s) accepted from the main "
                        "menu ({}); relaunching", pending);
          }
        }
      } else if (button == kButtonCancel) {
        RollBackLiveRows();
      } else if (button == kButtonDefaults) {
        // Before the original, whose re-bind then seeds from the rolled-back
        // values.
        RollBackLiveRows();
        reset_rows = true;
      }
    }
  }

  // The randomizer pages were opened from the main menu, so they leave for it
  // where the screen goes back to Help & Options. See NativeOptions_PostEvent.
  g_leaving_for_main_menu = IsRandomizerPage(Page());
  if (g_original_settings_event_fn) {
    g_original_settings_event_fn(ctx, base);
  }
  g_leaving_for_main_menu = false;

  if (restore_selected_row) {
    Write32BE(base, g_list + kListSelectedRow, kSubmenuRow);
  }

  // Every message repaints the prompt bar from `selected == 2 ? 9 : 4`, so
  // correcting it has to happen here rather than once on entry. Cheap enough
  // to do unconditionally on the row: it is the same setter the screen itself
  // just called, with the id every other row gets.
  if (screen && g_list && g_set_widget_text_by_id_fn &&
      Read32BE(base, screen + kScreenListOffset) == g_list &&
      Read32BE(base, g_list + kListSelectedRow) == kSubmenuRow &&
      FindActiveValueRow(kSubmenuRow + g_scroll) >= 0) {
    const uint32_t prompt = Read32BE(base, screen + kScreenPromptOffset);
    if (prompt) {
      CallGuest(ctx, base, g_set_widget_text_by_id_fn, prompt, kPromptOrdinaryRowStringId);
    }
  }

  if (reset_rows) {
    for (uint32_t i = 0; i < Page().row_count; ++i) {
      if (IsRowActive(Page().rows[i]) && !IsLabelOnly(Page().rows[i])) {
        SeedRow(i);
      }
    }
    WriteEntries(base);
    RenderRows(ctx, base);
  }
}

}  // namespace

void EnterPregameScreen(uint32_t page) { g_pregame_screen_page = page; }

void SelectSettingsPage(SettingsPage page) {
  switch (page) {
    case SettingsPage::kSettings:
      g_page = &g_settings_page;
      break;
    case SettingsPage::kEnhancements:
      g_page = &EnhancementsOptionsPage();
      break;
    case SettingsPage::kRandomizer:
      BeginRandomizerEdit();
      g_page = &RandomizerOptionsPage();
      break;
  }
}

namespace {

// Centres a title widget on x=320 by its measured width, as every screen's
// Activate does. A paged title gets the row arrows either side of it, and a
// page with a credit puts it right of where the right arrow goes, at the
// title's height; the title stays centred on its own.
constexpr int32_t kCreditGap = 12;
constexpr int32_t kTitleArrowGap = 10;
constexpr int32_t kTitleArrowYBias = 2;
void CentreTitle(PPCContext& ctx, uint8_t* base, uint32_t title) {
  const auto width = static_cast<int32_t>(CallGuest(ctx, base, g_text_width_fn, title));
  const bool credit = g_credit && g_prompt_set_pos_fn && Page().credit.key != nullptr;
  if (g_credit) {
    SetWidgetVisible(ctx, base, g_credit, credit);
  }
  if (credit) {
    SetWidgetText(ctx, base, g_credit, Page().credit.Get());
  }
  const int32_t x = kTitleCenterX - width / 2;
  const auto y = static_cast<int32_t>(Read32BE(base, title + kWidgetYOffset));
  Write32BE(base, title + kWidgetXOffset, static_cast<uint32_t>(x));

  const uint32_t right_arrow = g_title_arrows[1];
  const int32_t right_arrow_width =
      right_arrow ? static_cast<int32_t>(Read32BE(base, right_arrow + kImageWidgetWidthOffset)) : 0;
  for (int i = 0; i < 2; ++i) {
    const uint32_t arrow = g_title_arrows[i];
    if (!arrow) {
      continue;
    }
    SetWidgetVisible(ctx, base, arrow, Page().title_arrows);
    const auto arrow_width = static_cast<int32_t>(Read32BE(base, arrow + kImageWidgetWidthOffset));
    const int32_t arrow_x = i == 0 ? x - kTitleArrowGap - arrow_width : x + width + kTitleArrowGap;
    Write32BE(base, arrow + kWidgetXOffset, static_cast<uint32_t>(arrow_x));
    Write32BE(base, arrow + kWidgetYOffset, static_cast<uint32_t>(y + kTitleArrowYBias));
  }

  if (credit) {
    PPCContext saved = ctx;
    ctx.r3.u32 = g_credit;
    ctx.r4.u32 = static_cast<uint32_t>(x + width + kTitleArrowGap + right_arrow_width + kCreditGap);
    ctx.r5.u32 = static_cast<uint32_t>(y);
    g_prompt_set_pos_fn(ctx, base);
    ctx = saved;
  }
}

// Redoes the tail of the screen's Activate (sub_825B43E0) that sets and
// centres the title, or sets the page's own. Needed after a language switch
// because the signed-in title is formatted text and a page's title literal
// text, so the by-id re-texting in language_switch.cpp never sees either.
void RefreshSettingsTitle(PPCContext& ctx, uint8_t* base) {
  if (!g_settings_screen || !g_dispatcher || !g_alloc_fn) {
    return;
  }
  const uint32_t title = Read32BE(base, g_settings_screen + kScreenTitleOffset);
  const uint32_t app = Read32BE(base, kAppObjectPtrAddr);
  if (!title || !app) {
    return;
  }
  if (Page().title.key) {
    SetTextWidgetText(ctx, base, title, Page().title.Get());
    CentreTitle(ctx, base, title);
    return;
  }
  if (!g_title_scratch) {
    g_title_scratch = CallGuest(ctx, base, g_alloc_fn, kTitleChars * 2 + kUserNameBytes);
    if (!g_title_scratch) {
      return;
    }
  }

  const uint32_t signed_in_fn = Read32BE(base, Read32BE(base, app) + kAppSignedInVtableSlot);
  const bool signed_in =
      (CallGuest(ctx, base, g_dispatcher->GetFunction(signed_in_fn), app, g_settings_user) &
       0xFF) != 0;
  if (signed_in) {
    const uint32_t text = g_title_scratch;
    const uint32_t name = g_title_scratch + kTitleChars * 2;
    CallGuest(ctx, base, kGetUserNameFnAddr, g_settings_user, name, kUserNameBytes);
    CallGuest(ctx, base, kFormatStringFnAddr, text, kTitleChars, kTitleWithNameStringId, name);
    CallGuest(ctx, base, kSetTextFnAddr, title, text);
  } else {
    CallGuest(ctx, base, kSetTextByIdFnAddr, title, kTitleStringId);
  }
  CentreTitle(ctx, base, title);
}

// What the current page changes about the screen around its rows: the title,
// when the page has its own (the stock one has just been set by Activate), and
// which prompts the bar shows.
void ApplyPageChrome(PPCContext& ctx, uint8_t* base) {
  if (Page().title.key && g_settings_screen) {
    const uint32_t title = Read32BE(base, g_settings_screen + kScreenTitleOffset);
    if (title) {
      SetTextWidgetText(ctx, base, title, Page().title.Get());
      CentreTitle(ctx, base, title);
    }
  }
  LayoutPromptBar(ctx, base);
  if (g_settings_screen && !Page().title.key) {
    // A stock page has no credit and no title arrows.
    SetWidgetVisible(ctx, base, g_credit, false);
    for (uint32_t arrow : g_title_arrows) {
      SetWidgetVisible(ctx, base, arrow, false);
    }
  }
}

// Opens another page of a group on the open screen (the shoulder buttons).
// What the player changed on the page being left is committed first, as A
// would: the rows are re-seeded from their cvars on the new page, and the old
// ones would otherwise be lost. Then the new page is bound the way
// NativeOptions_BindRows binds a page that was just selected.
void SwitchPage(PPCContext& ctx, uint8_t* base, const OptionsPage& page) {
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    const OptionRow& row = Page().rows[i];
    if (IsRowActive(row) && row.commit) {
      row.commit(row, g_values[i]);
    }
  }

  g_page = &page;
  g_bound_page = &page;
  g_scroll = 0;
  if (Page().relayout) {
    Page().relayout();
  }
  Write32BE(base, g_list + kListSelectedRow, 0);
  // Back on a page that shows the stock rows, after one that wrote its own
  // rows over theirs: they come back from the game's table, which every
  // change to them was mirrored into (CopyEntryToVanilla), as the screen's
  // own bind does.
  if (!Page().owns_all_rows && g_vanilla_entries) {
    for (uint32_t i = 0; i < kVanillaRowCount * kEntryStride; i += 4) {
      Write32BE(base, g_entries + i, Read32BE(base, g_vanilla_entries + i));
    }
  }
  for (uint32_t row = 0; row < ListCapacity(); ++row) {
    SetWidgetScale(ctx, base, RowValueWidget(base, row), ListTextScale(base));
  }
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    if (IsRowActive(Page().rows[i]) && !IsLabelOnly(Page().rows[i])) {
      SeedRow(i);
    }
  }
  BindVisibleRows(ctx, base);

  if (g_settings_screen && Page().title.key) {
    const uint32_t title = Read32BE(base, g_settings_screen + kScreenTitleOffset);
    if (title) {
      SetTextWidgetText(ctx, base, title, Page().title.Get());
      CentreTitle(ctx, base, title);
    }
  }
  LayoutPromptBar(ctx, base);
}

// Repaints the value of every row the game renders itself (stock Graphics,
// and any numeric row). The list's bind and cycle both format the entry into a
// buffer and set that as literal text, so the by-id re-texting in
// language_switch.cpp never sees it: after a switch Graphics kept its value
// in the old language, drawn with the new font. Enumerated rows of ours are
// left out, since RenderRow paints those.
//
// The literal setter leaves the widget's scale alone, so the selected row
// keeps its larger one.
void RefreshStockRowValues(PPCContext& ctx, uint8_t* base) {
  if (!g_alloc_fn) {
    return;
  }
  if (!g_value_scratch) {
    g_value_scratch = CallGuest(ctx, base, g_alloc_fn, kEntryValueChars * 2);
    if (!g_value_scratch) {
      return;
    }
  }
  for (uint32_t row = 0; row < VisibleRowCount(); ++row) {
    const int32_t index = FindActiveValueRow(row + g_scroll);
    if (index >= 0 && IsEnumRow(Page().rows[index])) {
      continue;
    }
    const uint32_t widget = RowValueWidget(base, row);
    if (!widget) {
      continue;
    }
    CallGuest(ctx, base, kFormatEntryValueFnAddr, g_entries + row * kEntryStride,
              g_value_scratch, kEntryValueChars);
    CallGuest(ctx, base, kSetWidgetTextFnAddr, widget, g_value_scratch);
  }
}

}  // namespace

void RefreshNativeOptionsText(PPCContext& ctx, uint8_t* base) {
  RefreshSettingsTitle(ctx, base);
  if (!g_list || !g_entries) {
    return;
  }
  RefreshStockRowValues(ctx, base);
  for (uint32_t i = 0; i < Page().row_count; ++i) {
    const OptionRow& row = Page().rows[i];
    if (!IsRowActive(row)) {
      continue;
    }
    // A label-only row with nothing to say in this language (Graphics outside
    // Italian) goes back to the game's own label, keeping the widget's scale,
    // which is larger on the selected row.
    if (IsLabelOnly(row) && row.label.key && !row.label.Get() && g_set_widget_text_by_id_fn) {
      const uint32_t widget = RowLabelWidget(base, row.list_row);
      const uint32_t inner = widget ? Read32BE(base, widget + kWidgetInnerTextOffset) : 0;
      if (inner) {
        const uint32_t scale = Read32BE(base, inner + kWidgetTextScaleOffset);
        CallGuest(ctx, base, g_set_widget_text_by_id_fn, widget,
                  Read32BE(base, g_entries + row.list_row * kEntryStride + kEntryLabelId));
        SetWidgetScale(ctx, base, widget, std::bit_cast<float>(scale));
      }
    }
    RenderRow(ctx, base, i);
  }

  if (g_custom_prompt) {
    LayoutPromptBar(ctx, base);
  }
}

void NativeOptions::Bind(rex::Runtime* runtime) {
  runtime_ = runtime;
  if (!runtime_ || !runtime_->function_dispatcher()) {
    return;
  }
  auto* dispatcher = runtime_->function_dispatcher();
  g_dispatcher = dispatcher;
  g_runtime = runtime_;

  if (!runtime_->user_data_root().empty()) {
    g_user_settings_path = runtime_->user_data_root() / "settings.toml";
  }

  g_enable_row_fn = dispatcher->GetFunction(kEnableRowFnAddr);
  g_set_widget_colour_fn = dispatcher->GetFunction(kSetWidgetColourFnAddr);
  g_set_widget_text_by_id_fn = dispatcher->GetFunction(kSetWidgetTextByIdFnAddr);
  g_prompt_ctor_fn = dispatcher->GetFunction(kPromptCtorFnAddr);
  g_prompt_set_glyph_fn = dispatcher->GetFunction(kPromptSetGlyphFnAddr);
  g_prompt_show_glyph_fn = dispatcher->GetFunction(kPromptShowGlyphFnAddr);
  g_prompt_text_offset_fn = dispatcher->GetFunction(kPromptTextOffsetFnAddr);
  g_prompt_set_pos_fn = dispatcher->GetFunction(kPromptSetPosFnAddr);
  g_text_width_fn = dispatcher->GetFunction(kTextWidthFnAddr);
  g_image_widget_ctor_fn = dispatcher->GetFunction(kImageWidgetCtorFnAddr);
  g_find_image_fn = dispatcher->GetFunction(kFindImageFnAddr);
  g_alloc_fn = dispatcher->GetFunction(kAllocFnAddr);
  if (!g_alloc_fn) {
    REXLOG_WARN("[native_options] guest alloc unavailable; extra rows will misbehave");
  }

  // The screen's functions are hooked at codegen time (config/nocturnerecomp_config.toml).
  REXLOG_INFO("[native_options] hooks installed ({} settings row(s), {} enhancements row(s))",
              std::size(kSettingsRows), EnhancementsOptionsPage().row_count);
}

NativeOptions& GetNativeOptions() {
  static NativeOptions instance;
  return instance;
}

}  // namespace nocturne

// Entry hooks, wired up in config/nocturnerecomp_config.toml.
bool NativeOptions_ListSetupHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kListSetupFnAddr, &nocturne::NativeOptions_ListSetup);
}
bool NativeOptions_BindRowsHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kBindRowsFnAddr, &nocturne::NativeOptions_BindRows);
}
bool NativeOptions_CycleRowHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kCycleRowFnAddr, &nocturne::NativeOptions_CycleRow);
}
bool NativeOptions_MoveSelectionHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kMoveSelectionFnAddr, &nocturne::NativeOptions_MoveSelection);
}
bool NativeOptions_ListUpdateHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kListUpdateFnAddr, &nocturne::NativeOptions_ListUpdate);
}
bool NativeOptions_SettingsEventHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kSettingsEventFnAddr, &nocturne::NativeOptions_SettingsEvent);
}
bool NativeOptions_SettingsActivateHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kSettingsActivateFnAddr, &nocturne::NativeOptions_SettingsActivate);
}
bool NativeOptions_PromptBarLayoutHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kPromptBarLayoutFnAddr, &nocturne::NativeOptions_PromptBarLayout);
}
bool NativeOptions_SettingsBuildHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kSettingsBuildFnAddr, &nocturne::NativeOptions_SettingsBuild);
}
bool NativeOptions_PostEventHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kPostEventFnAddr, &nocturne::NativeOptions_PostEvent);
}
bool NativeOptions_SettingsUpdateHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kSettingsUpdateFnAddr, &nocturne::NativeOptions_SettingsUpdate);
}
