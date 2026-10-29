// nocturnerecomp: shared helpers for the text and widgets this project adds
// to the game's own front-end screens: localized literal text, guest calls,
// and a guest-side staging buffer for UTF-16 strings.
//
// The game's own text comes from strings_<code>.bin by id. Anything this
// project adds has no id, so it lives in the [native] table of res/lang (see
// ui_text.h) and is set as literal text instead.
#pragma once

#include <cstdint>

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

namespace nocturne {

// One string of res/lang's [native] table. Literal text renders through the
// same language-selected font as looked-up text, so it must only use
// characters that font has: font_en.xpr covers Latin-1, but font_ja.xpr is a
// subset font, so every kanji and kana in ja.toml's [native] table must be in
// it or in the supplemental pack (scripts/gen-glyphs.py).
//
// Write the key as a literal right after the type (or the variable's name), in
// braces: that is the form scripts/check-lang.py finds keys in.
struct NativeText {
  const char* key = nullptr;

  // Only the current language's own text, with no English fallback: for a
  // label that corrects one language's stock text (see the Graphics row in
  // native_options.cpp).
  bool current_only = false;

  // Null when there is no key, or under current_only when the current
  // language has no such string.
  const char16_t* Get() const;
};

// "Off" / "On", for every two-valued row.
extern const NativeText kOffOnTexts[2];

inline uint32_t Read32BE(const uint8_t* base, uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(base + guest_address);
}

inline void Write32BE(uint8_t* base, uint32_t guest_address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(base + guest_address, value);
}

inline void Write16BE(uint8_t* base, uint32_t guest_address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(base + guest_address, value);
}

// The recompiled function at `address`, going through any hook on it.
PPCFunc* GuestFunction(uint32_t address);

// Calls a guest function with up to five integer arguments on a scratch copy
// of the caller's registers, and returns r3.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3 = 0, uint32_t r4 = 0,
                   uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0);

inline uint32_t CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t r3 = 0,
                          uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0) {
  return CallGuest(ctx, base, GuestFunction(address), r3, r4, r5, r6, r7);
}

// Copies `text` into a guest buffer allocated from the game's heap on first
// use and returns its address, or 0 for no text or no buffer. The buffer is
// shared: the guest text setters copy out of it straight away, so it only has
// to live until the next call. Longer text is cut to fit.
inline constexpr uint32_t kStagedTextChars = 256;
uint32_t StageText(PPCContext& ctx, uint8_t* base, const char16_t* text);

// Widget text and scale. A literal text setter writes only the text, leaving
// the widget's scale alone, unlike the by-id one (which copies the string
// table entry's), so callers usually want SetWidgetScale after it.
//
// SetWidgetText is for the composite widgets list rows, list items and prompts
// are made of (a glyph plus a text widget at +548, which it forwards to);
// SetTextWidgetText for a plain text widget, like a screen's title.
void SetWidgetText(PPCContext& ctx, uint8_t* base, uint32_t widget, const char16_t* text);
void SetTextWidgetText(PPCContext& ctx, uint8_t* base, uint32_t text_widget,
                       const char16_t* text);
void SetWidgetScale(PPCContext& ctx, uint8_t* base, uint32_t widget, float scale);

// Shows or hides a widget through its own vtable (slots +16 and +20), the
// same virtuals the screens call.
void SetWidgetVisible(PPCContext& ctx, uint8_t* base, uint32_t widget, bool visible);

// Sets an option-list item to literal text, laid out exactly as the game's
// by-id setter for centred items (sub_825D29A8, also sub_825D3DB8) does it:
// the list's normal or selected scale, then centred on the list.
void SetListItemText(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t index,
                     const char16_t* text);

}  // namespace nocturne
