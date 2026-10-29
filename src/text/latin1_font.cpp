#include "text/latin1_font.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace nocturne {
namespace {

// Both sheets are 128 pixels wide at 4 bits per pixel.
constexpr size_t kSheetStride = 64;
constexpr int kCellWidth = 8;
constexpr int kCellsPerRow = 16;

// F_GAME.BIN's ASCII font: 8x8 cells from 0x20 to 0x7F.
constexpr size_t kAsciiOffset = 0x30000;
constexpr int kAsciiHeight = 8;
constexpr int kFirstAscii = 0x20;
constexpr int kAsciiRows = 6;

constexpr int kFirstLatin1 = 0x80;
constexpr int kLatin1Height = 11;
constexpr int kLatin1Rows = 8;
constexpr size_t kLatin1Size = kSheetStride * kLatin1Height * kLatin1Rows;
// Where an ASCII glyph's top row goes in a cell.
constexpr int kBaseTop = 3;
// The font's one color.
constexpr uint8_t kInk = 6;

// A cell's rows, bit x for column x.
using Glyph = std::array<uint8_t, kLatin1Height>;

uint8_t ParseRow(const char* art) {
  uint8_t row = 0;
  for (int x = 0; art[x]; ++x) {
    if (art[x] == '#')
      row |= 1 << x;
  }
  return row;
}

std::array<uint8_t, kAsciiHeight> AsciiGlyph(const std::vector<uint8_t>& font, char c) {
  const int index = c - kFirstAscii;
  const size_t cell = kAsciiOffset + size_t(index / kCellsPerRow) * kAsciiHeight * kSheetStride +
                      size_t(index % kCellsPerRow) * kCellWidth / 2;
  std::array<uint8_t, kAsciiHeight> rows{};
  for (int y = 0; y < kAsciiHeight; ++y) {
    for (int x = 0; x < kCellWidth; ++x) {
      const uint8_t byte = font[cell + y * kSheetStride + x / 2];
      if ((x & 1 ? byte >> 4 : byte & 0xF) != 0)
        rows[y] |= 1 << x;
    }
  }
  return rows;
}

void Put(std::vector<uint8_t>& sheet, int code, const Glyph& glyph) {
  const int index = code - kFirstLatin1;
  const size_t cell = size_t(index / kCellsPerRow) * kLatin1Height * kSheetStride +
                      size_t(index % kCellsPerRow) * kCellWidth / 2;
  for (int y = 0; y < kLatin1Height; ++y) {
    for (int x = 0; x < kCellWidth; ++x) {
      if (glyph[y] >> x & 1)
        sheet[cell + y * kSheetStride + x / 2] |= x & 1 ? kInk << 4 : kInk;
    }
  }
}

// ---------------------------------------------------------------------------
// Letters with a mark: an ASCII letter and an accent centered over it
// ---------------------------------------------------------------------------

struct Mark {
  std::array<const char*, 3> rows;
};

constexpr Mark kGrave{{"#.", ".#"}};
constexpr Mark kAcute{{".#", "#."}};
constexpr Mark kCircumflex{{".#.", "#.#"}};
constexpr Mark kTilde{{"##.#", "#.##"}};
constexpr Mark kDiaeresis{{"#..#"}};
constexpr Mark kDiaeresisUpres{{"##..##"}};
// Over the narrow dotless i, in both tables.
constexpr Mark kDiaeresisNarrow{{"#.#"}};
constexpr Mark kRing{{"###", "#.#", "###"}};
// Under the letter, which moves up a row to make room.
constexpr Mark kCedilla{{".#", "##"}};

struct Accented {
  uint8_t code;
  char base;
  const Mark* mark;
};

// The diaeresis is picked per table (see Compose).
constexpr Accented kAccented[] = {
    {0xC0, 'A', &kGrave},     {0xC1, 'A', &kAcute},      {0xC2, 'A', &kCircumflex},
    {0xC3, 'A', &kTilde},     {0xC4, 'A', &kDiaeresis},  {0xC5, 'A', &kRing},
    {0xC7, 'C', &kCedilla},   {0xC8, 'E', &kGrave},      {0xC9, 'E', &kAcute},
    {0xCA, 'E', &kCircumflex}, {0xCB, 'E', &kDiaeresis}, {0xCC, 'I', &kGrave},
    {0xCD, 'I', &kAcute},     {0xCE, 'I', &kCircumflex}, {0xCF, 'I', &kDiaeresis},
    {0xD1, 'N', &kTilde},     {0xD2, 'O', &kGrave},      {0xD3, 'O', &kAcute},
    {0xD4, 'O', &kCircumflex}, {0xD5, 'O', &kTilde},     {0xD6, 'O', &kDiaeresis},
    {0xD9, 'U', &kGrave},     {0xDA, 'U', &kAcute},      {0xDB, 'U', &kCircumflex},
    {0xDC, 'U', &kDiaeresis}, {0xDD, 'Y', &kAcute},      {0xE0, 'a', &kGrave},
    {0xE1, 'a', &kAcute},     {0xE2, 'a', &kCircumflex}, {0xE3, 'a', &kTilde},
    {0xE4, 'a', &kDiaeresis}, {0xE5, 'a', &kRing},       {0xE7, 'c', &kCedilla},
    {0xE8, 'e', &kGrave},     {0xE9, 'e', &kAcute},      {0xEA, 'e', &kCircumflex},
    {0xEB, 'e', &kDiaeresis}, {0xEC, 'i', &kGrave},      {0xED, 'i', &kAcute},
    {0xEE, 'i', &kCircumflex}, {0xEF, 'i', &kDiaeresis}, {0xF1, 'n', &kTilde},
    {0xF2, 'o', &kGrave},     {0xF3, 'o', &kAcute},      {0xF4, 'o', &kCircumflex},
    {0xF5, 'o', &kTilde},     {0xF6, 'o', &kDiaeresis},  {0xF9, 'u', &kGrave},
    {0xFA, 'u', &kAcute},     {0xFB, 'u', &kCircumflex}, {0xFC, 'u', &kDiaeresis},
    {0xFD, 'y', &kAcute},     {0xFF, 'y', &kDiaeresis},
};

Glyph Compose(const std::vector<uint8_t>& font, const Accented& letter, bool upres) {
  auto base = AsciiGlyph(font, letter.base);
  const bool upper = std::isupper(static_cast<unsigned char>(letter.base)) != 0;
  const bool dotless = letter.base == 'i';
  const Mark* mark = letter.mark;
  if (mark == &kDiaeresis)
    mark = dotless ? &kDiaeresisNarrow : upres ? &kDiaeresisUpres : &kDiaeresis;

  // Twice the column the mark is centered on: the middle of the letter's ink,
  // or of the dotless i's stem.
  int center2 = 0;
  if (dotless) {
    base[0] = base[1] = 0;
    center2 = 7;
  } else {
    uint8_t ink = 0;
    for (uint8_t row : base)
      ink |= row;
    int left = 0, right = kCellWidth - 1;
    while (left < right && !(ink >> left & 1))
      ++left;
    while (right > left && !(ink >> right & 1))
      --right;
    center2 = left + right;
  }

  int height = 0;
  int width = 0;
  while (height < 3 && mark->rows[height]) {
    width = std::max(width, static_cast<int>(std::strlen(mark->rows[height])));
    ++height;
  }
  // The mark's last row sits right over the letter, a blank row apart; a
  // capital's ring would not fit, so that A moves down a row instead.
  int base_top = kBaseTop;
  int mark_top = (upper ? 1 : 3) - height + 1;
  if (mark == &kCedilla) {
    base_top = kBaseTop - 1;
    mark_top = kLatin1Height - height;
  } else if (mark == &kRing && upper) {
    base_top = kBaseTop + 1;
    mark_top = 0;
  }

  Glyph glyph{};
  for (int y = 0; y < kAsciiHeight; ++y) {
    if (base_top + y < kLatin1Height)
      glyph[base_top + y] = base[y];
  }
  const int mark_left = (center2 - (width - 1) + 1) / 2;
  for (int y = 0; y < height; ++y)
    glyph[mark_top + y] |= static_cast<uint8_t>(ParseRow(mark->rows[y]) << mark_left);
  return glyph;
}

// ---------------------------------------------------------------------------
// Everything else, drawn whole
// ---------------------------------------------------------------------------

struct Drawn {
  uint8_t code;
  int top;
  std::array<const char*, kLatin1Height> rows;
};

constexpr Drawn kDrawn[] = {
    // Œ œ
    {0x8C, 3, {"..######", ".#..#...", "#...#...", "#...###.", "#...#...", ".#..#...",
               "..######"}},
    {0x9C, 5, {".##.##..", "#..#..#.", "#..####.", "#..#....", ".##.###."}},
    // ¢ £ ¤ ¥ ¦ §
    {0xA2, 4, {"...#....", "..####..", ".#.#....", ".#.#....", ".#.#....", "..####..",
               "...#...."}},
    {0xA3, 3, {"...##...", "..#..#..", "..#.....", ".####...", "..#.....", "..#.....",
               ".######."}},
    {0xA4, 4, {".#....#.", "..####..", "..#..#..", "..#..#..", "..####..", ".#....#."}},
    {0xA5, 3, {".#...#..", "..#.#...", "...#....", ".#####..", "...#....", ".#####..",
               "...#...."}},
    {0xA6, 3, {"...#....", "...#....", "...#....", "........", "...#....", "...#....",
               "...#...."}},
    {0xA7, 3, {"..###...", ".#......", "..##....", ".#..#...", "..##....", "....#...",
               "..###..."}},
    // ¨ © ª « ¬ ® ¯
    {0xA8, 3, {"..#..#.."}},
    {0xA9, 2, {"..####..", ".#....#.", "#..###.#", "#.#....#", "#.#....#", "#..###.#",
               ".#....#.", "..####.."}},
    {0xAA, 2, {"..##....", "....#...", "..###...", ".#..#...", "..###...", "........",
               ".####..."}},
    {0xAB, 5, {"...#..#.", "..#..#..", ".#..#...", "..#..#..", "...#..#."}},
    {0xAC, 6, {".######.", "......#.", "......#."}},
    {0xAE, 2, {"..####..", ".#....#.", "#.###..#", "#.#..#.#", "#.###..#", "#.#.#..#",
               ".#....#.", "..####.."}},
    {0xAF, 2, {".######."}},
    // ° ± ² ³ ´ µ ¶ · ¸ ¹ º »
    {0xB0, 3, {"..##....", ".#..#...", ".#..#...", "..##...."}},
    {0xB1, 3, {"...#....", "...#....", ".#####..", "...#....", "...#....", "........",
               ".#####.."}},
    {0xB2, 2, {".##.....", "...#....", "..#.....", ".#......", ".###...."}},
    {0xB3, 2, {".###....", "...#....", "..##....", "...#....", ".###...."}},
    {0xB4, 2, {"....#...", "...#...."}},
    {0xB5, 5, {".#....#.", ".#....#.", ".#....#.", ".##..##.", ".#.##.#.", ".#......"}},
    {0xB6, 3, {"..#####.", ".###.#..", ".###.#..", "..##.#..", "...#.#..", "...#.#..",
               "...#.#.."}},
    {0xB7, 6, {"...##...", "...##..."}},
    {0xB8, 9, {"...#....", "..##...."}},
    {0xB9, 2, {"..#.....", ".##.....", "..#.....", "..#.....", ".###...."}},
    {0xBA, 2, {"..##....", ".#..#...", ".#..#...", "..##....", "........", "........",
               ".####..."}},
    {0xBB, 5, {".#..#...", "..#..#..", "...#..#.", "..#..#..", ".#..#..."}},
    // ¼ ½ ¾
    {0xBC, 1, {".#......", "##......", ".#....#.", ".#...#..", "....#...", "...#....",
               "..#..#..", ".#..##..", "...####.", ".....#.."}},
    {0xBD, 1, {".#......", "##......", ".#....#.", ".#...#..", "....#...", "...#....",
               "..#.##..", ".#....#.", ".....#..", "....###."}},
    {0xBE, 1, {"##......", "..#.....", ".#....#.", "..#..#..", "##..#...", "...#....",
               "..#..#..", ".#..##..", "...####.", ".....#.."}},
    // Æ Ð × Ø Þ ß
    {0xC6, 3, {"...#####", "..#.#...", ".#..#...", ".#####..", ".#..#...", ".#..#...",
               ".#..####"}},
    {0xD0, 3, {".####...", ".#...#..", ".#....#.", "###...#.", ".#....#.", ".#...#..",
               ".####..."}},
    {0xD7, 5, {".#...#..", "..#.#...", "...#....", "..#.#...", ".#...#.."}},
    {0xD8, 2, {"......#.", "..####..", ".#...##.", ".#..#.#.", ".#.#..#.", ".##...#.",
               ".#....#.", "#.####.."}},
    {0xDE, 3, {".#......", ".####...", ".#...#..", ".#...#..", ".####...", ".#......",
               ".#......"}},
    {0xDF, 3, {"..###...", ".#...#..", ".#..#...", ".#.##...", ".#...#..", ".#...#..",
               ".#.##..."}},
    // æ ð ÷ ø þ
    {0xE6, 5, {".##.##..", "...#..#.", ".######.", "#..#....", ".##.###."}},
    {0xF0, 3, {"..#.#...", "...#....", "..#.#...", "..####..", ".#....#.", ".#....#.",
               "..####.."}},
    {0xF7, 5, {"...#....", "........", ".#####..", "........", "...#...."}},
    {0xF8, 4, {"......#.", "..####..", ".#..#.#.", ".#.#..#.", ".##...#.", ".#####.."}},
    {0xFE, 3, {".#......", ".#......", ".#.###..", ".##...#.", ".#....#.", ".##...#.",
               ".#.###..", ".#......"}},
};

Glyph Draw(const Drawn& drawn) {
  Glyph glyph{};
  for (int y = 0; drawn.top + y < kLatin1Height && drawn.rows[y]; ++y)
    glyph[drawn.top + y] = ParseRow(drawn.rows[y]);
  return glyph;
}

// ¡ and ¿: ! and ? turned over.
Glyph Inverted(const std::vector<uint8_t>& font, char c, bool mirror) {
  const auto base = AsciiGlyph(font, c);
  int bottom = kAsciiHeight - 1;
  while (bottom > 0 && !base[bottom])
    --bottom;
  Glyph glyph{};
  for (int y = 0; y <= bottom; ++y) {
    uint8_t row = base[bottom - y];
    if (mirror) {
      uint8_t flipped = 0;
      for (int x = 0; x < kCellWidth; ++x)
        flipped |= (row >> x & 1) << (kCellWidth - 1 - x);
      row = flipped;
    }
    glyph[kBaseTop + y] = row;
  }
  return glyph;
}

}  // namespace

std::vector<uint8_t> BuildLatin1Font(const std::vector<uint8_t>& game_font, bool upres) {
  // The font is where both releases have it if the space is blank and the
  // letters are not.
  if (game_font.size() < kAsciiOffset + kAsciiRows * kAsciiHeight * kSheetStride)
    return {};
  const auto space = AsciiGlyph(game_font, ' ');
  const auto a = AsciiGlyph(game_font, 'A');
  const auto m = AsciiGlyph(game_font, 'm');
  auto blank = [](const auto& rows) {
    for (uint8_t row : rows) {
      if (row)
        return false;
    }
    return true;
  };
  if (!blank(space) || blank(a) || blank(m))
    return {};

  std::vector<uint8_t> sheet(kLatin1Size, 0);
  for (const Accented& letter : kAccented)
    Put(sheet, letter.code, Compose(game_font, letter, upres));
  for (const Drawn& drawn : kDrawn)
    Put(sheet, drawn.code, Draw(drawn));
  Put(sheet, 0xA1, Inverted(game_font, '!', false));
  Put(sheet, 0xBF, Inverted(game_font, '?', true));
  return sheet;
}

}  // namespace nocturne
