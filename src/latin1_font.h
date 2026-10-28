// nocturnerecomp - the accented half of the PS1 side's text font, drawn for
// JP's release, which doesn't ship it.
//
// World's code draws the cp1252 characters 0x80..0xFF of the dialogue and the
// other PS1-side text (è, ñ, ß, ¡, «...) from latin1.bin and latin1_upres.bin
// in DATA\fonts\fonts.zip. Only World's fonts.zip has those two entries, so
// with JP's folder mounted every accented character of the European languages
// is blank. BuildLatin1Font draws the table from the ASCII font in F_GAME.BIN,
// which both releases share, with our own accent marks and symbols; the font
// overlay (font_overlay.h) adds it to the zip the guest reads.
//
// The table is a 128x88 sheet at 4 bits per pixel (the left pixel in the low
// nibble), 16 cells of 8x11 to a row, from 0x80. A cell holds an ASCII glyph
// three rows down (capitals on rows 3..9, the x-height on 5..9, descenders on
// row 10), so the marks of capitals go on rows 0..1 and those of lowercase
// letters on rows 2..3.
#pragma once

#include <cstdint>
#include <vector>

namespace nocturne {

inline constexpr const char* kLatin1FontName = "latin1.bin";
inline constexpr const char* kLatin1UpresFontName = "latin1_upres.bin";
inline constexpr const char* kGameFontName = "F_GAME.BIN";

// The table from F_GAME.BIN's bytes. The two tables differ only in the
// diaeresis: two pixels per dot in the upres one. Empty when the ASCII font
// isn't where it is in both releases.
std::vector<uint8_t> BuildLatin1Font(const std::vector<uint8_t>& game_font, bool upres);

}  // namespace nocturne
