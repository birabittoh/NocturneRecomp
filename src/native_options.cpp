// native_options - appends extra rows to the game's own settings screen
// (main menu or pause -> help & options -> settings), cycled in place with
// left/right exactly like the stock "Graphics" and "Volume" rows.
//
// ---------------------------------------------------------------------------
// Adding a row
// ---------------------------------------------------------------------------
//
// Append an entry to kCustomRows below and you are done. Widget creation,
// layout, left/right cycling with wraparound, commit on A, re-seed on B/X and
// the localized label are all handled generically from the table.
//
// A row is either:
//   * enumerated -- give it `values` (the display text per value) and
//     `value_count`; indices run 0..value_count-1, and this file paints the
//     text; or
//   * numeric -- leave `values` null and fill in min/max/step/multiplier;
//     the game's own renderer prints `multiplier * value`, same as the stock
//     Volume row, and this file only paints the label.
//
// `get` supplies the value to show when the screen opens; `commit` is handed
// the value when the player accepts with A. Nothing else needs touching.
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
// matters: the handler only opens the resolution screen when the selected row
// is exactly 2 (and sub_825B4CC0 hides the left/right arrows on that same
// row), so anything inserted *before* index 3 would silently rewire both
// checks. Past index 2 a row draws the arrows and takes A as "accept and
// close", which is what the stock rows 0/1 do too.
//
// Note this screen is *shared*: on A or B its handler checks the in-game flag
// (byte at 0x82E4F808+29) and returns to page 18 (the pause menu) when a save
// is loaded, or page 4 (the main menu) when not. One instance, two entry
// points -- so these rows deliberately show in the main-menu settings as well
// as the paused one. That is distinct from the pre-game "Graphics"-only
// screen, which is a different screen entirely and is not touched here.
#include "native_options.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>

#include "settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/gpu_plugin.h>

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
constexpr uint32_t kEnableRowFnAddr = 0x825D3530u;         // (list, row)
constexpr uint32_t kListUpdateFnAddr = 0x825D38A8u;        // (list, delta)
constexpr uint32_t kSetWidgetColourFnAddr = 0x825D2138u;   // (widget, argb)
constexpr uint32_t kSetWidgetTextFnAddr = 0x825D1EE0u;     // (widget, utf16_text)
constexpr uint32_t kSetTextScaleFnAddr = 0x825D20C0u;      // (widget, scale) -- scale in f1
constexpr uint32_t kSettingsEventFnAddr = 0x825B3F58u;     // (screen, message)
constexpr uint32_t kSettingsActivateFnAddr = 0x825B43E0u;  // (screen, user_index)
constexpr uint32_t kAllocFnAddr = 0x82576950u;             // (size) -> pointer

// Every front-end page change goes through this one queue push, which is where
// the pre-game watchdog switch gets filtered out. See NativeOptions_PostEvent.
constexpr uint32_t kPostEventFnAddr = 0x825CE8E8u;  // (class, arg1, page, controller)
constexpr uint32_t kEventClassPageSwitch = 8;

// The exact call site of the front-end watchdog's page switch, inside the UI
// manager's per-frame update (sub_825AAE90). Filtering on the return address
// keeps the suppression to that one post -- every other switch the manager
// makes, including the genuine "user signed out" one a few lines above it,
// goes through untouched.
constexpr uint32_t kWatchdogPostLr = 0x825AB030u;

// Front-end page index of the "Change Screen Size" screen, and where the
// manager keeps the current one.
constexpr uint32_t kResolutionPage = 17;
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
// sub_825D2C48 and sub_825D32B0. See SetWidgetText for why we reassert it.
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
// Hard ceiling on rows: sub_825D3200 zeroes exactly 16 value-widget slots.
constexpr uint32_t kListMaxRows = 16;

// Setting-entry layout.
constexpr uint32_t kEntryStride = 96;
constexpr uint32_t kEntryLabelId = 0;
constexpr uint32_t kEntryValue = 4;
constexpr uint32_t kEntryMin = 12;
constexpr uint32_t kEntryMax = 16;
constexpr uint32_t kEntryStep = 20;
constexpr uint32_t kEntryMultiplier = 24;

// The stock screen's three rows: Graphics / Volume / Change Screen Size...
constexpr uint32_t kVanillaRowCount = 3;

// The screen's own option list, off the screen instance.
constexpr uint32_t kScreenListOffset = 560;

// Rows the stock Activate greys out until a save is loaded: Volume and
// "Change Screen Size...". See NativeOptions_SettingsActivate.
constexpr uint32_t kGatedFirstRow = 1;
constexpr uint32_t kGatedLastRow = 2;

// "Change Screen Size...", the one row whose A opens another screen.
constexpr uint32_t kResolutionScreenRow = 2;

// The app object (a pointer *stored at* this address, not the object itself)
// and the "a save is loaded" latch inside it. sub_825B3F58 gates both its
// return page and its Change-Screen-Size navigation on this byte.
constexpr uint32_t kAppObjectPtrAddr = 0x82E4F808u;
constexpr uint32_t kInGameLatchOffset = 29;

// The UI manager, again a pointer stored at this address.
constexpr uint32_t kUiManagerPtrAddr = 0x82E7A02Cu;

// Message layout for a screen's event handler: word 0 is the event class
// (0 = button press) and word 2 is the button. 4/5/6 are A/B/X, matching the
// screen's own "X defaults, A accept, B cancel" prompt bar.
constexpr uint32_t kMsgClass = 0;
constexpr uint32_t kMsgButton = 8;
constexpr uint32_t kMsgClassButton = 0;
constexpr uint32_t kButtonAccept = 4;
constexpr uint32_t kButtonCancel = 5;
constexpr uint32_t kButtonDefaults = 6;

// Mirrors graphics_settings.cpp's XLanguageId -- the six languages the
// in-game Language dropdown offers; anything else falls back to English.
enum class XLanguageId : uint32_t {
  kEnglish = 1,
  kJapanese = 2,
  kGerman = 3,
  kFrench = 4,
  kSpanish = 5,
  kItalian = 6,
};

XLanguageId CurrentLanguage() {
  return static_cast<XLanguageId>(REXCVAR_QUERY(uint32_t, user_language));
}

// True once `name`'s cvar has actually been changed at runtime this session
// and needs a relaunch to take effect -- the same source the ImGui settings
// overlay's own "Some changes require a restart to take effect." banner
// reads (see settings.cpp's AnyPendingRestart/IsPendingRestart). Committing a
// restart-scoped row here goes through the same rex::cvar::SetFlagByName path
// the overlay uses, so it is tracked automatically; this just reads it back.
bool CvarPendingRestart(const char* name) {
  const auto pending = rex::cvar::GetPendingRestartFlags();
  return std::find(pending.begin(), pending.end(), name) != pending.end();
}

