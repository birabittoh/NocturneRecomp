// graphics_settings - owns the stretch preset catalog: applies presets, keeps
// the choice across launches, and publishes it to interested mods.
//
// Presets are picked from the settings screen's Preset row (native_options.cpp,
// which replaced the stock "Change Screen Size..." row with it -- see
// RequestGraphicsPreset). The stretch screen that row used to open is still
// reachable from it with Y, for finer control than the presets give, and this
// file's hooks on it all still apply: the unclamped percent-to-rect converter,
// the X-cycles-presets repurposing, and the localized "Presets" relabel of
// that screen's "Default" prompt.
#include "graphics_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/mod_registry.h>
#include <rex/system/mod_storage.h>

namespace nocturne {

namespace {

constexpr uint32_t kStretchXOffset = 560;
constexpr uint32_t kStretchYOffset = 564;

// LTRB offsets from graphics.stretch_rect.
constexpr uint32_t kRectLeftOffset = 0;
constexpr uint32_t kRectTopOffset = 4;
constexpr uint32_t kRectRightOffset = 8;
constexpr uint32_t kRectBottomOffset = 12;

// The title update relocates the whole image, so every guest address below
// needs a patched-image counterpart. Function addresses were re-derived with
// scripts/match_tu_functions.py (matching normalized recompiled bodies
// between a vanilla and a --tu codegen tree; see nocturnerecomp_tu_config.toml
// for the workflow) -- the shift is regional, not a single constant: +0x200
// for everything in this file except kFixedTimestepTickFnAddr (+0x1F8) and
// kAllocFnAddr (+0x1D8), both further from the 0x825B-0x825D core where the
// shift hasn't caught up to +0x200 yet. Each was confirmed by eyeballing the
// matched bodies (identical instruction sequences, just relocated).
//
// kStretchRectAddr/Max/Min and kUiTransitionManagerAddr are plain .data, not
// code -- match_tu_functions.py can't find those directly, but they were
// re-derived by finding the vanilla guest function(s) that reference each
// address (e.g. kStretchPercentToRectFnAddr itself for the rect trio,
// sub_825ABED0/the UI manager init for kUiTransitionManagerAddr), matching
// that function to its TU counterpart, then comparing the load/store offset
// literal at the same body line in both codegen trees (same technique as
// match_tu_functions.py, applied to data instead of function entry points).
// Confirmed live against a running --tu process with
// scripts/re/scan_guest_memory.py (the same live-tracking technique used for
// kAccentColorGuestAddress in accent_color.cpp): kStretchRectAddr/Max/Min read
// back their exact expected content ({0,0,1280,720} max, {232,54,1048,666}
// min), and kUiTransitionManagerAddr reads a live, plausible heap pointer.
//
// kStretchRectAddr/Max/Min are unmoved (same address as vanilla) --
// apparently this part of .data wasn't relocated by the patch.
// kUiTransitionManagerAddr moved by -0x240, the same delta already seen for
// kAccentColorGuestAddress/kPlayerStatsGuestAddress/kRoomsGuestAddress in
// accent_color.cpp/room_presence.cpp. Don't assume that delta generalizes to
// other addresses without re-deriving each one -- night-and-day difference
// between this and kYGlyphNameAddr in native_options.cpp (+0x30) shows deltas
// vary by which specific .data region an address lives in, not just by
// address range.
#ifdef NOCTURNE_TU
constexpr uint32_t kStretchRectAddr = 0x82882C68u;
constexpr uint32_t kStretchRectMaxAddr = 0x82882C98u;
constexpr uint32_t kStretchRectMinAddr = 0x82882CC8u;
constexpr uint32_t kUiTransitionManagerAddr = 0x82E79DECu;
constexpr uint32_t kChangeScreenSizeFnAddr = 0x825BA878u;
constexpr uint32_t kStretchPercentToRectFnAddr = 0x825BB4B0u;
constexpr uint32_t kStretchWidgetRepositionFnAddr = 0x825AB4B0u;
constexpr uint32_t kFixedTimestepTickFnAddr = 0x8258B5B0u;
constexpr uint32_t kSetWidgetTextByIdFnAddr = 0x825CFE68u;
#else
constexpr uint32_t kStretchRectAddr = 0x82882C68u;
constexpr uint32_t kStretchRectMaxAddr = 0x82882C98u;
constexpr uint32_t kStretchRectMinAddr = 0x82882CC8u;
constexpr uint32_t kUiTransitionManagerAddr = 0x82E7A02Cu;
constexpr uint32_t kChangeScreenSizeFnAddr = 0x825BA678u;
constexpr uint32_t kStretchPercentToRectFnAddr = 0x825BB2B0u;
constexpr uint32_t kStretchWidgetRepositionFnAddr = 0x825AB2B0u;
constexpr uint32_t kFixedTimestepTickFnAddr = 0x8258B3B8u;
constexpr uint32_t kSetWidgetTextByIdFnAddr = 0x825CFC68u;
#endif

// The plain text widget the screens use for headings, and the calls that make
// one -- all taken from the settings screen's own builder (sub_825B4650),
// which creates its title exactly this way: allocate, construct with the
// screen as parent (which is what puts it in the draw list), set the text,
// then measure it to centre it and give it a colour.
#ifdef NOCTURNE_TU
constexpr uint32_t kAllocFnAddr = 0x82576B28u;          // (size) -> pointer
constexpr uint32_t kTextWidgetCtorFnAddr = 0x825CEFA8u; // (memory, parent) -> widget
constexpr uint32_t kSetTextFnAddr = 0x825CF040u;        // (widget, utf16)
constexpr uint32_t kTextWidthFnAddr = 0x825CF208u;      // (widget) -> pixels
constexpr uint32_t kSetTextColourFnAddr = 0x825CF200u;  // (widget, argb)
#else
constexpr uint32_t kAllocFnAddr = 0x82576950u;          // (size) -> pointer
constexpr uint32_t kTextWidgetCtorFnAddr = 0x825CEDA8u; // (memory, parent) -> widget
constexpr uint32_t kSetTextFnAddr = 0x825CEE40u;        // (widget, utf16)
constexpr uint32_t kTextWidthFnAddr = 0x825CF008u;      // (widget) -> pixels
constexpr uint32_t kSetTextColourFnAddr = 0x825CF000u;  // (widget, argb)
#endif
constexpr uint32_t kTextWidgetSize = 4668;
constexpr uint32_t kWidgetXOffset = 4;
constexpr uint32_t kWidgetYOffset = 8;

// Opaque black. The channel order is ARGB, pinned down from the settings
// screen's own background shape (sub_825D0390(shape, 1275068415), i.e.
// 0x4BFFFFFF -- a ~29% alpha over white, which only reads correctly with the
// alpha in the high byte).
constexpr uint32_t kPresetLabelColour = 0xFF000000u;

// Where the preset name is drawn, in the front-end's own 640-wide coordinate
// space (its help bar sits at y=423, its titles at y=110).
constexpr int32_t kPresetLabelCentreX = 320;
constexpr int32_t kPresetLabelY = 230;

// Scratch for staging UTF-16 into guest memory, allocated once from the game's
// own heap. The text setter copies out of it immediately.
constexpr uint32_t kTextScratchChars = 64;

// Named after mods_src/graphics_settings' (the separate ImGui-overlay mod in
// NocturneRecomp-Mods) full preset catalog: PSX Default/Big, 16:10 Default/
// Big/Huge/Extreme, Other Stretched. Solved from that mod's own baseline
// pixel constants (kPsxDefaultWidth/Height etc.), which are really RIGHT/
// BOTTOM rect edges (not rendered width/height; see that mod's Apply()
// comment), inverted through this screen's confirmed converter formula:
// right(x) = 232x/50 + 1048, bottom(y) = 54y/30 + 666 (at 720p; the
// converter itself is resolution-independent, driven by the live
// kStretchRectMax/MinAddr bounds below, not these constants).
constexpr uint32_t kPresetCount = 7;
constexpr uint32_t kPresetX[kPresetCount] = {1, 11, 1, 19, 38, 50, 50};
constexpr uint32_t kPresetY[kPresetCount] = {30, 55, 1, 30, 55, 71, 56};
constexpr const char* kPresetNames[kPresetCount] = {
    "PSX Default", "PSX Big", "16:10 Default", "16:10 Big",
    "16:10 Huge",  "16:10 Extreme", "Stretched"};

// Index the row shows for a hand-dialled stretch that matches no preset.
constexpr uint32_t kCustomIndex = kGraphicsCustomPresetIndex;

static_assert(kPresetCount + 1 == kGraphicsPresetCount,
              "kGraphicsPresetTexts (graphics_settings.h) must stay index-aligned with this "
              "catalog plus one for Custom -- the native options Preset row indexes both by the "
              "same value");

constexpr int64_t kToastDurationMs = 2000;

// Message class 11 is the d-pad, with word 2 naming the direction. Confirmed
// from the stretch screen's own handler, which is where the percentages get
// nudged one step at a time.
constexpr uint32_t kMsgClassDpad = 11;
constexpr uint32_t kDpadLeft = 0;
constexpr uint32_t kDpadRight = 1;
constexpr uint32_t kDpadUp = 2;
constexpr uint32_t kDpadDown = 3;

// How far the stretch may be pushed by hand. The vanilla handler clamps to
// [1,50] horizontally and [1,30] vertically -- the point at which the image
// fills the screen -- which is exactly the ceiling being lifted here: past it
// the (unclamped, see GraphicsSettings_PercentToRect) converter extrapolates
// and the image runs off the edges, which is the intent.
//
// A bound still exists, just a far looser one: the percentages feed integer
// arithmetic in the converter and the profile they're saved to, and nothing
// good comes of letting them run away. The floor stays at the vanilla 1 --
// 0 collapses the rect and negatives invert it. The maximum is deliberately
// above the tallest preset (16:10 Extreme, y=71), so every preset stays
// reachable by hand.
constexpr int32_t kStretchPercentMin = 1;
constexpr int32_t kStretchPercentMax = 150;

// The localized-string-id menu.set_widget_text_by_id_fn uses for "DEFAULT"
// -- confirmed live by dumping the whole shared string table (base/count
// globals at 0x82E61854/0x82E61858) and matching on content.
constexpr uint32_t kDefaultStringId = 1;
// Offset of a widget's own copied-text buffer from its own base pointer --
// confirmed via kSetWidgetTextByIdFnAddr's decompile (it copies into
// a1+572, up to 2048 bytes, rather than keeping a pointer into the shared
// string table). This is why editing the shared table's entry at runtime
// has no visible effect on any widget that already called this for that id.
constexpr uint32_t kWidgetTextBufferOffset = 572;
// The stretch screen's own widget-construction function (sub_825BADD0,
// confirmed by its direct reference to the literal string "ScreenSize", its
// background asset name) calls menu.set_widget_text_by_id_fn with id==1
// exactly once, from this return address. Each options-submenu screen
// (Graphics/Volume Level/Change Screen Size/one more) has its own dedicated
// widget-construction call rather than sharing one, so filtering on this
// call site scopes the relabel to just this screen -- every other screen
// showing "Default" keeps saying so.
#ifdef NOCTURNE_TU
constexpr uint32_t kStretchScreenPromptCallSite = 0x825BB324u;
#else
constexpr uint32_t kStretchScreenPromptCallSite = 0x825BB124u;
#endif

// Mirrors rex::system::XLanguage's numeric values (see xcontent.h), but only
// the 6 ids settings.cpp's own kLanguageOptions actually offers in the
// in-game Language dropdown (English/Japanese/German/French/Spanish/
// Italian, ids 1-6. Any language id outside the 6 offered by the dropdown
// falls back to English.
enum class XLanguageId : uint32_t {
  kEnglish = 1,
  kJapanese = 2,
  kGerman = 3,
  kFrench = 4,
  kSpanish = 5,
  kItalian = 6,
};

const char16_t* PresetsLabelForCurrentLanguage() {
  switch (static_cast<XLanguageId>(REXCVAR_QUERY(uint32_t, user_language))) {
    case XLanguageId::kItalian:
      return u"PRESET";
    case XLanguageId::kJapanese:
      return u"プリセット";
    case XLanguageId::kEnglish:
    case XLanguageId::kGerman:
    case XLanguageId::kFrench:
    case XLanguageId::kSpanish:
    default:
      return u"PRESETS";
  }
}

PPCFunc* g_original_stretch_fn = nullptr;
PPCFunc* g_original_perframe_fn = nullptr;
PPCFunc* g_original_percent_to_rect_fn = nullptr;
PPCFunc* g_original_set_widget_text_fn = nullptr;
PPCFunc* g_widget_reposition_fn = nullptr;
rex::system::ModRegistry* g_mod_registry = nullptr;

std::atomic<bool> g_cycle_requested{false};
std::atomic<bool> g_screen_open{false};
std::atomic<uint32_t> g_last_a1{0};
uint32_t g_preset_index = 0;

// "Open" is detected as "the stretch screen's handler has fired at least
// once and hasn't seen a close message yet" (word0==0/word2==5) -- this
// screen's real open message didn't match the (0,0,4,0) pattern assumed
// from static RE, but close (0,0,5,0) is confirmed live and reliable.

// Published to mods_src/graphics_settings (the separate ImGui custom-
// resolution overlay mod in NocturneRecomp-Mods, which also writes
// graphics.stretch_rect -- unconditionally, every frame, to keep a custom
// override alive across menus/launches) whenever a preset is applied here,
// carrying the resulting RIGHT/BOTTOM rect edges (packed into the event's
// u64: high 32 bits = right, low 32 bits = bottom -- the exact same units
// that mod's own Apply()/SetOverride() already use, see its header
// comment on the "(2 * edge - max) " edge convention, so no unit
// conversion is needed on either side).
//
// Earlier versions of this file tried to resolve the conflict by telling
// that mod when to stand aside (an open/close event, then an idle
// watchdog on top of it when the open/close latch proved unreliable
// on its own) -- wrong approach: arbitrating *who* writes still leaves two
// independent pieces of state that can disagree. This publishes the
// actual value instead, so the mod can adopt it as its own override; both
// sides then keep reasserting the same number, so there's nothing left to
// race over. Nothing about this event is specific to that one mod; any mod
// wanting to track the live stretch preset can subscribe the same way.
constexpr const char* kPresetAppliedEvent = "graphics_settings.preset_applied";

// A preset picked from somewhere with no live PPCContext of its own -- the
// native options Preset row's commit, which runs on the guest thread but from
// inside a hook whose context belongs to an unrelated call. -1 means nothing
// pending; GraphicsSettings_PerFrame takes it from here (see
// ApplyRequestedPreset).
std::atomic<int> g_requested_preset{-1};

// The preset-name label drawn over the stretch screen, the screen it was
// built for, and when it should blank itself again. It replaces an ImGui
// overlay toast that used to do the same job from outside the game.
uint32_t g_preset_label = 0;
uint32_t g_preset_label_screen = 0;
bool g_preset_label_shown = false;
std::atomic<int64_t> g_preset_label_deadline_ms{0};

PPCFunc* g_alloc_fn = nullptr;
PPCFunc* g_text_widget_ctor_fn = nullptr;
PPCFunc* g_set_text_fn = nullptr;
PPCFunc* g_text_width_fn = nullptr;
PPCFunc* g_set_text_colour_fn = nullptr;
uint32_t g_text_scratch = 0;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t Read32BE(const uint8_t* base, uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(base + guest_address);
}

void Write32BE(uint8_t* base, uint32_t guest_address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(base + guest_address, value);
}

void Write16BE(uint8_t* base, uint32_t guest_address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(base + guest_address, value);
}

// Calls a guest function with a scratch register set, restoring the caller's
// context afterwards.
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

// Stages ASCII into the guest scratch buffer as UTF-16 and returns its guest
// address, allocating the buffer on first use. The preset names are ASCII by
// construction (see kPresetNames), so this is a widen, not a conversion.
uint32_t StagePresetName(PPCContext& ctx, uint8_t* base, const char* text) {
  if (!g_text_scratch) {
    g_text_scratch = CallGuest(ctx, base, g_alloc_fn, kTextScratchChars * 2, 0);
    if (!g_text_scratch) {
      return 0;
    }
  }
  uint32_t length = 0;
  while (text[length] != '\0' && length < kTextScratchChars - 1) {
    Write16BE(base, g_text_scratch + length * 2, static_cast<uint16_t>(text[length]));
    ++length;
  }
  Write16BE(base, g_text_scratch + length * 2, 0);
  return g_text_scratch;
}

// Builds the label as a child of the stretch screen, once. Parenting it to the
// screen is what gets it drawn and updated; nothing else has to hold it.
uint32_t EnsurePresetLabel(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  if (g_preset_label != 0 && g_preset_label_screen == screen) {
    return g_preset_label;
  }
  if (!g_alloc_fn || !g_text_widget_ctor_fn || !g_set_text_fn) {
    return 0;
  }
  const uint32_t memory = CallGuest(ctx, base, g_alloc_fn, kTextWidgetSize, 0);
  if (!memory) {
    return 0;
  }
  const uint32_t widget = CallGuest(ctx, base, g_text_widget_ctor_fn, memory, screen);
  if (!widget) {
    return 0;
  }
  if (g_set_text_colour_fn) {
    CallGuest(ctx, base, g_set_text_colour_fn, widget, kPresetLabelColour);
  }
  g_preset_label = widget;
  g_preset_label_screen = screen;
  return widget;
}

// Shows `text` centred, or blanks the label when given nullptr -- there is no
// need to hide the widget itself when empty text draws nothing.
void SetPresetLabelText(PPCContext& ctx, uint8_t* base, const char* text) {
  if (!g_preset_label || !g_set_text_fn) {
    return;
  }
  const uint32_t staged = StagePresetName(ctx, base, text ? text : "");
  if (!staged) {
    return;
  }
  CallGuest(ctx, base, g_set_text_fn, g_preset_label, staged);

  const int32_t width = static_cast<int32_t>(CallGuest(ctx, base, g_text_width_fn, g_preset_label, 0));
  Write32BE(base, g_preset_label + kWidgetXOffset,
            static_cast<uint32_t>(kPresetLabelCentreX - width / 2));
  Write32BE(base, g_preset_label + kWidgetYOffset, static_cast<uint32_t>(kPresetLabelY));
  g_preset_label_shown = text != nullptr;
}

void PublishPresetApplied(const uint8_t* base) {
  if (!g_mod_registry) {
    return;
  }
  uint32_t right = Read32BE(base, kStretchRectAddr + kRectRightOffset);
  uint32_t bottom = Read32BE(base, kStretchRectAddr + kRectBottomOffset);
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = (static_cast<uint64_t>(right) << 32) | static_cast<uint64_t>(bottom);
  g_mod_registry->Publish(kPresetAppliedEvent, payload);
}

// Persists the stretch as a fraction of the configured render resolution,
// not raw pixels (the resolution can differ launch to launch, e.g. 720p one
// session, 1080p the next) -- mirrors mods_src/graphics_settings' own
// former persistence exactly (same file, same keys: user_data_root()/
// "mods"/"graphics_settings.cfg", "width_ratio"/"height_ratio"), which this
// now replaces; that mod already read/wrote this same file, so an existing
// player's saved preference carries over with no migration step needed.
std::optional<rex::system::ModStorage> g_storage;

// Set once in Bind() from a persisted ratio, if any; reasserted from every
// GraphicsSettings_PerFrame tick for as long as this stays true. A one-shot
// restore isn't enough -- confirmed live, it wrote successfully (logged) but
// still didn't show up -- and neither was a bounded ~10s reassert window
// (mirroring mods_src/graphics_settings' TryRestoreStyle, which uses the
// same pattern for the graphics-style byte): reasserted correctly and
// continuously for the full 600 ticks with no gaps, right up to the last
// one, and it *still* didn't take. That rules out "something overwrites it
// a few seconds in" -- more likely the boot sequence (company logos, etc.)
// just takes longer than 10s to reach the point where rendering actually
// reads stretch_rect, so the window had already closed by the time it
// mattered. No fixed boot-duration assumption is reliable here, so this
// reasserts unconditionally, indefinitely, exactly like that removed mod's
// own ReassertOverride used to.
//
// The same engine keeps a *player-applied* preset alive too, for a second
// reason: the game recomputes stretch_rect from its own profile percents when
// a save loads. So applying a preset re-points this at the new rect (see
// LatchCurrentRectForReassert) rather than switching it off.
std::atomic<bool> g_restoring{false};
bool g_restore_logged = false;
double g_pending_restore_width_ratio = 0.0;
double g_pending_restore_height_ratio = 0.0;

// The player's own stretch, as a fraction of the render resolution -- the one
// they dialled in on the stretch screen that matched no preset. Kept alongside
// the "current" ratios rather than derived from them, so that picking a preset
// from the Preset row and then cycling back to Custom returns to it instead of
// losing it.
bool g_has_custom = false;
double g_custom_width_ratio = 0.0;
double g_custom_height_ratio = 0.0;

// Which preset a pair of stretch percentages is, or -1 for none -- the test
// that decides whether leaving the stretch screen counts as "still on a
// preset" or "this is now Custom". Percentages are what the presets are
// defined in and what that screen edits, so this is an exact comparison with
// no tolerance to tune.
int32_t PresetIndexForPercent(int32_t x, int32_t y) {
  for (uint32_t i = 0; i < kPresetCount; ++i) {
    if (static_cast<int32_t>(kPresetX[i]) == x && static_cast<int32_t>(kPresetY[i]) == y) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

void SavePresetRatios(const uint8_t* base) {
  if (!g_storage) {
    return;
  }
  uint32_t right = Read32BE(base, kStretchRectAddr + kRectRightOffset);
  uint32_t bottom = Read32BE(base, kStretchRectAddr + kRectBottomOffset);
  uint32_t max_w = Read32BE(base, kStretchRectMaxAddr + kRectRightOffset);
  uint32_t max_h = Read32BE(base, kStretchRectMaxAddr + kRectBottomOffset);
  if (max_w == 0 || max_h == 0) {
    return;
  }
  g_storage->SetDouble("width_ratio", static_cast<double>(right) / max_w);
  g_storage->SetDouble("height_ratio", static_cast<double>(bottom) / max_h);
  // The ratios alone can't be turned back into a preset index (two presets
  // can round to the same rect, and a custom override matches none of them),
  // and the native Preset row has to show *which* preset is active the moment
  // the settings screen opens -- so the index is persisted alongside them.
  g_storage->SetInt("preset_index", static_cast<int64_t>(g_preset_index));
  if (g_has_custom) {
    g_storage->SetDouble("custom_width_ratio", g_custom_width_ratio);
    g_storage->SetDouble("custom_height_ratio", g_custom_height_ratio);
  }
  g_storage->Save();
}

// Points the reassert engine below at whatever is in the live rect right now,
// so it keeps being put back every frame.
//
// Applying a stretch *once* isn't enough to make it stick past the menu: the
// game recomputes stretch_rect from its own profile-stored percents when a
// save loads, so a preset picked from the main menu was silently overwritten
// the moment gameplay started (and only appeared to "take" if it was applied
// again from the pause menu, after that recompute had already happened).
// The boot-time restore already had to solve this -- see g_restoring's comment
// on why nothing bounded works -- so an explicitly applied preset just hands
// itself to the same engine rather than switching it off. Ratios round-trip
// exactly here (they're derived from the very rect that was just written), so
// nothing is lost by going through them.
void LatchCurrentRectForReassert(const uint8_t* base) {
  uint32_t right = Read32BE(base, kStretchRectAddr + kRectRightOffset);
  uint32_t bottom = Read32BE(base, kStretchRectAddr + kRectBottomOffset);
  uint32_t max_w = Read32BE(base, kStretchRectMaxAddr + kRectRightOffset);
  uint32_t max_h = Read32BE(base, kStretchRectMaxAddr + kRectBottomOffset);
  if (max_w == 0 || max_h == 0) {
    return;
  }
  g_pending_restore_width_ratio = static_cast<double>(right) / max_w;
  g_pending_restore_height_ratio = static_cast<double>(bottom) / max_h;
  g_restoring.store(true, std::memory_order_release);
}

// Applies a persisted ratio directly to the live rect (LEFT/TOP derived as
// max-edge, same convention Apply()/the vanilla converter both use) --
// doesn't go through a screen instance's percent fields like
// ApplyPendingCycle does, since none exists yet this early (no player has
// opened the stretch screen), only the always-live stretch_rect/_max
// globals. Called every frame for as long as g_restoring stays true (see
// GraphicsSettings_PerFrame) rather than just once or a few times -- a no-op
// (returns false) if stretch_rect_max reads as 0/0 is harmless and just
// means "retry next frame."
bool ApplyPendingRestore(uint8_t* base) {
  uint32_t max_w = Read32BE(base, kStretchRectMaxAddr + kRectRightOffset);
  uint32_t max_h = Read32BE(base, kStretchRectMaxAddr + kRectBottomOffset);
  if (max_w == 0 || max_h == 0) {
    return false;
  }
  uint32_t right = static_cast<uint32_t>(std::lround(g_pending_restore_width_ratio * max_w));
  uint32_t bottom = static_cast<uint32_t>(std::lround(g_pending_restore_height_ratio * max_h));
  Write32BE(base, kStretchRectAddr + kRectLeftOffset, max_w - right);
  Write32BE(base, kStretchRectAddr + kRectTopOffset, max_h - bottom);
  Write32BE(base, kStretchRectAddr + kRectRightOffset, right);
  Write32BE(base, kStretchRectAddr + kRectBottomOffset, bottom);
  PublishPresetApplied(base);
  if (!g_restore_logged) {
    g_restore_logged = true;
    REXLOG_INFO("[graphics_settings] restoring persisted stretch (right={}, bottom={})", right,
                bottom);
  }
  return true;
}

// The vanilla percent-to-rect converter's arithmetic -- same formula,
// confirmed via its decompile, minus its two [1,50]/[1,30] clamp checks, so
// an out-of-range percent (e.g. >50) correctly extrapolates the rect past the
// native bounds. The stretch screen's own d-pad handler has a *separate* clamp
// on the percent fields, which this doesn't touch -- that one is lifted in
// GraphicsSettings_StretchScreen instead, so hand adjustment can run off the
// screen edges too.
//
// Takes the percents as arguments rather than reading them out of a screen
// instance, so a preset can also be applied with no stretch screen open --
// which is the only way the native options row can apply one (see
// ApplyRequestedPreset).
void WriteStretchRectForPercent(uint8_t* base, int32_t x, int32_t y) {
  int32_t max_left = static_cast<int32_t>(Read32BE(base, kStretchRectMaxAddr + kRectLeftOffset));
  int32_t max_top = static_cast<int32_t>(Read32BE(base, kStretchRectMaxAddr + kRectTopOffset));
  int32_t max_right = static_cast<int32_t>(Read32BE(base, kStretchRectMaxAddr + kRectRightOffset));
  int32_t max_bottom =
      static_cast<int32_t>(Read32BE(base, kStretchRectMaxAddr + kRectBottomOffset));
  int32_t min_left = static_cast<int32_t>(Read32BE(base, kStretchRectMinAddr + kRectLeftOffset));
  int32_t min_top = static_cast<int32_t>(Read32BE(base, kStretchRectMinAddr + kRectTopOffset));
  int32_t min_right = static_cast<int32_t>(Read32BE(base, kStretchRectMinAddr + kRectRightOffset));
  int32_t min_bottom =
      static_cast<int32_t>(Read32BE(base, kStretchRectMinAddr + kRectBottomOffset));

  int32_t left = (max_left - min_left) * x / 50 + min_left;
  int32_t right = (max_right - min_right) * x / 50 + min_right;
  int32_t top = (max_top - min_top) * y / 30 + min_top;
  int32_t bottom = (max_bottom - min_bottom) * y / 30 + min_bottom;

  Write32BE(base, kStretchRectAddr + kRectLeftOffset, static_cast<uint32_t>(left));
  Write32BE(base, kStretchRectAddr + kRectTopOffset, static_cast<uint32_t>(top));
  Write32BE(base, kStretchRectAddr + kRectRightOffset, static_cast<uint32_t>(right));
  Write32BE(base, kStretchRectAddr + kRectBottomOffset, static_cast<uint32_t>(bottom));
}

// Repositions the front-end widgets to match a just-applied stretch, exactly
// as the vanilla converter's own tail call does. Clobbers r3-r5, so callers
// that need their context back save and restore it themselves.
void RepositionWidgets(PPCContext& ctx, uint8_t* base, int32_t x, int32_t y) {
  if (!g_widget_reposition_fn) {
    return;
  }
  ctx.r3.u32 = Read32BE(base, kUiTransitionManagerAddr);
  ctx.r4.u32 = static_cast<uint32_t>(x);
  ctx.r5.u32 = static_cast<uint32_t>(y);
  g_widget_reposition_fn(ctx, base);
}

// Full replacement for the vanilla converter: reads the screen instance's own
// percent fields, then does exactly what the original did with them.
extern "C" void GraphicsSettings_PercentToRect(PPCContext& ctx, uint8_t* base) {
  uint32_t a1 = static_cast<uint32_t>(ctx.r3.u32);
  int32_t x = static_cast<int32_t>(Read32BE(base, a1 + kStretchXOffset));
  int32_t y = static_cast<int32_t>(Read32BE(base, a1 + kStretchYOffset));

  WriteStretchRectForPercent(base, x, y);
  RepositionWidgets(ctx, base, x, y);
}

// Applies the next preset using whatever live ctx/base this call was given
// -- shared by both hooks below, since either might notice a pending cycle
// request first.
void ApplyPendingCycle(PPCContext& ctx, uint8_t* base) {
  uint32_t a1 = g_last_a1.load(std::memory_order_acquire);
  if (a1 == 0) {
    return;
  }

  g_preset_index = (g_preset_index + 1) % kPresetCount;
  uint32_t target_x = kPresetX[g_preset_index];
  uint32_t target_y = kPresetY[g_preset_index];

  Write32BE(base, a1 + kStretchXOffset, target_x);
  Write32BE(base, a1 + kStretchYOffset, target_y);

  ctx.r3.u32 = a1;
  GraphicsSettings_PercentToRect(ctx, base);

  // An explicit player-driven cycle takes the reassert engine over -- both so
  // it isn't overwritten by the next restore tick, and so it survives the
  // game's own recompute on save load (see LatchCurrentRectForReassert).
  LatchCurrentRectForReassert(base);

  PublishPresetApplied(base);
  SavePresetRatios(base);

  if (EnsurePresetLabel(ctx, base, a1)) {
    SetPresetLabelText(ctx, base, kPresetNames[g_preset_index]);
    g_preset_label_deadline_ms.store(NowMs() + kToastDurationMs, std::memory_order_release);
  }
}

// Applies a preset queued by RequestGraphicsPreset. Unlike ApplyPendingCycle
// this doesn't go through a screen instance's percent fields -- there needn't
// be one, since the request comes from the settings screen's own Preset row --
// so it writes the rect directly, the same way ApplyPendingRestore does. If
// the stretch screen *has* been visited this session its percent fields are
// updated too, so re-opening it doesn't show stale numbers.
void ApplyRequestedPreset(PPCContext& ctx, uint8_t* base) {
  const int index = g_requested_preset.exchange(-1, std::memory_order_acq_rel);
  if (index < 0 || index >= static_cast<int>(kGraphicsPresetCount)) {
    return;
  }

  // Custom isn't a percentage pair, so it goes back through the ratio path --
  // the same one the boot-time restore uses, since a hand-dialled stretch is
  // only ever recorded as a ratio.
  if (index == static_cast<int>(kCustomIndex)) {
    if (!g_has_custom) {
      return;
    }
    g_preset_index = kCustomIndex;
    g_pending_restore_width_ratio = g_custom_width_ratio;
    g_pending_restore_height_ratio = g_custom_height_ratio;
    if (ApplyPendingRestore(base)) {
      g_restoring.store(true, std::memory_order_release);
      SavePresetRatios(base);
    }
    return;
  }

  g_preset_index = static_cast<uint32_t>(index);
  const int32_t x = static_cast<int32_t>(kPresetX[index]);
  const int32_t y = static_cast<int32_t>(kPresetY[index]);

  const uint32_t a1 = g_last_a1.load(std::memory_order_acquire);
  if (a1 != 0) {
    Write32BE(base, a1 + kStretchXOffset, static_cast<uint32_t>(x));
    Write32BE(base, a1 + kStretchYOffset, static_cast<uint32_t>(y));
  }

  WriteStretchRectForPercent(base, x, y);
  PPCContext saved = ctx;
  RepositionWidgets(ctx, base, x, y);
  ctx = saved;

  // Takes over the reassert engine rather than switching it off: the pick has
  // to survive the game's own recompute when a save loads.
  LatchCurrentRectForReassert(base);

  PublishPresetApplied(base);
  SavePresetRatios(base);
}

// Hooks the stretch screen itself: tracks the live a1 and open/close state,
// and repurposes X (msg2==6) to cycle instead of resetting to the graphics
// style's default.
extern "C" void GraphicsSettings_StretchScreen(PPCContext& ctx, uint8_t* base) {
  uint32_t a1 = static_cast<uint32_t>(ctx.r3.u32);
  uint32_t a2 = static_cast<uint32_t>(ctx.r4.u32);

  if (a1 != 0) {
    g_last_a1.store(a1, std::memory_order_release);
    g_screen_open.store(true, std::memory_order_release);
  }

  uint32_t msg0 = Read32BE(base, a2 + 0);
  uint32_t msg2 = Read32BE(base, a2 + 8);

  // A (accept) and B (cancel) both leave the screen. Tracking *both* matters
  // now that "open" also suspends the reassert engine: treating only B as the
  // exit would leave it suspended for the rest of the session after an A.
  const bool leaving = msg0 == 0 && (msg2 == 4 || msg2 == 5);

  // A d-pad step, re-applied after the original with our own wider bounds.
  //
  // The vanilla handler does `percent += 1` and clamps, so it can't simply be
  // let through: once the value is sitting on 50 (or 30) every further press
  // is a no-op. Recomputing the step from the value we saw going in, and
  // writing it back afterwards, overrides that clamp without having to
  // reimplement the rest of what the handler does on a d-pad press (the
  // bump feedback, the base-class handling).
  int32_t dpad_dx = 0;
  int32_t dpad_dy = 0;
  if (msg0 == kMsgClassDpad && a1 != 0) {
    switch (msg2) {
      case kDpadLeft: dpad_dx = -1; break;
      case kDpadRight: dpad_dx = 1; break;
      case kDpadUp: dpad_dy = 1; break;
      case kDpadDown: dpad_dy = -1; break;
      default: break;
    }
  }
  const int32_t percent_x_before =
      dpad_dx != 0 ? static_cast<int32_t>(Read32BE(base, a1 + kStretchXOffset)) : 0;
  const int32_t percent_y_before =
      dpad_dy != 0 ? static_cast<int32_t>(Read32BE(base, a1 + kStretchYOffset)) : 0;

  bool suppress_default_reset = false;
  if (msg0 == 0 && msg2 == 6) {
    g_cycle_requested.store(true, std::memory_order_release);
    Write32BE(base, a2 + 8, 0xFFFFFFFFu);
    suppress_default_reset = true;
  }

  if (a1 != 0 && g_cycle_requested.exchange(false)) {
    PPCContext saved = ctx;
    ApplyPendingCycle(ctx, base);
    ctx = saved;
  }

  if (g_original_stretch_fn) {
    g_original_stretch_fn(ctx, base);
  }

  if (suppress_default_reset) {
    Write32BE(base, a2 + 8, 6);
  }

  if (dpad_dx != 0 || dpad_dy != 0) {
    const int32_t x = dpad_dx != 0
                          ? std::clamp(percent_x_before + dpad_dx, kStretchPercentMin,
                                       kStretchPercentMax)
                          : static_cast<int32_t>(Read32BE(base, a1 + kStretchXOffset));
    const int32_t y = dpad_dy != 0
                          ? std::clamp(percent_y_before + dpad_dy, kStretchPercentMin,
                                       kStretchPercentMax)
                          : static_cast<int32_t>(Read32BE(base, a1 + kStretchYOffset));
    Write32BE(base, a1 + kStretchXOffset, static_cast<uint32_t>(x));
    Write32BE(base, a1 + kStretchYOffset, static_cast<uint32_t>(y));

    // The handler recomputed the rect from the value it clamped, so redo it
    // from ours.
    PPCContext saved = ctx;
    ctx.r3.u32 = a1;
    GraphicsSettings_PercentToRect(ctx, base);
    ctx = saved;
  }

  // Adopt whatever the player settled on -- the accepted value on A, or the
  // one the vanilla handler just restored from the profile on B. Latching
  // after the original has run is what makes that distinction free.
  if (leaving) {
    g_screen_open.store(false, std::memory_order_release);
    if (g_preset_label_shown) {
      SetPresetLabelText(ctx, base, nullptr);
    }
    LatchCurrentRectForReassert(base);

    // Classify what the player left with. Anything that isn't one of the
    // presets becomes (or replaces) Custom, which is what puts that entry in
    // the Preset row's list -- so adjusting a preset by hand shows up there as
    // its own choice rather than silently leaving the row claiming the preset
    // it no longer matches.
    const uint32_t screen = g_last_a1.load(std::memory_order_acquire);
    if (screen != 0) {
      const int32_t x = static_cast<int32_t>(Read32BE(base, screen + kStretchXOffset));
      const int32_t y = static_cast<int32_t>(Read32BE(base, screen + kStretchYOffset));
      const int32_t matched = PresetIndexForPercent(x, y);
      if (matched >= 0) {
        g_preset_index = static_cast<uint32_t>(matched);
      } else {
        g_preset_index = kCustomIndex;
        g_has_custom = true;
        g_custom_width_ratio = g_pending_restore_width_ratio;
        g_custom_height_ratio = g_pending_restore_height_ratio;
      }
    }

    SavePresetRatios(base);
  }
}

// Hooks a guaranteed-every-frame guest function purely to get a live, valid
// PPCContext without waiting for the player to press a button.
extern "C" void GraphicsSettings_PerFrame(PPCContext& ctx, uint8_t* base) {
  // Suspended while the stretch screen is open: that screen writes the rect
  // live from its own percent fields as the player holds a direction, and a
  // reassert every frame would put the previous value straight back -- which
  // looks exactly like the d-pad doing nothing. The screen re-latches whatever
  // the player settles on when it closes (see GraphicsSettings_StretchScreen).
  if (g_restoring.load(std::memory_order_acquire) &&
      !g_screen_open.load(std::memory_order_acquire)) {
    ApplyPendingRestore(base);
  }

  ApplyRequestedPreset(ctx, base);

  // Blank the preset name once its moment is up. Doing it here rather than in
  // the screen's own handler is what makes it time out at all: that handler
  // only runs when a message arrives, so a player who presses X and then lets
  // go of the pad would keep the name on screen indefinitely.
  if (g_preset_label_shown && NowMs() >= g_preset_label_deadline_ms.load(std::memory_order_acquire)) {
    SetPresetLabelText(ctx, base, nullptr);
  }

  if (g_screen_open.load(std::memory_order_acquire) && g_cycle_requested.exchange(false)) {
    PPCContext saved = ctx;
    ApplyPendingCycle(ctx, base);
    ctx = saved;
  }

  if (g_original_perframe_fn) {
    g_original_perframe_fn(ctx, base);
  }
}

// Wraps menu.set_widget_text_by_id_fn: calls through to the vanilla
// copy-string-by-id-into-widget-buffer behavior first, then, if the id was
// kDefaultStringId ("DEFAULT") AND the call came from the stretch screen's
// own constructor, overwrites the widget's own just-copied buffer with a
// language-appropriate "PRESETS"/"PRESET"/プリセット label. All are shorter
// than or equal to "DEFAULT"'s own length, so this can never overrun the
// widget's 2048-byte buffer or need to touch any length field -- just
// writes its own NUL terminator.
extern "C" void GraphicsSettings_SetWidgetText(PPCContext& ctx, uint8_t* base) {
  uint32_t a1 = static_cast<uint32_t>(ctx.r3.u32);
  uint32_t a2 = static_cast<uint32_t>(ctx.r4.u32);
  uint32_t lr = static_cast<uint32_t>(ctx.lr);

  if (g_original_set_widget_text_fn) {
    g_original_set_widget_text_fn(ctx, base);
  }

  if (a1 != 0 && a2 == kDefaultStringId && lr == kStretchScreenPromptCallSite) {
    uint32_t text_addr = a1 + kWidgetTextBufferOffset;
    const char16_t* label = PresetsLabelForCurrentLanguage();
    size_t len = std::char_traits<char16_t>::length(label) + 1;  // include NUL
    for (size_t i = 0; i < len; ++i) {
      Write16BE(base, text_addr + static_cast<uint32_t>(i * 2), static_cast<uint16_t>(label[i]));
    }
  }
}

}  // namespace

const char16_t* const kGraphicsPresetTexts[kGraphicsPresetCount] = {
    u"PSX Default", u"PSX Big",       u"16:10 Default", u"16:10 Big",
    u"16:10 Huge",  u"16:10 Extreme", u"Stretched",     u"Custom"};

uint32_t GraphicsPresetChoiceCount() {
  return g_has_custom ? kGraphicsPresetCount : kPresetCount;
}

int32_t GetGraphicsPresetIndex() { return static_cast<int32_t>(g_preset_index); }

void RequestGraphicsPreset(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(GraphicsPresetChoiceCount())) {
    return;
  }
  g_requested_preset.store(index, std::memory_order_release);
}

GraphicsSettings::GraphicsSettings() = default;
GraphicsSettings::~GraphicsSettings() = default;

void GraphicsSettings::Bind(rex::Runtime* runtime) {
  runtime_ = runtime;
  if (!runtime_ || !runtime_->function_dispatcher()) {
    return;
  }
  auto* dispatcher = runtime_->function_dispatcher();
  g_mod_registry = runtime_->mod_registry();

  g_storage.emplace(runtime_->user_data_root() / "mods" / "graphics_settings.cfg");
  g_storage->Load();
  auto custom_width = g_storage->GetDouble("custom_width_ratio");
  auto custom_height = g_storage->GetDouble("custom_height_ratio");
  if (custom_width && custom_height && *custom_width > 0.0 && *custom_height > 0.0) {
    g_custom_width_ratio = *custom_width;
    g_custom_height_ratio = *custom_height;
    g_has_custom = true;
  }
  // Loaded after the custom ratios: a saved index of kCustomIndex is only
  // valid if the custom stretch it refers to came back too.
  if (auto preset_index = g_storage->GetInt("preset_index");
      preset_index && *preset_index >= 0 &&
      *preset_index < static_cast<int64_t>(GraphicsPresetChoiceCount())) {
    g_preset_index = static_cast<uint32_t>(*preset_index);
  }
  auto width_ratio = g_storage->GetDouble("width_ratio");
  auto height_ratio = g_storage->GetDouble("height_ratio");
  if (width_ratio && height_ratio && *width_ratio > 0.0 && *height_ratio > 0.0) {
    g_pending_restore_width_ratio = *width_ratio;
    g_pending_restore_height_ratio = *height_ratio;
    g_restoring.store(true, std::memory_order_release);
  }

  if (!dispatcher->OverrideFunction(kStretchPercentToRectFnAddr,
                                    &GraphicsSettings_PercentToRect,
                                    &g_original_percent_to_rect_fn)) {
    REXLOG_WARN("[graphics_settings] OverrideFunction failed for {:08X} (percent-to-rect); "
                "presets will stay clamped to the native range",
                kStretchPercentToRectFnAddr);
  }
  g_widget_reposition_fn = dispatcher->GetFunction(kStretchWidgetRepositionFnAddr);
  g_alloc_fn = dispatcher->GetFunction(kAllocFnAddr);
  g_text_widget_ctor_fn = dispatcher->GetFunction(kTextWidgetCtorFnAddr);
  g_set_text_fn = dispatcher->GetFunction(kSetTextFnAddr);
  g_text_width_fn = dispatcher->GetFunction(kTextWidthFnAddr);
  g_set_text_colour_fn = dispatcher->GetFunction(kSetTextColourFnAddr);

  if (!dispatcher->OverrideFunction(kChangeScreenSizeFnAddr, &GraphicsSettings_StretchScreen,
                                    &g_original_stretch_fn)) {
    REXLOG_WARN("[graphics_settings] OverrideFunction failed for {:08X} (stretch screen)",
                kChangeScreenSizeFnAddr);
  }

  if (!dispatcher->OverrideFunction(kFixedTimestepTickFnAddr, &GraphicsSettings_PerFrame,
                                    &g_original_perframe_fn)) {
    REXLOG_WARN("[graphics_settings] OverrideFunction failed for {:08X} (per-frame); cycling "
                "will still work but only on the next real button press",
                kFixedTimestepTickFnAddr);
  }

  if (!dispatcher->OverrideFunction(kSetWidgetTextByIdFnAddr, &GraphicsSettings_SetWidgetText,
                                    &g_original_set_widget_text_fn)) {
    REXLOG_WARN("[graphics_settings] OverrideFunction failed for {:08X} (set-widget-text); the "
                "X prompt will keep saying \"Default\"",
                kSetWidgetTextByIdFnAddr);
  }

  REXLOG_INFO("[graphics_settings] hooks installed");
}

GraphicsSettings& GetGraphicsSettings() {
  static GraphicsSettings instance;
  return instance;
}

}  // namespace nocturne
