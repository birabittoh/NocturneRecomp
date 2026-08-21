// nocturnerecomp - open the game's own CScreenAchievement (front-end page 15)
// from the Achievements menu entries. See achievements_screen.h for how that
// screen was identified and what it draws.
//
// Navigation:
//   * Achievements (guest A, either menu) -> post a page switch to page 15 with
//     arg1 = 0, which opens the screen with the stock image widget blank.
//   * B -> post a page switch back to whatever page we came from.
//
// The list itself is built onto that otherwise-empty screen out of the same
// widgets the game's own screens use -- see BuildList. Names, descriptions and
// unlock state come from the SDK's AchievementManager. The icons are looked up
// in the guest's own UI image bank under the ACHIEVEMENT_<n> names this screen
// already asks for -- but the retail banks carry no achievement art at all, so
// achievement_icons.cpp publishes it there first.

#include "achievements_screen.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "achievement_icons.h"
#include "native_options.h"
#include "settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

namespace nocturne {

namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------
//
// Vanilla addresses come from the pinned build's default.xex:
//   * kScreenEventFnAddr is CScreenAchievement's vtable slot 1 (off_820140A8+4),
//     the same slot the settings screen's own event handler occupies in its
//     vtable -- see native_options.cpp, which documents that layout.
//   * kUiManagerPtrAddr / kAppObjectPtrAddr / kPostEventFnAddr are the same
//     three native_options.cpp uses, repeated here rather than shared so each
//     file stays independently greppable against the image.
//
// The TU (title update) values are the vanilla ones plus the regional shift
// native_options.cpp documents for this address range (+0x200 for 0x825Bxxxx
// code, -0x240 for the .data pointers). They have NOT been verified against a
// patched-image codegen the way native_options.cpp's were -- if the achievements
// screen misbehaves under --tu, re-derive kScreenEventFnAddr with
// scripts/match_tu_functions.py before assuming anything else is wrong.
// The widget-building addresses below are the same vocabulary the settings
// screen's own builder (sub_825B4650) uses, read straight off it:
//   * an image widget is alloc(612) + sub_825D0C48(mem, parent, x, y, image);
//     sub_825D02A8 sets its rect (position + size, so it doubles as the scaler),
//     sub_825D0390 sets its four corner tints, sub_825D0320 swaps the image and
//     sub_825D0300 re-reads the image's own width/height back into the widget.
//     An image widget with a null image draws a solid rect of its tint, which is
//     how the stock screens do translucent backing panels.
//   * a text widget is alloc(4668) + sub_825CEDA8(mem, parent); sub_825CEE40
//     sets literal UTF-16 text, sub_825CEFA0 the scale, sub_825CF000 the colour
//     and sub_825CF008 measures it. (sub_825CFC68 is the by-string-id setter the
//     stock screens use instead -- see the note on localization in BuildList.)
//   * a prompt is alloc(552) + sub_825D1DB0(mem, parent, 0), the same recipe
//     native_options.cpp documents for the settings screen's prompt bar.
// Both widget kinds take their position from +4/+8 directly.
#ifdef NOCTURNE_TU
constexpr uint32_t kScreenEventFnAddr = 0x825B70D0u;     // (screen, message)
constexpr uint32_t kScreenActivateFnAddr = 0x825B7200u;  // (screen, arg1)
constexpr uint32_t kPostEventFnAddr = 0x825CEAE8u;       // (class, arg1, page, controller)
constexpr uint32_t kAllocFnAddr = 0x82576B28u;           // (size) -> pointer
constexpr uint32_t kImageCtorFnAddr = 0x825D0E48u;       // (mem, parent, x, y, image)
constexpr uint32_t kImageSetRectFnAddr = 0x825D04A8u;    // (image, x0, y0, x1, y1)
constexpr uint32_t kImageSetTintFnAddr = 0x825D0590u;    // (image, argb)
constexpr uint32_t kTextCtorFnAddr = 0x825CEFA8u;        // (mem, parent) -> text widget
constexpr uint32_t kTextSetTextFnAddr = 0x825CF040u;     // (text, utf16_text)
constexpr uint32_t kTextSetScaleFnAddr = 0x825CF1A0u;    // (text, scale) -- scale in f1
constexpr uint32_t kTextSetColourFnAddr = 0x825CF200u;   // (text, argb)
constexpr uint32_t kFindImageFnAddr = 0x825CED68u;       // (image_bank, name) -> image
constexpr uint32_t kPromptCtorFnAddr = 0x825D1FB0u;      // (memory, parent, flag) -> prompt
constexpr uint32_t kPromptSetGlyphFnAddr = 0x825D20F8u;  // (prompt, image)
constexpr uint32_t kPromptShowGlyphFnAddr = 0x825D2210u; // (prompt)
constexpr uint32_t kPromptSetTextByIdFnAddr = 0x825D20E8u;  // (prompt, string_id)
constexpr uint32_t kPromptTextOffsetFnAddr = 0x825D2218u;   // (prompt, dx, dy)
constexpr uint32_t kPromptSetColourFnAddr = 0x825D2338u;    // (prompt, argb)
constexpr uint32_t kPromptSetPosFnAddr = 0x825D21A8u;       // (prompt, x, y)
constexpr uint32_t kTextWidthFnAddr = 0x825CF208u;          // (text_widget) -> pixels
constexpr uint32_t kTextSetTextByIdFnAddr = 0x825CFE68u;    // (text_widget, string_id)
constexpr uint32_t kUiManagerPtrAddr = 0x82E79DECu;
constexpr uint32_t kAppObjectPtrAddr = 0x82E4F5C8u;
constexpr uint32_t kImageBankPtrAddr = 0x82E7A330u;
constexpr uint32_t kBGlyphNameAddr = 0x82202300u;
#else
constexpr uint32_t kScreenEventFnAddr = 0x825B6ED0u;     // (screen, message)
constexpr uint32_t kScreenActivateFnAddr = 0x825B7000u;  // (screen, arg1)
constexpr uint32_t kPostEventFnAddr = 0x825CE8E8u;       // (class, arg1, page, controller)
constexpr uint32_t kAllocFnAddr = 0x82576950u;           // (size) -> pointer
constexpr uint32_t kImageCtorFnAddr = 0x825D0C48u;       // (mem, parent, x, y, image)
constexpr uint32_t kImageSetRectFnAddr = 0x825D02A8u;    // (image, x0, y0, x1, y1)
constexpr uint32_t kImageSetTintFnAddr = 0x825D0390u;    // (image, argb)
constexpr uint32_t kTextCtorFnAddr = 0x825CEDA8u;        // (mem, parent) -> text widget
constexpr uint32_t kTextSetTextFnAddr = 0x825CEE40u;     // (text, utf16_text)
constexpr uint32_t kTextSetScaleFnAddr = 0x825CEFA0u;    // (text, scale) -- scale in f1
constexpr uint32_t kTextSetColourFnAddr = 0x825CF000u;   // (text, argb)
constexpr uint32_t kFindImageFnAddr = 0x825CEB68u;       // (image_bank, name) -> image
constexpr uint32_t kPromptCtorFnAddr = 0x825D1DB0u;      // (memory, parent, flag) -> prompt
constexpr uint32_t kPromptSetGlyphFnAddr = 0x825D1EF8u;  // (prompt, image)
constexpr uint32_t kPromptShowGlyphFnAddr = 0x825D2010u; // (prompt)
constexpr uint32_t kPromptSetTextByIdFnAddr = 0x825D1EE8u;  // (prompt, string_id)
constexpr uint32_t kPromptTextOffsetFnAddr = 0x825D2018u;   // (prompt, dx, dy)
constexpr uint32_t kPromptSetColourFnAddr = 0x825D2138u;    // (prompt, argb)
constexpr uint32_t kPromptSetPosFnAddr = 0x825D1FA8u;       // (prompt, x, y)
constexpr uint32_t kTextWidthFnAddr = 0x825CF008u;          // (text_widget) -> pixels
constexpr uint32_t kTextSetTextByIdFnAddr = 0x825CFC68u;    // (text_widget, string_id)
constexpr uint32_t kUiManagerPtrAddr = 0x82E7A02Cu;
constexpr uint32_t kAppObjectPtrAddr = 0x82E4F808u;
constexpr uint32_t kImageBankPtrAddr = 0x82E7A570u;
// The glyph names are single-character strings in a descending-letter table
// ("Z" at 0x82202270, 4 bytes per entry), so "B" is 24 entries in. The settings
// screen's own B prompt uses this exact pointer.
constexpr uint32_t kBGlyphNameAddr = 0x822022D0u;
#endif

// Allocation sizes, taken from the stock builders' own alloc calls.
constexpr uint32_t kImageWidgetSize = 612;
constexpr uint32_t kTextWidgetSize = 4668;
constexpr uint32_t kPromptSize = 552;

// Widget field offsets. Position is common to both kinds; the image's width and
// height are what sub_825D0300 copies out of the image it points at.
constexpr uint32_t kWidgetXOffset = 4;
constexpr uint32_t kWidgetYOffset = 8;
constexpr uint32_t kImageWidthOffset = 536;
constexpr uint32_t kImageHeightOffset = 540;

// Where the Back prompt sits -- along the bottom, clear of the last row (the
// twelfth is at y = 84 + 11*27 = 381). 423 is the same baseline the stock
// screens give their own prompt bars. Its x is measured, not fixed, so it comes
// out centred; see CreateBackPrompt.
constexpr uint32_t kPromptBarY = 423;

// A prompt's width is its glyph plus its text, the same sum the stock prompt-bar
// layout uses to space three of them across the bar.
constexpr uint32_t kPromptGlyphOffset = 544;
constexpr uint32_t kPromptTextOffset = 548;
constexpr uint32_t kPromptGlyphWidthOffset = 536;
constexpr uint32_t kPromptTextDx = 30;
constexpr uint32_t kPromptTextDy = 5;
// String id 3 is "Back", the same id the settings screen gives its own B prompt.
constexpr uint32_t kPromptBackStringId = 3;

// ---------------------------------------------------------------------------
// Layout (the front end is laid out in a fixed 640x480 space)
// ---------------------------------------------------------------------------

constexpr uint32_t kSafeAreaWidth = 640;
constexpr uint32_t kScreenHeight = 480;

// The 640-wide logical space is only the 4:3 safe area -- the front end actually
// draws wider than that, which is why the stock screens put their full-screen
// backdrops (BGshapeLGHelp, new_HelpBar) at x = -106 rather than 0. Spanning
// -106..746 is what the game itself treats as edge to edge.
constexpr int32_t kFullBleedLeft = -106;
constexpr int32_t kFullBleedRight = 746;

// A near-opaque black panel behind everything, so the list stays readable over
// whatever it was opened on top of -- live gameplay, from the pause menu. Uses
// the same null-image-plus-tint trick the stock screens use for backing panels.
constexpr uint32_t kPanelColour = 0xE0000000u;

// String id 33 is the main menu's own "Achievements" row label (set by that
// screen's builder, sub_825C5400) -- the entry that opens this screen.
constexpr uint32_t kAchievementsStringId = 33;
// Where a text widget keeps its own UTF-16 text (sub_825CEE40 copies into it).
constexpr uint32_t kTextWidgetBufferOffset = 572;

constexpr uint32_t kTitleY = 44;
constexpr float kTitleScale = 0.95f;

// One row per achievement: a square icon, the name beside it, and the
// description beside that.
constexpr uint32_t kFirstRowY = 84;
constexpr uint32_t kRowSpacing = 27;
constexpr int32_t kIconX = -4;
constexpr uint32_t kIconSize = 22;
constexpr uint32_t kNameX = 24;
constexpr uint32_t kDescriptionX = 210;
constexpr float kNameScale = 0.70f;
constexpr float kDescriptionScale = 0.58f;
// Rows sit a couple of pixels below their icon so the text baseline lines up.
constexpr uint32_t kTextYBias = 4;

// Unlocked rows draw at full brightness; locked ones are dimmed rather than
// hidden, so the list always shows what there is left to earn.
constexpr uint32_t kUnlockedTint = 0xFFFFFFFFu;
constexpr uint32_t kLockedTint = 0xFF585858u;
constexpr uint32_t kUnlockedTextColour = 0xFFFFFFFFu;
constexpr uint32_t kLockedTextColour = 0xFF909090u;

// The heading and the Back prompt deliberately do *not* use the game's own
// title/prompt colour constants (dword_82895090 / dword_82895098). Those are a
// dark purple, which works on the stock screens' light backing art but is
// invisible against this screen's dark panel.
constexpr uint32_t kHeadingColour = 0xFFFFFFFFu;
constexpr uint32_t kPromptTextColour = 0xFFFFFFFFu;

// The guest image bank's achievement icons are named ACHIEVEMENT_1 upwards.
// Twelve is all the screen's own Activate knows about, and all the title has.
constexpr uint32_t kMaxAchievements = 12;

// Scratch for staging UTF-16 strings into guest memory, in characters, and for
// the ASCII image-bank names, in bytes.
constexpr uint32_t kTextScratchChars = 256;
constexpr uint32_t kNameScratchBytes = 64;

// Front-end page index of CScreenAchievement, and where the UI manager keeps
// the page it is currently showing.
constexpr uint32_t kAchievementsPage = 15;
constexpr uint32_t kCurrentPageOffset = 4;

// The page-switch event class, and the arg1 that leaves the screen's image
// widget empty (anything outside 1..12 does; 0 is the obvious one).
constexpr uint32_t kEventClassPageSwitch = 8;
constexpr uint32_t kBlankAchievementArg = 0;

// The app object's "a save is loaded" latch -- read only to decide whether the
// pre-game watchdog needs suppressing on the way in. Same field
// native_options.cpp reads.
constexpr uint32_t kInGameLatchOffset = 29;

// Message layout for a screen's event handler, identical to the one
// native_options.cpp documents: word 0 is the event class (0 = button press),
// word 2 the button (4/5/6 = A/B/X), word 3 the controller that sent it.
constexpr uint32_t kMsgClass = 0;
constexpr uint32_t kMsgButton = 8;
constexpr uint32_t kMsgController = 12;
constexpr uint32_t kMsgClassButton = 0;
constexpr uint32_t kButtonCancel = 5;

PPCFunc* g_original_screen_event_fn = nullptr;
PPCFunc* g_original_screen_activate_fn = nullptr;
PPCFunc* g_post_event_fn = nullptr;
PPCFunc* g_alloc_fn = nullptr;
PPCFunc* g_image_ctor_fn = nullptr;
PPCFunc* g_image_set_rect_fn = nullptr;
PPCFunc* g_image_set_tint_fn = nullptr;
PPCFunc* g_text_ctor_fn = nullptr;
PPCFunc* g_text_set_text_fn = nullptr;
PPCFunc* g_text_set_scale_fn = nullptr;
PPCFunc* g_text_set_colour_fn = nullptr;
PPCFunc* g_find_image_fn = nullptr;
PPCFunc* g_prompt_ctor_fn = nullptr;
PPCFunc* g_prompt_set_glyph_fn = nullptr;
PPCFunc* g_prompt_show_glyph_fn = nullptr;
PPCFunc* g_prompt_set_text_by_id_fn = nullptr;
PPCFunc* g_prompt_text_offset_fn = nullptr;
PPCFunc* g_prompt_set_colour_fn = nullptr;
PPCFunc* g_prompt_set_pos_fn = nullptr;
PPCFunc* g_text_width_fn = nullptr;
PPCFunc* g_text_set_text_by_id_fn = nullptr;

// The screen we built our widgets onto, so the build happens exactly once (the
// screen object outlives every visit -- the UI manager creates all 22 up front).
uint32_t g_built_screen = 0;

// Guest scratch for staging UTF-16 text. The text setter copies out of it
// immediately, so one buffer serves every caller.
uint32_t g_text_scratch = 0;
uint32_t g_name_scratch = 0;

// One row's widgets, so a re-open can refresh text and tint without rebuilding.
struct RowWidgets {
  uint32_t icon = 0;
  uint32_t name = 0;
  uint32_t description = 0;
};
RowWidgets g_rows[kMaxAchievements];
uint32_t g_row_count = 0;

// The page the Achievements entry was selected from, so B can go back to it
// rather than guessing between the main menu and the pause menu. 0 when we are
// not inside the screen.
uint32_t g_return_page = 0;

uint32_t Read32BE(const uint8_t* base, uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(base + guest_address);
}

// Queues a front-end page switch, exactly as the screens themselves do.
void PostPageSwitch(PPCContext& ctx, uint8_t* base, uint32_t arg1, uint32_t page,
                    uint32_t controller) {
  if (!g_post_event_fn) {
    return;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = kEventClassPageSwitch;
  ctx.r4.u32 = arg1;
  ctx.r5.u32 = page;
  ctx.r6.u32 = controller;
  g_post_event_fn(ctx, base);
  ctx = saved;
}

// Calls a guest function with a scratch register set, restoring the caller's
// context afterwards -- the same pattern native_options.cpp uses to reuse a
// live context for its own guest calls.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3, uint32_t r4 = 0,
                   uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0) {
  if (!fn) {
    return 0;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = r3;
  ctx.r4.u32 = r4;
  ctx.r5.u32 = r5;
  ctx.r6.u32 = r6;
  ctx.r7.u32 = r7;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// Same, for the setters that take their value in f1 rather than a GPR.
void CallGuestFloat(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3, float value) {
  if (!fn) {
    return;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = r3;
  ctx.f1.f64 = static_cast<double>(value);
  fn(ctx, base);
  ctx = saved;
}

// Decodes one UTF-8 sequence starting at `i`, advancing `i` past it. Returns
// U+FFFD for anything malformed, so a bad byte costs one character rather than
// desynchronising the rest of the string.
char32_t DecodeUtf8(const std::string& text, size_t& i) {
  const auto byte = [&](size_t at) { return static_cast<unsigned char>(text[at]); };
  const unsigned char lead = byte(i);
  size_t extra = 0;
  char32_t codepoint = 0;
  if (lead < 0x80) {
    ++i;
    return lead;
  } else if ((lead & 0xE0) == 0xC0) {
    extra = 1;
    codepoint = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    extra = 2;
    codepoint = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    extra = 3;
    codepoint = lead & 0x07;
  } else {
    ++i;
    return 0xFFFD;
  }
  if (i + extra >= text.size()) {  // truncated sequence at the end of the string
    i = text.size();
    return 0xFFFD;
  }
  for (size_t n = 1; n <= extra; ++n) {
    const unsigned char continuation = byte(i + n);
    if ((continuation & 0xC0) != 0x80) {
      i += n;
      return 0xFFFD;
    }
    codepoint = (codepoint << 6) | (continuation & 0x3F);
  }
  i += extra + 1;
  return codepoint;
}

// Folds the typographic punctuation the title's metadata actually uses down to
// the plain ASCII equivalents the font has glyphs for.
//
// The XDBF strings are UTF-8 and use real curly quotes -- "Belmont’s Revenge" is
// U+2019, not an apostrophe. Even decoded correctly to UTF-16 those render as
// blank boxes: the Latin font is an old fixed set with no glyph for typographic
// punctuation. This is deliberately a short list of specific substitutions, not
// a general "reduce to ASCII" pass -- the font handles Japanese perfectly well
// (that is what font_ja.xpr is for), so anything outside this list is left
// exactly as it is.
char32_t FoldToFontGlyph(char32_t codepoint) {
  switch (codepoint) {
    case 0x2018:  // ' left single quote
    case 0x2019:  // ' right single quote
    case 0x201A:
    case 0x2032:  // prime
      return U'\'';
    case 0x201C:  // " left double quote
    case 0x201D:  // " right double quote
    case 0x201E:
    case 0x2033:  // double prime
      return U'"';
    case 0x2010:  // hyphen
    case 0x2011:
    case 0x2012:
    case 0x2013:  // en dash
    case 0x2014:  // em dash
    case 0x2015:
      return U'-';
    case 0x00A0:  // non-breaking space
      return U' ';
    case 0x2026:  // ellipsis -- no room to expand to "...", one dot reads better
      return U'.';
    default:
      return codepoint;
  }
}

// Copies text into the shared guest scratch buffer as UTF-16 and returns its
// guest address, or 0 if there is no scratch.
uint32_t StageText(uint8_t* base, const std::string& text) {
  if (!g_text_scratch) {
    return 0;
  }
  uint32_t length = 0;
  size_t i = 0;
  while (i < text.size() && length < kTextScratchChars - 1) {
    char32_t codepoint = FoldToFontGlyph(DecodeUtf8(text, i));
    // The scratch is UTF-16, so only genuinely unrepresentable codepoints -- the
    // ones above the BMP, which would need a surrogate pair -- become '?'. The
    // whole BMP goes through untouched: when the console language is Japanese
    // the XDBF hands back Japanese names and descriptions, and the game's font
    // draws them.
    if (codepoint > 0xFFFF) {
      codepoint = U'?';
    }
    rex::memory::store_and_swap<uint16_t>(base + g_text_scratch + length * 2,
                                          static_cast<uint16_t>(codepoint));
    ++length;
  }
  rex::memory::store_and_swap<uint16_t>(base + g_text_scratch + length * 2, 0);
  return g_text_scratch;
}

// Copies already-UTF-16 text (as returned by FindNativeStringTranslation) into
// the shared guest scratch buffer and returns its guest address, or 0 if
// there is no scratch. Unlike StageText this needs no UTF-8 decoding or font
// folding -- a mod-supplied translation is expected to only use characters the
// current language's font can already render, same assumption
// native_options.cpp makes for its own row text.
uint32_t StageTextU16(uint8_t* base, const char16_t* text) {
  if (!g_text_scratch || !text) {
    return 0;
  }
  uint32_t length = 0;
  while (text[length] != u'\0' && length < kTextScratchChars - 1) {
    rex::memory::store_and_swap<uint16_t>(base + g_text_scratch + length * 2,
                                          static_cast<uint16_t>(text[length]));
    ++length;
  }
  rex::memory::store_and_swap<uint16_t>(base + g_text_scratch + length * 2, 0);
  return g_text_scratch;
}

// Mirrors settings.h's kNativeStringKey* convention for native_options.cpp's
// own rows, but keyed per achievement id instead of a fixed name: a mod calls
// RegisterNativeStringListener's "settings.native_string" event with one of
// these as the key (id being the achievement's own AchievementInfo::id, not
// its row index -- row order can change if the title ever adds one) to
// override that one field for the achievement in a language it registered via
// RegisterLanguageOptionsListener. Falls back to the XDBF string (whatever
// language the title metadata carries) when nothing is registered.
std::string AchievementNameKey(uint32_t id) { return "achv_name_" + std::to_string(id); }
std::string AchievementDescriptionKey(uint32_t id) { return "achv_desc_" + std::to_string(id); }
std::string AchievementLockedDescriptionKey(uint32_t id) {
  return "achv_desc_locked_" + std::to_string(id);
}

// Stages a plain ASCII, NUL-terminated string for the guest. The image-bank
// lookup takes a normal C string (the stock callers pass .rodata pointers), not
// the UTF-16 the text setters want, so it gets its own buffer.
uint32_t StageAsciiName(uint8_t* base, const std::string& text) {
  if (!g_name_scratch) {
    return 0;
  }
  const uint32_t length =
      std::min<uint32_t>(static_cast<uint32_t>(text.size()), kNameScratchBytes - 1);
  for (uint32_t i = 0; i < length; ++i) {
    base[g_name_scratch + i] = static_cast<uint8_t>(text[i]);
  }
  base[g_name_scratch + length] = 0;
  return g_name_scratch;
}

// Creates an image widget parented to `screen`, so the screen draws it.
// A null `image` gives a solid rect of whatever tint is set on it.
uint32_t CreateImage(PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t x, uint32_t y,
                     uint32_t image) {
  const uint32_t memory = CallGuest(ctx, base, g_alloc_fn, kImageWidgetSize);
  if (!memory) {
    return 0;
  }
  return CallGuest(ctx, base, g_image_ctor_fn, memory, screen, x, y, image);
}

// Creates a text widget parented to `screen`, with literal text at a scale.
//
// Everything on this screen is literal text rather than a string-table id,
// because none of it has an id: the achievement names and descriptions come
// from the title's XDBF metadata (via the SDK), which the string tables know
// nothing about. That also means this screen does not follow the in-game
// Language setting the way the stock ones do by itself -- the XDBF strings
// are whatever language the title metadata carries. A mod can still supply a
// translation per achievement (see AchievementNameKey/AchievementDescriptionKey/
// AchievementLockedDescriptionKey and SetTranslatableText below), the same
// "settings.native_string" mechanism native_options.cpp uses for its own
// synthesized row text; RefreshList prefers that over the XDBF string when
// one is registered for the current language. The heading and Back prompt
// are unaffected -- both are already string-table ids, so they follow
// whatever strings_<code>.bin the current language loads with no help
// needed here.
uint32_t CreateText(PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t x, uint32_t y,
                    float scale) {
  const uint32_t memory = CallGuest(ctx, base, g_alloc_fn, kTextWidgetSize);
  if (!memory) {
    return 0;
  }
  const uint32_t widget = CallGuest(ctx, base, g_text_ctor_fn, memory, screen);
  if (!widget) {
    return 0;
  }
  rex::memory::store_and_swap<uint32_t>(base + widget + kWidgetXOffset, x);
  rex::memory::store_and_swap<uint32_t>(base + widget + kWidgetYOffset, y);
  CallGuestFloat(ctx, base, g_text_set_scale_fn, widget, scale);
  return widget;
}

void SetText(PPCContext& ctx, uint8_t* base, uint32_t widget, const std::string& text) {
  if (widget) {
    CallGuest(ctx, base, g_text_set_text_fn, widget, StageText(base, text));
  }
}

// Sets a text widget's contents, preferring a mod-registered translation for
// the current language over `fallback` (the XDBF string). `key` identifies
// which field this is (see AchievementNameKey/AchievementDescriptionKey/
// AchievementLockedDescriptionKey).
void SetTranslatableText(PPCContext& ctx, uint8_t* base, uint32_t widget, const std::string& key,
                         const std::string& fallback) {
  if (!widget) {
    return;
  }
  const uint32_t language_id = REXCVAR_QUERY(uint32_t, user_language);
  if (const char16_t* translated = FindNativeStringTranslation(language_id, key)) {
    CallGuest(ctx, base, g_text_set_text_fn, widget, StageTextU16(base, translated));
    return;
  }
  SetText(ctx, base, widget, fallback);
}

// Adds the "Back" prompt for B. Only one prompt, so it is positioned directly
// rather than through the three-at-a-time bar layout the stock screens use.
void CreateBackPrompt(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  const uint32_t memory = CallGuest(ctx, base, g_alloc_fn, kPromptSize);
  if (!memory) {
    return;
  }
  const uint32_t prompt = CallGuest(ctx, base, g_prompt_ctor_fn, memory, screen, 0);
  if (!prompt) {
    return;
  }
  const uint32_t bank = rex::memory::load_and_swap<uint32_t>(base + kImageBankPtrAddr);
  const uint32_t glyph = CallGuest(ctx, base, g_find_image_fn, bank, kBGlyphNameAddr);
  if (glyph) {
    CallGuest(ctx, base, g_prompt_set_glyph_fn, prompt, glyph);
  }
  CallGuest(ctx, base, g_prompt_show_glyph_fn, prompt);
  CallGuest(ctx, base, g_prompt_set_text_by_id_fn, prompt, kPromptBackStringId);
  CallGuest(ctx, base, g_prompt_text_offset_fn, prompt, kPromptTextDx, kPromptTextDy);
  CallGuest(ctx, base, g_prompt_set_colour_fn, prompt, kPromptTextColour);

  // Centre it: measure the glyph plus the text, then place the pair so the whole
  // thing straddles the middle of the safe area. Measured rather than eyeballed
  // because the label is localized and its width moves with the language.
  const uint32_t glyph_widget = Read32BE(base, prompt + kPromptGlyphOffset);
  const uint32_t text = Read32BE(base, prompt + kPromptTextOffset);
  const uint32_t glyph_width =
      glyph_widget ? Read32BE(base, glyph_widget + kPromptGlyphWidthOffset) : 0;
  const uint32_t text_width = text ? CallGuest(ctx, base, g_text_width_fn, text) : 0;
  const uint32_t total = glyph_width + kPromptTextDx + text_width;
  const uint32_t x = (total < kSafeAreaWidth) ? (kSafeAreaWidth - total) / 2 : 0;

  // A prompt is a glyph widget plus a text widget drawn at an offset from it,
  // so it has to be positioned through its own setter -- writing +4/+8 the way
  // a plain widget allows moves only part of it.
  CallGuest(ctx, base, g_prompt_set_pos_fn, prompt, x, kPromptBarY);
}

// Sets the heading to the game's own localized "Achievements" label, uppercased.
//
// The label is string id 33 -- the id the main menu's builder (sub_825C5400)
// gives its own Achievements row, which is the very entry that opens this
// screen. Taking it from the string table rather than hardcoding English means
// the heading follows the in-game Language setting like the stock screens do;
// the achievement names and descriptions below it still cannot, since those come
// from XDBF metadata the string tables know nothing about.
//
// There is no by-id setter that also uppercases, so this sets the localized
// string, reads it back out of the widget's own buffer, uppercases it and writes
// it back as literal text. The by-id setter also copies the string table entry's
// scale into the widget, so the caller's scale has to be reasserted afterwards.
void SetHeadingText(PPCContext& ctx, uint8_t* base, uint32_t widget, float scale) {
  if (!widget || !g_text_set_text_by_id_fn) {
    return;
  }
  CallGuest(ctx, base, g_text_set_text_by_id_fn, widget, kAchievementsStringId);

  std::u16string text;
  for (uint32_t i = 0; i < kTextScratchChars - 1; ++i) {
    const uint16_t unit =
        rex::memory::load_and_swap<uint16_t>(base + widget + kTextWidgetBufferOffset + i * 2);
    if (unit == 0) {
      break;
    }
    // ASCII and the Latin-1 accented range both uppercase by clearing the same
    // bit; anything else (Japanese, say) has no case and is left alone.
    uint16_t upper = unit;
    if (unit >= u'a' && unit <= u'z') {
      upper = static_cast<uint16_t>(unit - 0x20);
    } else if (unit >= 0xE0 && unit <= 0xFE && unit != 0xF7) {
      upper = static_cast<uint16_t>(unit - 0x20);
    }
    text.push_back(static_cast<char16_t>(upper));
  }
  if (text.empty()) {
    return;  // no string table entry; leave whatever the by-id setter produced
  }

  if (g_text_scratch) {
    for (size_t i = 0; i < text.size(); ++i) {
      rex::memory::store_and_swap<uint16_t>(base + g_text_scratch + i * 2,
                                            static_cast<uint16_t>(text[i]));
    }
    rex::memory::store_and_swap<uint16_t>(base + g_text_scratch + text.size() * 2, 0);
    CallGuest(ctx, base, g_text_set_text_fn, widget, g_text_scratch);
  }
  CallGuestFloat(ctx, base, g_text_set_scale_fn, widget, scale);
}

const std::vector<rex::system::AchievementInfo>& CachedAchievements() {
  // Sorted by id so the order is stable, and so row N lines up with the guest
  // image bank's ACHIEVEMENT_<N+1> -- see BuildList's note on that mapping.
  static std::vector<rex::system::AchievementInfo> achievements = [] {
    std::vector<rex::system::AchievementInfo> list;
    auto* ks = rex::system::kernel_state();
    if (ks) {
      list = ks->achievements().ListAchievements();
    }
    std::sort(list.begin(), list.end(),
              [](const rex::system::AchievementInfo& a, const rex::system::AchievementInfo& b) {
                return a.id < b.id;
              });
    return list;
  }();
  return achievements;
}

// Builds the list onto the screen, once. Runs from the Activate override rather
// than the screen's own build so the SDK's achievement metadata is certain to be
// loaded by the time we ask for it (the screen objects are all constructed
// during UI manager init, much earlier).
void BuildList(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  if (!screen || g_built_screen == screen) {
    return;
  }
  g_built_screen = screen;

  if (!g_text_scratch) {
    g_text_scratch = CallGuest(ctx, base, g_alloc_fn, kTextScratchChars * 2);
  }
  if (!g_name_scratch) {
    g_name_scratch = CallGuest(ctx, base, g_alloc_fn, kNameScratchBytes);
  }

  // Backing panel first, so everything after it draws on top. The rect setter
  // just subtracts to get the size, so the negative left edge goes in as its
  // two's complement and comes out as the width we want.
  const uint32_t panel = CreateImage(ctx, base, screen, static_cast<uint32_t>(kFullBleedLeft), 0, 0);
  if (panel) {
    CallGuest(ctx, base, g_image_set_rect_fn, panel, static_cast<uint32_t>(kFullBleedLeft), 0,
              static_cast<uint32_t>(kFullBleedRight), kScreenHeight);
    CallGuest(ctx, base, g_image_set_tint_fn, panel, kPanelColour);
  }

  const uint32_t title = CreateText(ctx, base, screen, 0, kTitleY, kTitleScale);
  if (title) {
    SetHeadingText(ctx, base, title, kTitleScale);
    CallGuest(ctx, base, g_text_set_colour_fn, title, kHeadingColour);
    // Centre it. Needs the text already set, since the width is measured.
    const uint32_t width = CallGuest(ctx, base, g_text_width_fn, title);
    const uint32_t x = (width < kSafeAreaWidth) ? (kSafeAreaWidth - width) / 2 : 0;
    rex::memory::store_and_swap<uint32_t>(base + title + kWidgetXOffset, x);
  }

  const uint32_t bank = rex::memory::load_and_swap<uint32_t>(base + kImageBankPtrAddr);
  const auto& achievements = CachedAchievements();
  g_row_count = std::min<uint32_t>(static_cast<uint32_t>(achievements.size()), kMaxAchievements);

  // The art the lookup below wants was never shipped in the game's banks, so
  // put it there first, out of the XEX's own XDBF metadata. See
  // achievement_icons.h.
  RegisterAchievementIcons(base, bank, {achievements.data(), g_row_count});

  for (uint32_t i = 0; i < g_row_count; ++i) {
    const uint32_t y = kFirstRowY + i * kRowSpacing;

    // The same name CScreenAchievement itself looks up -- satisfied by the
    // entries RegisterAchievementIcons just added, since the retail banks carry
    // no achievement art at all.
    //
    // An image widget with a null image draws a solid rect of its tint, so if
    // the icon is still missing (no XDBF image for that achievement, say) there
    // must be no widget at all rather than a grey square.
    const std::string image_name = "ACHIEVEMENT_" + std::to_string(i + 1);
    const uint32_t name_ptr = StageAsciiName(base, image_name);
    const uint32_t image = name_ptr ? CallGuest(ctx, base, g_find_image_fn, bank, name_ptr) : 0;

    RowWidgets& row = g_rows[i];
    if (image) {
      row.icon = CreateImage(ctx, base, screen, static_cast<uint32_t>(kIconX), y, image);
    }
    if (row.icon) {
      const uint32_t w =
          rex::memory::load_and_swap<uint32_t>(base + row.icon + kImageWidthOffset);
      const uint32_t h =
          rex::memory::load_and_swap<uint32_t>(base + row.icon + kImageHeightOffset);
      REXLOG_INFO("[achievements_screen] {} -> image {:08X} ({}x{})", image_name, image, w, h);
      // Scale whatever the art actually is into a square icon box.
      CallGuest(ctx, base, g_image_set_rect_fn, row.icon, static_cast<uint32_t>(kIconX), y,
                static_cast<uint32_t>(kIconX + static_cast<int32_t>(kIconSize)), y + kIconSize);
    } else {
      REXLOG_INFO("[achievements_screen] {} not present in the guest image bank", image_name);
    }
    row.name = CreateText(ctx, base, screen, kNameX, y + kTextYBias, kNameScale);
    row.description =
        CreateText(ctx, base, screen, kDescriptionX, y + kTextYBias, kDescriptionScale);
  }

  CreateBackPrompt(ctx, base, screen);
  REXLOG_INFO("[achievements_screen] built {} achievement rows", g_row_count);
}

// Refreshes every row's text and locked/unlocked styling. Runs on each open, so
// an achievement unlocked mid-session shows up without a restart.
void RefreshList(PPCContext& ctx, uint8_t* base) {
  auto* ks = rex::system::kernel_state();
  const auto& achievements = CachedAchievements();

  for (uint32_t i = 0; i < g_row_count && i < achievements.size(); ++i) {
    const rex::system::AchievementInfo& info = achievements[i];
    const bool unlocked = ks && ks->achievements().IsUnlocked(info.id);
    const RowWidgets& row = g_rows[i];

    SetTranslatableText(ctx, base, row.name, AchievementNameKey(info.id), info.label);
    // A locked achievement shows its "unachieved" blurb when the title provides
    // one -- that is exactly what that field is for, and some of them are
    // deliberately vaguer than the real description. A mod's translation
    // follows the same split: AchievementLockedDescriptionKey only applies
    // while locked, and only when the XDBF title itself has an unachieved
    // blurb to begin with -- a language that didn't bother translating the
    // vague version isn't missing anything, it falls through to the regular
    // description key like every other locked achievement without one.
    const bool use_locked_blurb = !unlocked && !info.unachieved_description.empty();
    const std::string& blurb = use_locked_blurb ? info.unachieved_description : info.description;
    const std::string blurb_key = use_locked_blurb ? AchievementLockedDescriptionKey(info.id)
                                                    : AchievementDescriptionKey(info.id);
    SetTranslatableText(ctx, base, row.description, blurb_key, blurb);

    if (row.icon) {
      CallGuest(ctx, base, g_image_set_tint_fn, row.icon,
                unlocked ? kUnlockedTint : kLockedTint);
    }
    const uint32_t text_colour = unlocked ? kUnlockedTextColour : kLockedTextColour;
    CallGuest(ctx, base, g_text_set_colour_fn, row.name, text_colour);
    CallGuest(ctx, base, g_text_set_colour_fn, row.description, text_colour);
  }
}

// Leaves the screen, back to wherever we came from, and disarms the pre-game
// watchdog suppression we may have armed on the way in.
void LeaveScreen(PPCContext& ctx, uint8_t* base, uint32_t controller) {
  const uint32_t page = g_return_page;
  g_return_page = 0;
  EnterPregameScreen(0);
  if (page) {
    PostPageSwitch(ctx, base, 0, page, controller);
  }
}

}  // namespace

// CScreenAchievement's event handler (vtable slot 1). B backs out; everything
// else falls through to the stock handler, which forwards to the shared screen
// base handler.
//
// The stock handler has no notion of leaving this screen -- on real hardware
// nothing ever navigated to it -- so B is ours to define. Returning 1 without
// running the original is what the other screens' own navigation paths do.
extern "C" void AchievementsScreen_ScreenEvent(PPCContext& ctx, uint8_t* base) {
  const uint32_t message = ctx.r4.u32;

  if (message && g_return_page) {
    const uint32_t message_class = Read32BE(base, message + kMsgClass);
    const uint32_t button = Read32BE(base, message + kMsgButton);

    // The screen was never wired for input, so it is not yet established which
    // messages actually reach it. Log them until that is settled in-game.
    REXLOG_DEBUG("[achievements_screen] message class {} button {}", message_class, button);

    if (message_class == kMsgClassButton && button == kButtonCancel) {
      LeaveScreen(ctx, base, Read32BE(base, message + kMsgController));
      ctx.r3.u32 = 1;
      return;
    }
  }

  if (g_original_screen_event_fn) {
    g_original_screen_event_fn(ctx, base);
  }
}

// CScreenAchievement's Activate (vtable slot 15). The stock body points the
// screen's one stock image widget at ACHIEVEMENT_<arg1> and is a no-op for the
// arg1 = 0 we always pass, so it runs first and we build/refresh on top of it.
extern "C" void AchievementsScreen_ScreenActivate(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;

  if (g_original_screen_activate_fn) {
    g_original_screen_activate_fn(ctx, base);
  }

  // Only dress the screen when we are the ones who opened it. Nothing else ever
  // navigates here in the stock game, but that is an argument for cheap caution,
  // not against it.
  if (!screen || !g_return_page) {
    return;
  }
  BuildList(ctx, base, screen);
  RefreshList(ctx, base);
}

AchievementsScreen& GetAchievementsScreen() {
  static AchievementsScreen instance;
  return instance;
}

bool AchievementsScreen::OpenFromGuest(PPCContext& ctx, uint8_t* base, uint32_t user_index) {
  if (!available_ || g_return_page != 0) {
    return false;  // not wired up, or already inside the screen
  }

  const uint32_t manager = Read32BE(base, kUiManagerPtrAddr);
  const uint32_t current_page = manager ? Read32BE(base, manager + kCurrentPageOffset) : 0;
  if (!current_page) {
    REXLOG_WARN("[achievements_screen] no current front-end page; leaving the button alone");
    return false;
  }
  g_return_page = current_page;

  // With no save loaded, CScreenAchievement::Activate clears the UI manager's
  // settled flag (mgr+332) exactly as the stretch screen's does, which trips
  // the pre-game watchdog into yanking us straight back out. native_options
  // owns the suppression for that -- see EnterPregameScreen.
  const uint32_t app_object = Read32BE(base, kAppObjectPtrAddr);
  const bool save_loaded = app_object != 0 && base[app_object + kInGameLatchOffset] != 0;
  if (!save_loaded) {
    EnterPregameScreen(kAchievementsPage);
  }

  REXLOG_INFO("[achievements_screen] opening page {} from page {} (save loaded: {})",
              kAchievementsPage, current_page, save_loaded);
  PostPageSwitch(ctx, base, kBlankAchievementArg, kAchievementsPage, user_index);
  return true;
}

void AchievementsScreen::Bind(rex::Runtime* runtime) {
  runtime_ = runtime;
  if (!runtime_ || !runtime_->function_dispatcher()) {
    return;
  }
  auto* dispatcher = runtime_->function_dispatcher();

  g_post_event_fn = dispatcher->GetFunction(kPostEventFnAddr);
  if (!g_post_event_fn) {
    REXLOG_WARN("[achievements_screen] page-switch post {:08X} unavailable; the Achievements "
                "entry keeps opening the SDK overlay",
                kPostEventFnAddr);
    return;
  }

  if (!dispatcher->OverrideFunction(kScreenEventFnAddr, &AchievementsScreen_ScreenEvent,
                                    &g_original_screen_event_fn)) {
    REXLOG_WARN("[achievements_screen] OverrideFunction failed for {:08X} (screen events); "
                "without a way back out the Achievements entry keeps opening the SDK overlay",
                kScreenEventFnAddr);
    return;
  }

  // From here on failures are survivable: the screen still opens and still backs
  // out, it just has nothing on it.
  available_ = true;

  g_alloc_fn = dispatcher->GetFunction(kAllocFnAddr);
  g_image_ctor_fn = dispatcher->GetFunction(kImageCtorFnAddr);
  g_image_set_rect_fn = dispatcher->GetFunction(kImageSetRectFnAddr);
  g_image_set_tint_fn = dispatcher->GetFunction(kImageSetTintFnAddr);
  g_text_ctor_fn = dispatcher->GetFunction(kTextCtorFnAddr);
  g_text_set_text_fn = dispatcher->GetFunction(kTextSetTextFnAddr);
  g_text_set_scale_fn = dispatcher->GetFunction(kTextSetScaleFnAddr);
  g_text_set_colour_fn = dispatcher->GetFunction(kTextSetColourFnAddr);
  g_find_image_fn = dispatcher->GetFunction(kFindImageFnAddr);
  g_prompt_ctor_fn = dispatcher->GetFunction(kPromptCtorFnAddr);
  g_prompt_set_glyph_fn = dispatcher->GetFunction(kPromptSetGlyphFnAddr);
  g_prompt_show_glyph_fn = dispatcher->GetFunction(kPromptShowGlyphFnAddr);
  g_prompt_set_text_by_id_fn = dispatcher->GetFunction(kPromptSetTextByIdFnAddr);
  g_prompt_text_offset_fn = dispatcher->GetFunction(kPromptTextOffsetFnAddr);
  g_prompt_set_colour_fn = dispatcher->GetFunction(kPromptSetColourFnAddr);
  g_prompt_set_pos_fn = dispatcher->GetFunction(kPromptSetPosFnAddr);
  g_text_width_fn = dispatcher->GetFunction(kTextWidthFnAddr);
  g_text_set_text_by_id_fn = dispatcher->GetFunction(kTextSetTextByIdFnAddr);

  if (!g_alloc_fn || !g_image_ctor_fn || !g_text_ctor_fn || !g_text_set_text_fn) {
    REXLOG_WARN("[achievements_screen] widget builders unavailable; the screen will open empty");
    return;
  }

  if (!dispatcher->OverrideFunction(kScreenActivateFnAddr, &AchievementsScreen_ScreenActivate,
                                    &g_original_screen_activate_fn)) {
    REXLOG_WARN("[achievements_screen] OverrideFunction failed for {:08X} (screen activate); the "
                "screen will open empty",
                kScreenActivateFnAddr);
  }
}

}  // namespace nocturne