// Set once by NativeOptions::Bind from Runtime::user_data_root(); empty means
// Bind() couldn't resolve it (or hasn't run yet), in which case
// SaveToUserSettings below just no-ops rather than guessing a path.
std::filesystem::path g_user_settings_path;

// Persists a single cvar to the same settings.toml the ImGui overlay's Basic
// section writes (see settings.cpp's SaveBasic/user_settings_path). Setting a
// cvar with persist=true only flags it as *worth* saving next time someone
// saves -- unlike the overlay, which calls SaveBasic() itself after every
// change, nothing here was actually writing the file, so a value picked from
// this screen looked committed (it applied, and the restart marker/banner
// both showed) but reverted to whatever was last saved on the next launch.
//
// Resolved from g_user_settings_path (see NativeOptions::Bind), NOT the
// `user_data_root` *cvar* -- that cvar (runtime.h) is only a command-line
// override and defaults to "", so reading it back here would silently no-op
// on every normal launch. The actual resolved path lives on the Runtime
// object (Runtime::user_data_root()), which Bind() only has once, so it's
// cached in a global rather than re-derived per commit.
void SaveToUserSettings(const char* cvar_name) {
  if (g_user_settings_path.empty()) {
    return;
  }
  rex::cvar::SaveConfigSubset(g_user_settings_path, {cvar_name});
}

// Appends a "(Restart)" marker to a row label when its cvar is pending
// restart -- the only way to surface that state on this screen, since it has
// no free area of its own to put a banner in and a player without the ImGui
// overlay open (F4) would otherwise have no way to see it at all.
//
// Returns a pointer into a single shared static buffer rather than a fresh
// string per call: every label() a row supplies is consumed immediately by
// SetWidgetText (which copies it into the widget's own buffer) and never
// held onto past that one call, so nothing else needs the previous row's text
// by the time this is asked for the next one.
const char16_t* AppendRestartMarker(const char16_t* text, bool pending_restart) {
  static char16_t buffer[64];
  if (!pending_restart || !text) {
    return text;
  }
  constexpr char16_t kMarker[] = u" (Restart)";
  size_t i = 0;
  while (text[i] != u'\0' && i < std::size(buffer) - std::size(kMarker)) {
    buffer[i] = text[i];
    ++i;
  }
  for (size_t j = 0; j < std::size(kMarker); ++j) {
    buffer[i + j] = kMarker[j];
  }
  return buffer;
}

// ---------------------------------------------------------------------------
// Row table
// ---------------------------------------------------------------------------

struct CustomRow {
  // Which row of the list this occupies.
  //
  // >= kVanillaRowCount appends a new row (they must run contiguously up from
  // kVanillaRowCount). < kVanillaRowCount *takes over* a stock row instead:
  // the row keeps its position and its own localized label, but its value is
  // seeded from `get` and applied through `commit`, and it is no longer
  // mirrored back into the game's model -- so whatever the stock row used to
  // drive is left alone entirely.
  uint32_t list_row;

  // Localized label, re-read every time the row is painted so a mid-session
  // language change is picked up on the next visit to the screen. Leave null
  // on an override row to keep the game's own label (which is the usual
  // choice: it is already localized, and set at the right scale).
  const char16_t* (*label)() = nullptr;

  // Enumerated row: one display string per value, indices 0..value_count-1.
  // Null for a numeric row, which uses the min/max/step/multiplier below and
  // is rendered by the game itself.
  const char16_t* const* values = nullptr;
  uint32_t value_count = 0;

  // Numeric row bounds. Ignored when `values` is set.
  int32_t min = 0;
  int32_t max = 0;
  int32_t step = 1;
  int32_t multiplier = 1;

  // Value to show when the screen opens, and where it goes on A.
  //
  // Both null makes this a *label-only* row: the stock row keeps its value,
  // its behaviour and its model copy-back untouched, and only its text is
  // replaced. Useful for correcting a label without adopting the setting.
  int32_t (*get)() = nullptr;
  void (*commit)(int32_t value) = nullptr;

  // Apply on every left/right as well as on A, so the player gets immediate
  // feedback (and B puts the original value back). Right for things you can
  // perceive while the menu is open, like volume; pointless for restart-scoped
  // settings.
  bool live = false;

  // Override rows only: force the stock setting this row displaced to its
  // maximum, so whatever now backs the row has the full range to work with
  // rather than being multiplied down by a leftover value nobody can reach
  // any more.
  bool pin_stock_to_max = false;

  // Optional runtime gate. Returning false leaves an override row completely
  // alone -- the stock row keeps its vanilla behaviour, model copy-back and
  // all. An appended row may use this too, but only the *last* one in the
  // table: NativeOptions::Bind folds the decision into g_active_row_count
  // before the list is ever built, so an inactive trailing row's widget
  // simply never gets created rather than being left blank. An appended row
  // that isn't last can't use this -- suppressing it would leave a gap.
  bool (*available)() = nullptr;
};

// --- Resolution -------------------------------------------------------------

// `resolution` is consumed when the render target and video mode are sized
// (native_command_processor.cpp's ResolveVideoModeWidth/Height), not polled
// per frame, so a change here lands on the next launch -- the same
// restart-scoped behaviour the ImGui settings dialog warns about. That is
// also why committing the cvar straight from the guest thread is safe: it is
// a string write plus a config save, with nothing reallocating underneath it.
constexpr const char* kResolutionCvar = "resolution";

// Kept in lockstep with settings.cpp's own Resolution row so the native row
// and the ImGui overlay row are the same setting with the same choices.
constexpr std::array<const char*, 4> kResolutionCvarValues = {"720p", "1080p", "1440p", "4K"};
constexpr const char16_t* kResolutionText[] = {u"720p", u"1080p", u"1440p", u"4K"};

