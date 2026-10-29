// nocturnerecomp - applies a `user_language` change while the game runs,
// instead of on the next launch.
//
// The game loads everything language-specific once, at boot (sub_82586AA8,
// called from the app init sub_82576C58):
//
//   * strings_<code>.bin, read into one heap block whose pointer, entry array
//     and count sit at 0x82E61850/54/58. Each 12-byte entry is
//     {text, font name, scale}; the file stores the pointers as offsets and
//     the scale in thousandths, which the loader fixes up in place.
//   * the bitmap font (font_ja.xpr for Japanese, font_en.xpr for the Latin
//     languages), created by sub_825CEC78 and stored at 0x82E7A590. Every
//     text draw and measure reads that pointer fresh.
//   * the PS1 side's own language index at 0x8313BC20 (sub_824FAEF8), which
//     its code uses to pick from its static per-language tables. It also
//     picks each dialogue scene's script routine, so it only changes once no
//     scene is running.
//
// Anything else that branches on the language (screen layouts, the native
// options labels) asks XConfig or the cvar each time, so it is already live.
//
// Switching swaps those three globals over. Each table and font is loaded
// once and then kept, never freed: the old font's texture may still be
// referenced by a frame the GPU hasn't finished, and switching back to a
// language seen before then costs nothing.
//
// Widgets are the other half. Text set by string id (sub_825CFC68) is
// *copied* into the widget, so a screen built before the switch would keep
// its old text until the next launch. NoteTextSetById records every such
// call; the switch rewrites each recorded widget whose text still matches
// what that id said in the old language. A widget that has since been given
// other text, or freed and reused, fails that check and is left alone.
#pragma once

#include <cstdint>

struct PPCContext;

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

// Makes `user_language` a hot-reload cvar and starts listening for changes.
// Call once at setup, before the player can change any settings.
void BindLanguageSwitch(rex::Runtime* runtime);

// Records that `text_widget` was just given string `string_id`. Called from
// the hook on sub_825CFC68, after the original has run.
void NoteTextSetById(uint32_t text_widget, uint32_t string_id);

// Applies a language change, if one is pending. Needs a live guest context,
// so it runs from the per-frame hook (GraphicsSettings_PerFrame).
void ApplyPendingLanguageSwitch(PPCContext& ctx, uint8_t* base);

}  // namespace nocturne
