// nocturnerecomp - JP's Japanese text and glyphs on the PS1 side of World's code.
//
// World's executable has Japanese PS1-side text of its own (language index 1),
// but it is unfinished: strings cut short (Soul of Bat's name is a single
// stray kanji), a corrupt glyph in its kanji font (亜 draws as a bar), a
// Librarian entry too long for its row, and screens that stay in English
// (enemy names, the bestiary, the tactics bosses). JP's executable is the same
// code base finished in Japanese, with its data laid out differently, so it
// can't simply be mapped over World's.
//
// At startup, with JP installed, JP's default.xex is decrypted and the pieces
// of its text World lacks are copied into guest memory, converted to the
// layout World's code expects where it differs (see the tables in
// jp_text.cpp, which name both releases' addresses):
//
// - glyphs World draws wrongly are overwritten with JP's;
// - slots World reads only in Japanese are pointed at the copies once;
// - slots World reads in every language (it shows English there in Japanese)
//   are swapped while the PS1 side is in Japanese, by ApplyPendingJpText.
//
// It applies whether World's or JP's folder is mounted: the code and its data
// are World's either way (core/guest_image.h). Nothing happens when JP is not
// installed.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

// After the image is mapped and before the guest starts. Returns whether JP's
// text is in place.
bool InstallJpText(rex::Runtime* runtime);

// Follows the PS1 side's language (and whose voices play) with the shared
// slots. Runs from the per-frame hook (GraphicsSettings_PerFrame).
void ApplyPendingJpText();

// The PS1 side is in Japanese (its language index is 1) and JP's text is in
// place: what the hooks check before doing anything.
bool JpTextShown();

// JP's prologue can be shown, so the prologue setting is a choice: World
// scrolls a Japanese prologue of its own, JP's release the original English
// one. The prologue cvar ("auto", "jp" or "world") picks; auto is JP's, since
// the choice only exists in Japanese.
bool JpPrologueAvailable();

}  // namespace nocturne