// The shared string table has no id for "Resolution", so the label is
// authored here rather than looked up. Title case to match the stock rows
// ("Grafici", "Volume", "Cambia risoluzione....."), not the all-caps prompt
// bar. The value texts are the preset names themselves and stay untranslated.
const char16_t* ResolutionLabel() {
  const char16_t* base = [] {
    switch (CurrentLanguage()) {
      case XLanguageId::kItalian:
        return u"Risoluzione";
      // No kJapanese case: the literal-text widget setter (SetWidgetText,
      // via 0x825D1EE0) writes raw UTF-16 straight into the widget with no
      // font/atlas selection of its own -- glyph rendering is resolved later
      // from whatever's actually baked into the game's glyph atlas, which
      // only covers the shipped string table's own corpus. A phrase we
      // invented (never part of that corpus, e.g. "解像度") has no glyph
      // there and shows as tofu, so this falls back to English instead.
      case XLanguageId::kGerman:
        return u"Auflösung";
      case XLanguageId::kFrench:
        return u"Résolution";
      case XLanguageId::kSpanish:
        return u"Resolución";
      case XLanguageId::kEnglish:
      default:
        return u"Resolution";
    }
  }();
  return AppendRestartMarker(base, CvarPendingRestart(kResolutionCvar));
}

int32_t GetResolutionIndex() {
  const std::string current = rex::cvar::GetFlagByName(kResolutionCvar);
  for (size_t i = 0; i < kResolutionCvarValues.size(); ++i) {
    if (current == kResolutionCvarValues[i]) {
      return static_cast<int32_t>(i);
    }
  }
  return 0;
}

void CommitResolutionIndex(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(kResolutionCvarValues.size())) {
    return;
  }
  const char* value = kResolutionCvarValues[static_cast<size_t>(index)];
  if (rex::cvar::GetFlagByName(kResolutionCvar) == value) {
    return;
  }
  rex::cvar::SetFlagByName(kResolutionCvar, value, /*persist=*/true);
  SaveToUserSettings(kResolutionCvar);
  REXLOG_INFO("[native_options] resolution set to {} (applies on next launch)", value);
}

// --- Fullscreen ---------------------------------------------------------

// `fullscreen` is kHotReload -- Window::SetFullscreen applies live via the
// change callback ReXApp::SetupPresentation registers -- so this row is
// `live`, same as Volume, and needs no "applies on next launch" caveat.
constexpr const char* kFullscreenCvar = "fullscreen";
constexpr std::array<const char*, 2> kFullscreenCvarValues = {"false", "true"};
constexpr const char16_t* kFullscreenText[] = {u"Off", u"On"};

const char16_t* FullscreenLabel() {
  switch (CurrentLanguage()) {
    case XLanguageId::kItalian:
      return u"Schermo intero";
    // No kJapanese case -- see ResolutionLabel's comment on why: an invented
    // phrase has no baked glyph in the game's atlas and renders as tofu.
    case XLanguageId::kGerman:
      return u"Vollbild";
    case XLanguageId::kFrench:
      return u"Plein écran";
    case XLanguageId::kSpanish:
      return u"Pantalla completa";
    case XLanguageId::kEnglish:
    default:
      return u"Fullscreen";
  }
}

int32_t GetFullscreenIndex() {
  return rex::cvar::GetFlagByName(kFullscreenCvar) == "true" ? 1 : 0;
}

void CommitFullscreenIndex(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(kFullscreenCvarValues.size())) {
    return;
  }
  const char* value = kFullscreenCvarValues[static_cast<size_t>(index)];
  if (rex::cvar::GetFlagByName(kFullscreenCvar) == value) {
    return;
  }
  rex::cvar::SetFlagByName(kFullscreenCvar, value, /*persist=*/true);
  SaveToUserSettings(kFullscreenCvar);
}

// --- Language -------------------------------------------------------------

// `user_language` is kRequiresRestart (see xam_user.cpp), same restart-scoped
// behaviour as Resolution -- and it is also the very cvar CurrentLanguage()
// reads to localize every row's label, so changing it here is only reflected
// the next time this screen (or any other native_options row) is painted
// after a relaunch.
constexpr const char* kLanguageCvar = "user_language";
constexpr std::array<const char*, 6> kLanguageCvarValues = {"1", "2", "3", "4", "5", "6"};
constexpr const char16_t* kLanguageText[] = {u"English", u"Japanese", u"German",
                                             u"French",  u"Spanish",  u"Italian"};

const char16_t* LanguageRowLabel() {
  const char16_t* base = [] {
    switch (CurrentLanguage()) {
      case XLanguageId::kItalian:
        return u"Lingua";
      // No kJapanese case -- see ResolutionLabel's comment on why.
      case XLanguageId::kGerman:
        return u"Sprache";
      case XLanguageId::kFrench:
        return u"Langue";
      case XLanguageId::kSpanish:
        return u"Idioma";
      case XLanguageId::kEnglish:
      default:
        return u"Language";
    }
  }();
  return AppendRestartMarker(base, CvarPendingRestart(kLanguageCvar));
}

int32_t GetLanguageIndex() {
  return std::clamp(static_cast<int32_t>(CurrentLanguage()) - 1, int32_t{0},
                     static_cast<int32_t>(kLanguageCvarValues.size()) - 1);
}

void CommitLanguageIndex(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(kLanguageCvarValues.size())) {
    return;
  }
  const char* value = kLanguageCvarValues[static_cast<size_t>(index)];
  if (rex::cvar::GetFlagByName(kLanguageCvar) == value) {
    return;
  }
  rex::cvar::SetFlagByName(kLanguageCvar, value, /*persist=*/true);
  SaveToUserSettings(kLanguageCvar);
  REXLOG_INFO("[native_options] language set to {} (applies on next launch)", value);
}

// --- GPU Backend ------------------------------------------------------------

// Whether the active GPU plugin offers more than one backend to choose from
// (rexgpu-xenos: D3D12 + Vulkan on Windows, Vulkan only on Linux), queried via
// rex::system::QuerySupportedBackends -- same call settings.cpp's
// DrawGpuBackendRow makes, and cached the same way (see
// PrewarmSettingsDialogCaches): loading a plugin DLL just to ask isn't
// something to redo on every screen open. This row's own `available` reads
// the cached result, and NativeOptions::Bind folds it into the *runtime* row
// count the list is actually built with (see g_active_row_count) rather than
// leaving a widget the game created but this file never fills in -- so an
// appended row's `available` returning false is safe here as long as it is
// the last row in the table, which it is.
constexpr const char* kGpuBackendCvar = "gpu_backend";
constexpr std::array<const char*, 2> kGpuBackendCvarValues = {"d3d12", "vulkan"};
constexpr const char16_t* kGpuBackendText[] = {u"D3D12", u"Vulkan"};

// Set once by NativeOptions::Bind, before any row-count decision is made.
bool g_gpu_backend_row_available = false;

bool GpuBackendAvailable() { return g_gpu_backend_row_available; }

