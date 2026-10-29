// nocturnerecomp - the font the overlays draw Japanese in.
//
// PT Serif, the overlays' default, has no kana or kanji, so while the
// interface language is Japanese they push the pixel font registered in
// OnConfigureFonts instead (see nocturnerecomp_app.h).
#pragma once

#include <imgui.h>

#include "ui/ui_text.h"

namespace nocturne {

// Set once while the atlas is configured; null if it was never added.
inline ImFont* g_japanese_font = nullptr;

inline void SetJapaneseFont(ImFont* font) { g_japanese_font = font; }

// Draws what is inside it in the Japanese font when the interface is Japanese.
// Put one at the top of an overlay's draw, before its ImGui::Begin, so the
// window's title bar uses it too.
class UiFontScope {
 public:
  UiFontScope() : pushed_(g_japanese_font && UiLanguageId() == 2) {
    if (pushed_)
      ImGui::PushFont(g_japanese_font);
  }
  ~UiFontScope() {
    if (pushed_)
      ImGui::PopFont();
  }
  UiFontScope(const UiFontScope&) = delete;
  UiFontScope& operator=(const UiFontScope&) = delete;

 private:
  bool pushed_;
};

}  // namespace nocturne