const char16_t* GpuBackendLabel() {
  const char16_t* base = [] {
    switch (CurrentLanguage()) {
      case XLanguageId::kItalian:
        return u"Backend GPU";
      // No kJapanese case -- see ResolutionLabel's comment on why.
      case XLanguageId::kGerman:
        return u"GPU-Backend";
      case XLanguageId::kFrench:
        return u"Backend GPU";
      case XLanguageId::kSpanish:
        return u"Backend de GPU";
      case XLanguageId::kEnglish:
      default:
        return u"GPU Backend";
    }
  }();
  return AppendRestartMarker(base, CvarPendingRestart(kGpuBackendCvar));
}

int32_t GetGpuBackendIndex() {
  const std::string current = rex::cvar::GetFlagByName(kGpuBackendCvar);
  for (size_t i = 0; i < kGpuBackendCvarValues.size(); ++i) {
    if (current == kGpuBackendCvarValues[i]) {
      return static_cast<int32_t>(i);
    }
  }
  return 0;
}

void CommitGpuBackendIndex(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(kGpuBackendCvarValues.size())) {
    return;
  }
  const char* value = kGpuBackendCvarValues[static_cast<size_t>(index)];
  if (rex::cvar::GetFlagByName(kGpuBackendCvar) == value) {
    return;
  }
  rex::cvar::SetFlagByName(kGpuBackendCvar, value, /*persist=*/true);
  SaveToUserSettings(kGpuBackendCvar);
  REXLOG_INFO("[native_options] gpu backend set to {} (applies on next launch)", value);
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

bool MasterVolumeAvailable() {
  // False on an SDK build predating the audio_volume cvar -- in which case
  // the stock row keeps working exactly as it did, rather than turning into
  // a dead control stuck at 0.
  return rex::cvar::GetFlagInfo(kAudioVolumeCvar) != nullptr;
}

int32_t GetMasterVolumeStep() {
  const double amplitude = std::atof(rex::cvar::GetFlagByName(kAudioVolumeCvar).c_str());
  const int percent = VolumePercentFromAmplitude(amplitude);
  const int32_t step = static_cast<int32_t>(std::lround(percent * kVolumeSteps / 100.0));
  return std::clamp(step, int32_t{0}, kVolumeSteps);
}

void CommitMasterVolumeStep(int32_t step) {
  step = std::clamp(step, int32_t{0}, kVolumeSteps);
  const int percent = static_cast<int>(std::lround(step * 100.0 / kVolumeSteps));
  const double amplitude = VolumeAmplitudeFromPercent(percent);
  rex::cvar::SetFlagByName(kAudioVolumeCvar, std::to_string(amplitude), /*persist=*/true);
  SaveToUserSettings(kAudioVolumeCvar);
}

// --- Graphics (stock row 0, relabelled) -------------------------------------

// The Italian localisation of the Graphics row reads "Grafici" (the plural of
// "graph"/"chart") where it should read "Grafica". Every other language's
// string is fine, so this returns null for them -- RenderCustomRow skips a
// null label, leaving the game's own text in place, and no translation has to
// be invented here.
//
// Fixing it this way rather than patching the string table means nothing on
// disk changes and the entry stays correct for every other screen that shows
// it (see docs/reverse-engineering.md for the static-text patching route,
// which would be the wrong tool here).
const char16_t* GraphicsLabel() {
  return CurrentLanguage() == XLanguageId::kItalian ? u"Grafica" : nullptr;
}

// --- The table itself -------------------------------------------------------

constexpr uint32_t kStockGraphicsRow = 0;
constexpr uint32_t kStockVolumeRow = 1;
constexpr uint32_t kFullscreenRow = kVanillaRowCount + 1;
constexpr uint32_t kLanguageRow = kVanillaRowCount + 2;
// Kept in the table on every platform; whether it actually shows is decided
// at runtime by g_gpu_backend_row_available (see GpuBackendAvailable and
// NativeOptions::Bind), not by a compile-time platform check.
constexpr uint32_t kGpuBackendRow = kVanillaRowCount + 3;

constexpr CustomRow kCustomRows[] = {
    {
        // Label-only: no get/commit, so the Graphics setting itself is
        // untouched -- value, behaviour and model copy-back all stay stock.
        .list_row = kStockGraphicsRow,
        .label = &GraphicsLabel,
    },
    {
        // Keeps the game's own localized "Volume" label and its 0-10 spinner.
        .list_row = kStockVolumeRow,
        .min = 0,
        .max = kVolumeSteps,
        .get = &GetMasterVolumeStep,
        .commit = &CommitMasterVolumeStep,
        .live = true,
        .pin_stock_to_max = true,
        .available = &MasterVolumeAvailable,
    },
    {
        // `live` here doesn't mean the *effect* is immediate -- resolution is
        // still restart-scoped -- it means the cvar is written on every
        // left/right, same as the ImGui overlay's own dropdown (which has no
        // separate accept step either). That's what lets the label's
        // "(Restart)" marker (see AppendRestartMarker) track the real cvar
        // state live instead of only updating the next time this screen is
        // re-entered.
        .list_row = kVanillaRowCount,
        .label = &ResolutionLabel,
        .values = kResolutionText,
        .value_count = static_cast<uint32_t>(std::size(kResolutionText)),
        .get = &GetResolutionIndex,
        .commit = &CommitResolutionIndex,
        .live = true,
    },
    {
        .list_row = kFullscreenRow,
        .label = &FullscreenLabel,
        .values = kFullscreenText,
        .value_count = static_cast<uint32_t>(std::size(kFullscreenText)),
        .get = &GetFullscreenIndex,
        .commit = &CommitFullscreenIndex,
        .live = true,
    },
    {
        // See the Resolution row above for why this is `live` despite being
        // restart-scoped.
        .list_row = kLanguageRow,
        .label = &LanguageRowLabel,
        .values = kLanguageText,
        .value_count = static_cast<uint32_t>(std::size(kLanguageText)),
        .get = &GetLanguageIndex,
        .commit = &CommitLanguageIndex,
        .live = true,
    },
    {
        // See the Resolution row above for why this is `live` despite being
        // restart-scoped.
        .list_row = kGpuBackendRow,
        .label = &GpuBackendLabel,
        .values = kGpuBackendText,
        .value_count = static_cast<uint32_t>(std::size(kGpuBackendText)),
        .get = &GetGpuBackendIndex,
        .commit = &CommitGpuBackendIndex,
        .live = true,
        .available = &GpuBackendAvailable,
    },
};

constexpr uint32_t kCustomRowCount = static_cast<uint32_t>(std::size(kCustomRows));

consteval uint32_t CountAppendedRows() {
  uint32_t count = 0;
  for (const CustomRow& row : kCustomRows) {
    if (row.list_row >= kVanillaRowCount) {
      ++count;
    }
  }
  return count;
}

constexpr uint32_t kTotalRowCount = kVanillaRowCount + CountAppendedRows();

static_assert(kTotalRowCount <= kListMaxRows,
              "the option list only has room for 16 rows (sub_825D3200)");

consteval bool RowsAreWellFormed() {
  for (uint32_t i = 0; i < kCustomRowCount; ++i) {
    if (kCustomRows[i].list_row >= kTotalRowCount) {
      return false;
    }
    if (kCustomRows[i].get == nullptr) {
      // Label-only: must actually supply a label, and can only ever apply to
      // a stock row (an appended one would have no value behind it at all).
      if (kCustomRows[i].label == nullptr || kCustomRows[i].list_row >= kVanillaRowCount) {
        return false;
      }
    }
    for (uint32_t j = i + 1; j < kCustomRowCount; ++j) {
      if (kCustomRows[i].list_row == kCustomRows[j].list_row) {
        return false;
      }
    }
  }
  return true;
}

static_assert(RowsAreWellFormed(),
              "every kCustomRows entry needs a unique in-range list_row; appended rows must run "
              "contiguously up from kVanillaRowCount and supply a `get`; a row without `get` is "
              "label-only and must supply a `label` and target a stock row");

bool IsEnumRow(const CustomRow& row) { return row.values != nullptr && row.value_count > 0; }

// A row that only replaces a stock row's text, leaving its value alone.
bool IsLabelOnly(const CustomRow& row) { return row.get == nullptr; }

// An override row whose `available` says no is inert: the stock row keeps its
// vanilla behaviour, model copy-back included.
bool IsRowActive(const CustomRow& row) { return row.available == nullptr || row.available(); }

// Index into kCustomRows for a given list row, or -1 if that row is stock (or
// an override that has opted out).
int32_t FindActiveCustomRow(uint32_t list_row) {
  for (uint32_t i = 0; i < kCustomRowCount; ++i) {
    if (kCustomRows[i].list_row == list_row && IsRowActive(kCustomRows[i])) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

// As above, but ignoring label-only rows -- for the paths that care about who
// *owns a value* (model copy-back, cycling) rather than who paints text.
int32_t FindActiveValueRow(uint32_t list_row) {
  const int32_t index = FindActiveCustomRow(list_row);
  return (index >= 0 && IsLabelOnly(kCustomRows[index])) ? -1 : index;
}

int32_t RowMin(const CustomRow& row) { return IsEnumRow(row) ? 0 : row.min; }

int32_t RowMax(const CustomRow& row) {
  return IsEnumRow(row) ? static_cast<int32_t>(row.value_count) - 1 : row.max;
}

int32_t RowStep(const CustomRow& row) { return IsEnumRow(row) ? 1 : row.step; }

int32_t RowMultiplier(const CustomRow& row) { return IsEnumRow(row) ? 1 : row.multiplier; }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

PPCFunc* g_original_list_setup_fn = nullptr;
PPCFunc* g_original_bind_rows_fn = nullptr;
PPCFunc* g_original_cycle_row_fn = nullptr;
PPCFunc* g_original_settings_event_fn = nullptr;
PPCFunc* g_original_settings_activate_fn = nullptr;
PPCFunc* g_set_widget_text_fn = nullptr;
PPCFunc* g_set_text_scale_fn = nullptr;
PPCFunc* g_enable_row_fn = nullptr;
PPCFunc* g_original_list_update_fn = nullptr;
PPCFunc* g_set_widget_colour_fn = nullptr;
PPCFunc* g_alloc_fn = nullptr;
PPCFunc* g_original_post_event_fn = nullptr;

// True from the moment we force our way into the resolution screen with no
// save loaded, until we leave it again. Scopes both the watchdog suppression
// and the latch cleanup below to exactly that window.
bool g_pregame_resolution_screen = false;

// The settings screen's option list, claimed in the list-setup hook. Every
// other hook keys off this so the controls screen's identically-shaped list
// is never touched. Only ever read/written from the guest thread that runs
// the front-end, so no synchronisation is needed.
uint32_t g_list = 0;

// The number of rows the list is actually built with this run -- kTotalRowCount
// (the table's compile-time maximum) minus one if GpuBackendAvailable() came
// back false. Set once by NativeOptions::Bind, before the settings screen can
// possibly open, and used everywhere that needs the *real* row count: how
// many rows ListSetup asks the game to create, how many entries BindRows
// hands over, and the upper bound CycleRow/ListUpdate iterate to. This is
// what lets an appended row (GPU Backend) opt out via `available` without
// leaving a widget behind that nothing ever fills in -- the widget for it is
// simply never created, because it's the last row in the table.
uint32_t g_active_row_count = kTotalRowCount;

// Our extended entry table, plus a scratch buffer for UTF-16 text, in one
// allocation from the game's own heap. Allocated once, on the first bind.
uint32_t g_entries = 0;
uint32_t g_text_scratch = 0;
constexpr uint32_t kTextScratchChars = 64;
constexpr uint32_t kArenaSize = kTotalRowCount * kEntryStride + kTextScratchChars * 2;

// The game's own 3-entry table (model+12). The stock rows of our copy are
// mirrored back into it whenever they change, because the screen's "apply"
// path (sub_8257F9E0 on A) reads the model, not the list -- without the
// copy-back, cycling Graphics or Volume would render correctly and then apply
// nothing.
uint32_t g_vanilla_entries = 0;

// Each custom row's value as of the last bind, so B can undo a live row's
// running changes.
int32_t g_seeded_values[kCustomRowCount] = {};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint32_t Read32BE(const uint8_t* base, uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(base + guest_address);
}

void Write32BE(uint8_t* base, uint32_t guest_address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(base + guest_address, value);
}

void Write16BE(uint8_t* base, uint32_t guest_address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(base + guest_address, value);
}

uint8_t Read8(const uint8_t* base, uint32_t guest_address) { return base[guest_address]; }

void Write8(uint8_t* base, uint32_t guest_address, uint8_t value) { base[guest_address] = value; }

// Address of the "a save is loaded" latch, or 0 before the app object exists.
uint32_t InGameLatchAddress(const uint8_t* base) {
  const uint32_t app_object = Read32BE(base, kAppObjectPtrAddr);
  return app_object ? app_object + kInGameLatchOffset : 0;
}


// Calls a guest function with a scratch register set, restoring the caller's
// context afterwards -- same pattern graphics_settings.cpp uses to reuse the
// live context for its own guest calls.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3, uint32_t r4) {
  if (!fn) {
    return 0;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = r3;
  ctx.r4.u32 = r4;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// The list's normal row text scale, read live rather than hardcoded.
float ListTextScale(const uint8_t* base) {
  if (!g_list) {
    return kFallbackTextScale;
  }
  const float scale = std::bit_cast<float>(Read32BE(base, g_list + kListTextScale));
  return (scale > 0.0f && scale <= 4.0f) ? scale : kFallbackTextScale;
}

// Replaces a widget's text outright, then reasserts the row text scale.
//
// The scale matters because the two text setters are not equivalent. The
// stock rows go through sub_825D1EE8 -> sub_825CFC68, which looks the string
// up in the shared table and copies *that entry's own scale* into the
// widget's +536/+540 alongside the text. Literal text goes through
// sub_825D1EE0 -> sub_825CEE40, which writes only the text and leaves the
// scale at the widget's construction default -- noticeably larger than every
// stock row. So anything setting literal text here has to supply the scale
// itself; sub_825D20C0 is the same setter sub_825D36B8 uses to re-scale the
// highlighted row (it takes the scale in f1, not a GPR).
//
// sub_825D1EE0 copies the string into the widget's own buffer (the same
// copy-in-not-point-at behaviour noted in graphics_settings.cpp), so the
// scratch buffer can be reused immediately.
void SetWidgetText(PPCContext& ctx, uint8_t* base, uint32_t widget, const char16_t* text,
                   bool preserve_scale = false) {
  if (!widget || !text || !g_set_widget_text_fn || !g_text_scratch) {
    return;
  }

  // Relabelling a stock row means the widget already carries the scale
  // sub_825CFC68 copied out of the string table for its original text --
  // which is the scale it should keep. Appended rows have no such history:
  // theirs is the widget's construction default (visibly oversized), so they
  // take the list's normal scale instead.
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

  uint32_t length = 0;
  while (text[length] != u'\0' && length < kTextScratchChars - 1) {
    Write16BE(base, g_text_scratch + length * 2, static_cast<uint16_t>(text[length]));
    ++length;
  }
  Write16BE(base, g_text_scratch + length * 2, 0);
  CallGuest(ctx, base, g_set_widget_text_fn, widget, g_text_scratch);

  if (g_set_text_scale_fn) {
    PPCContext saved = ctx;
    ctx.r3.u32 = widget;
    ctx.f1.f64 = static_cast<double>(scale);
    g_set_text_scale_fn(ctx, base);
    ctx = saved;
  }
}

// Lays a row out as a stock spinner entry. Enumerated rows carry no inline
// value string ids, so the vanilla renderer prints the raw number -- which
// RenderCustomRow then paints over with the row's own text.
void WriteCustomEntry(uint8_t* base, uint32_t custom_index) {
  const CustomRow& row = kCustomRows[custom_index];
  const uint32_t entry = g_entries + row.list_row * kEntryStride;

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
  if (!row.label) {
    Write32BE(base, entry + kEntryLabelId, inherited_label_id);
  }

  const int32_t min = RowMin(row);
  const int32_t max = RowMax(row);
  int32_t value = row.get ? row.get() : min;
  value = std::clamp(value, min, max);

  Write32BE(base, entry + kEntryValue, static_cast<uint32_t>(value));
  Write32BE(base, entry + kEntryMin, static_cast<uint32_t>(min));
  Write32BE(base, entry + kEntryMax, static_cast<uint32_t>(max));
  Write32BE(base, entry + kEntryStep, static_cast<uint32_t>(RowStep(row)));
  Write32BE(base, entry + kEntryMultiplier, static_cast<uint32_t>(RowMultiplier(row)));

  // Remembered so B (cancel) can put a live row's real value back -- see
  // NativeOptions_SettingsEvent.
  g_seeded_values[custom_index] = value;
}

int32_t CustomEntryValue(const uint8_t* base, uint32_t custom_index) {
  if (!g_entries) {
    return 0;
  }
  return static_cast<int32_t>(
      Read32BE(base, g_entries + kCustomRows[custom_index].list_row * kEntryStride + kEntryValue));
}

// Paints a row's label, and its value too when the row is enumerated. Numeric
// rows keep the number the game itself just rendered, and a row with no
// `label` keeps the game's own.
void RenderCustomRow(PPCContext& ctx, uint8_t* base, uint32_t custom_index) {
  if (!g_list) {
    return;
  }
  const CustomRow& row = kCustomRows[custom_index];
  const uint32_t list_row = row.list_row;

  if (row.label) {
    SetWidgetText(ctx, base, Read32BE(base, g_list + kListRowLabels + 4 * list_row), row.label(),
                  /*preserve_scale=*/IsLabelOnly(row));
  }
  if (IsEnumRow(row)) {
    const int32_t value = CustomEntryValue(base, custom_index);
    if (value >= 0 && value < static_cast<int32_t>(row.value_count)) {
      SetWidgetText(ctx, base, Read32BE(base, g_list + kListRowValues + 4 * list_row),
                    row.values[value]);
    }
  }
}

// Mirrors one stock row back into the game's own table. See g_vanilla_entries.
// Rows an active override has taken over are deliberately *not* mirrored:
// leaving the model's original entry alone is what keeps whatever that stock
// row used to drive from following our value around.
void CopyEntryToVanilla(uint8_t* base, uint32_t row) {
  if (!g_vanilla_entries || !g_entries || row >= kVanillaRowCount) {
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

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// Claims the settings screen's option list and grows it by kCustomRowCount.
// Doing it here rather than after the fact means the list creates each extra
// row's label widget, value widget, cyclable flag and layout slot itself,
// exactly like the three it already makes.
extern "C" void NativeOptions_ListSetup(PPCContext& ctx, uint8_t* base) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const bool from_settings_screen =
      lr > kSettingsBuildFnAddr && lr <= kSettingsBuildFnAddr + kSettingsBuildFnSize;
  if (from_settings_screen && ctx.r6.u32 == kVanillaRowCount) {
    g_list = ctx.r3.u32;
    ctx.r6.u32 = g_active_row_count;
  }
  if (g_original_list_setup_fn) {
    g_original_list_setup_fn(ctx, base);
  }
}

// Substitutes our extended table for the game's 3-entry one, then paints the
// extra rows. Runs again on every re-entry to the screen (its activate and
// its B/X paths all re-bind), which is also what re-seeds each row from its
// `get` -- so cancelling out of the screen discards an uncommitted change for
// free, the same way it does for the stock rows.
extern "C" void NativeOptions_BindRows(PPCContext& ctx, uint8_t* base) {
  bool ours = g_list != 0 && ctx.r3.u32 == g_list && ctx.r5.u32 == kVanillaRowCount;

  if (ours) {
    if (!g_entries) {
      const uint32_t arena = CallGuest(ctx, base, g_alloc_fn, kArenaSize, 0);
      if (arena) {
        g_entries = arena;
        g_text_scratch = arena + kTotalRowCount * kEntryStride;
      } else {
        REXLOG_WARN("[native_options] guest allocation failed; no extra rows");
      }
    }
    if (g_entries) {
      g_vanilla_entries = ctx.r4.u32;
      for (uint32_t i = 0; i < kVanillaRowCount * kEntryStride; i += 4) {
        Write32BE(base, g_entries + i, Read32BE(base, g_vanilla_entries + i));
      }
      for (uint32_t i = 0; i < kCustomRowCount; ++i) {
        if (IsRowActive(kCustomRows[i]) && !IsLabelOnly(kCustomRows[i])) {
          WriteCustomEntry(base, i);
        }
      }

      // Pin displaced stock settings to their maximum, in the game's *own*
      // table -- that is the copy its apply path (sub_8257F9E0) and its live
      // on-change apply both read, and the one we otherwise leave frozen at
      // whatever the row held before it was taken over.
      for (uint32_t i = 0; i < kCustomRowCount; ++i) {
        const CustomRow& custom = kCustomRows[i];
        if (!IsRowActive(custom) || !custom.pin_stock_to_max ||
            custom.list_row >= kVanillaRowCount) {
          continue;
        }
        const uint32_t stock = g_vanilla_entries + custom.list_row * kEntryStride;
        Write32BE(base, stock + kEntryValue, Read32BE(base, stock + kEntryMax));
      }

      ctx.r4.u32 = g_entries;
      ctx.r5.u32 = g_active_row_count;
    } else {
      ours = false;
    }
  }

  if (g_original_bind_rows_fn) {
    g_original_bind_rows_fn(ctx, base);
  }

  if (ours) {
    for (uint32_t i = 0; i < kCustomRowCount; ++i) {
      if (IsRowActive(kCustomRows[i])) {
        RenderCustomRow(ctx, base, i);
      }
    }
  }
}

// Left/right on a row. The vanilla path already advances and wraps every row
// correctly (ours are stock spinner entries), so all this adds is repainting
// an enumerated row with its own text instead of the raw number, and keeping
// the stock rows mirrored back into the game's own table.
extern "C" void NativeOptions_CycleRow(PPCContext& ctx, uint8_t* base) {
  const bool ours = g_list != 0 && ctx.r3.u32 == g_list && g_entries != 0;
  const uint32_t row = ours ? Read32BE(base, g_list + kListSelectedRow) : 0;

  if (g_original_cycle_row_fn) {
    g_original_cycle_row_fn(ctx, base);
  }

  if (!ours || row >= g_active_row_count) {
    return;
  }

  const int32_t custom_index = FindActiveValueRow(row);
  if (custom_index < 0) {
    CopyEntryToVanilla(base, row);
    return;
  }

  const CustomRow& custom = kCustomRows[custom_index];
  // Commit before painting: a live row's label may depend on the committed
  // cvar state (see Resolution/Language/GPU Backend's "(Restart)" marker),
  // so the paint has to see this keypress's change, not the previous one.
  if (custom.live && custom.commit) {
    custom.commit(CustomEntryValue(base, static_cast<uint32_t>(custom_index)));
  }
  RenderCustomRow(ctx, base, static_cast<uint32_t>(custom_index));
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
    for (uint32_t row = kStockLabelResetCount; row < g_active_row_count; ++row) {
      const uint32_t label = Read32BE(base, list + kListRowLabels + 4 * row);
      if (label) {
        CallGuest(ctx, base, g_set_widget_colour_fn, label, colour);
      }
    }
  }

  if (g_original_list_update_fn) {
    g_original_list_update_fn(ctx, base);
  }
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
extern "C" void NativeOptions_SettingsActivate(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;

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
    CallGuest(ctx, base, g_enable_row_fn, list, row);
  }
}

// Keeps the pre-game resolution screen from being yanked away the frame after
// it opens.
//
// With no game running, the UI manager's per-frame update (sub_825AAE90) runs
// a watchdog: "game state settled && no page switch pending && not on page 0"
// makes it clear the save-loaded latch and post its own switch back to
// mPrevScreen. The resolution screen trips it every time, because its own
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
  if (ctx.r3.u32 == kEventClassPageSwitch && g_pregame_resolution_screen) {
    const uint32_t manager = Read32BE(base, kUiManagerPtrAddr);
    const uint32_t current_page = manager ? Read32BE(base, manager + kCurrentPageOffset) : 0;
    const uint32_t latch = InGameLatchAddress(base);

    if (static_cast<uint32_t>(ctx.lr) == kWatchdogPostLr && current_page == kResolutionPage) {
      if (latch) {
        Write8(base, latch, 1);
      }
      return;  // swallow it -- deliberately not forwarded to the original
    }

    if (ctx.r5.u32 != kResolutionPage) {
      // Leaving the screen under our own steam (A/B/X). Its Activate raised
      // the latch unconditionally and nothing lowers it again, so without
      // this the front-end would keep believing a save was loaded -- and the
      // settings screen we are returning to would then try to exit to the
      // pause menu instead of the main menu.
      g_pregame_resolution_screen = false;
      if (latch) {
        Write8(base, latch, 0);
      }
    }
  }

  if (g_original_post_event_fn) {
    g_original_post_event_fn(ctx, base);
  }
}

// A commits every custom row. B rolls back the live ones -- they have been
// applying as the player cycled, so cancelling has to undo that; the non-live
// ones need nothing, since the re-bind on the way out reloads them from their
// own `get`. X ("defaults") does the same rollback and repaints.
extern "C" void NativeOptions_SettingsEvent(PPCContext& ctx, uint8_t* base) {
  const uint32_t message = ctx.r4.u32;
  bool reset_rows = false;
  uint32_t forced_latch_address = 0;

  if (message && g_list) {
    const uint32_t message_class = Read32BE(base, message + kMsgClass);
    const uint32_t button = Read32BE(base, message + kMsgButton);

    // Unlock "Change Screen Size..." before a save is loaded.
    //
    // Greying the row out and letting it navigate are two separate gates:
    // NativeOptions_SettingsActivate re-enables the row, but the handler
    // *also* guards the page-17 jump on the same save-loaded latch --
    //
    //   if ( selected_row == 2 && latch ) sub_825CE8E8(8, 0, 17, ...);
    //
    // -- so without this the row highlights, accepts A, misses that branch
    // and falls through to the exit path, dumping the player back to the
    // main menu. Raising the latch just across the original call takes the
    // intended branch, which posts the page switch and returns immediately;
    // nothing else in that call reads the latch afterwards. It goes straight
    // back down so the rest of the front-end still sees the true game state
    // (the screen we are switching to does not read it at all).
    if (message_class == kMsgClassButton && button == kButtonAccept &&
        Read32BE(base, g_list + kListSelectedRow) == kResolutionScreenRow) {
      const uint32_t latch = InGameLatchAddress(base);
      if (latch && Read8(base, latch) == 0) {
        Write8(base, latch, 1);
        forced_latch_address = latch;
      }
    }

    if (message_class == kMsgClassButton && g_entries) {
      if (button == kButtonAccept) {
        for (uint32_t i = 0; i < kCustomRowCount; ++i) {
          if (IsRowActive(kCustomRows[i]) && kCustomRows[i].commit) {
            kCustomRows[i].commit(CustomEntryValue(base, i));
          }
        }
      } else if (button == kButtonCancel) {
        for (uint32_t i = 0; i < kCustomRowCount; ++i) {
          if (IsRowActive(kCustomRows[i]) && kCustomRows[i].live && kCustomRows[i].commit) {
            kCustomRows[i].commit(g_seeded_values[i]);
          }
        }
      } else if (button == kButtonDefaults) {
        reset_rows = true;
      }
    }
  }

  if (g_original_settings_event_fn) {
    g_original_settings_event_fn(ctx, base);
  }

  if (forced_latch_address) {
    Write8(base, forced_latch_address, 0);

    // From here until we leave it again, the resolution screen is open with
    // no save loaded -- a state the front-end watchdog actively tries to undo,
    // and which leaves the latch needing cleanup on the way out. Both are
    // handled in NativeOptions_PostEvent.
    g_pregame_resolution_screen = true;
  }

  if (reset_rows) {
    for (uint32_t i = 0; i < kCustomRowCount; ++i) {
      if (!IsRowActive(kCustomRows[i])) {
        continue;
      }
      if (kCustomRows[i].live && kCustomRows[i].commit) {
        kCustomRows[i].commit(g_seeded_values[i]);
      }
      if (!IsLabelOnly(kCustomRows[i])) {
        WriteCustomEntry(base, i);
      }
      RenderCustomRow(ctx, base, i);
    }
  }
}

}  // namespace

void NativeOptions::Bind(rex::Runtime* runtime) {
  runtime_ = runtime;
  if (!runtime_ || !runtime_->function_dispatcher()) {
    return;
  }
  auto* dispatcher = runtime_->function_dispatcher();

  if (!runtime_->user_data_root().empty()) {
    g_user_settings_path = runtime_->user_data_root() / "settings.toml";
  }

  g_set_widget_text_fn = dispatcher->GetFunction(kSetWidgetTextFnAddr);
  g_set_text_scale_fn = dispatcher->GetFunction(kSetTextScaleFnAddr);
  g_enable_row_fn = dispatcher->GetFunction(kEnableRowFnAddr);
  g_set_widget_colour_fn = dispatcher->GetFunction(kSetWidgetColourFnAddr);
  g_alloc_fn = dispatcher->GetFunction(kAllocFnAddr);
  if (!g_set_widget_text_fn || !g_alloc_fn) {
    REXLOG_WARN("[native_options] set-widget-text/alloc unavailable; no extra rows");
    return;
  }

  // GPU Backend is the last row in the table, so deciding it out here -- once,
  // rather than per screen-open -- and folding it into g_active_row_count
  // before any hook runs is enough to make it disappear cleanly when the
  // active GPU plugin doesn't offer a choice.
  g_gpu_backend_row_available =
      rex::system::QuerySupportedBackends(rex::cvar::GetFlagByName("gpu_plugin")).size() > 1;
  if (!g_gpu_backend_row_available) {
    --g_active_row_count;
  }

  if (!dispatcher->OverrideFunction(kListSetupFnAddr, &NativeOptions_ListSetup,
                                    &g_original_list_setup_fn)) {
    REXLOG_WARN("[native_options] OverrideFunction failed for {:08X} (list setup); the settings "
                "screen will keep its three stock rows",
                kListSetupFnAddr);
    return;
  }
  if (!dispatcher->OverrideFunction(kBindRowsFnAddr, &NativeOptions_BindRows,
                                    &g_original_bind_rows_fn)) {
    REXLOG_WARN("[native_options] OverrideFunction failed for {:08X} (bind rows)", kBindRowsFnAddr);
    return;
  }
  if (!dispatcher->OverrideFunction(kCycleRowFnAddr, &NativeOptions_CycleRow,
                                    &g_original_cycle_row_fn)) {
    REXLOG_WARN("[native_options] OverrideFunction failed for {:08X} (cycle row); the extra rows "
                "will show but not respond to left/right",
                kCycleRowFnAddr);
  }
  if (!dispatcher->OverrideFunction(kListUpdateFnAddr, &NativeOptions_ListUpdate,
                                    &g_original_list_update_fn)) {
    REXLOG_WARN("[native_options] OverrideFunction failed for {:08X} (list update); appended rows "
                "will keep the highlight colour after the cursor leaves them",
                kListUpdateFnAddr);
  }
  if (!dispatcher->OverrideFunction(kSettingsEventFnAddr, &NativeOptions_SettingsEvent,
                                    &g_original_settings_event_fn)) {
    REXLOG_WARN("[native_options] OverrideFunction failed for {:08X} (settings events); the extra "
                "rows will cycle but never commit",
                kSettingsEventFnAddr);
  }
  if (!dispatcher->OverrideFunction(kSettingsActivateFnAddr, &NativeOptions_SettingsActivate,
                                    &g_original_settings_activate_fn)) {
    REXLOG_WARN("[native_options] OverrideFunction failed for {:08X} (settings activate); Volume "
                "and Change Screen Size will stay greyed out until a save is loaded",
                kSettingsActivateFnAddr);
  }

  if (!dispatcher->OverrideFunction(kPostEventFnAddr, &NativeOptions_PostEvent,
                                    &g_original_post_event_fn)) {
    REXLOG_WARN("[native_options] OverrideFunction failed for {:08X} (page switches); opening "
                "Change Screen Size with no save loaded will bounce back to the main menu",
                kPostEventFnAddr);
  }

  REXLOG_INFO("[native_options] hooks installed ({} row(s) in table)", kCustomRowCount);
}

NativeOptions& GetNativeOptions() {
  static NativeOptions instance;
  return instance;
}

}  // namespace nocturne
