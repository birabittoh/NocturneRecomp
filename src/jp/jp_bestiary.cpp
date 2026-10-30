// nocturnerecomp - the Librarian's bestiary (怪物図鑑) in Japanese, as JP shows
// it. See jp_text.h.
//
// World's bestiary page is the English one in every language: STRONG / IMMUNE
// / WEAK / ABSORB with element abbreviations in the small font, and the
// description drawn proportionally. JP's (sub_82465630, sub_82465050,
// sub_824642E0) draws four rows of kanji instead, 耐性 / 無効 / 弱点 / 吸収
// and the element of each (聖 闇 炎 ...), into VRAM next to the description,
// shows them with a sprite of their own, and places the rest a little
// differently. The page is built from the same sprite set in both releases;
// JP gives sprite 11 to the kanji rows and sprite 12 to the description
// (World: 11), so its small-font text starts one sprite later.
//
// So in Japanese World's three bestiary functions run as JP's, on World's
// data: the enemy definitions are the same 400 enemies without the name (36
// bytes, JP's are 40 with the name first), and the names and descriptions are
// JP's (jp_text.cpp swaps them in).
#include "generated/nocturnerecomp_init.h"

#include <cstring>
#include <string>
#include <vector>

#include "core/function_hooks.h"
#include "jp/jp_text.h"
#include "jp/jp_text_internal.h"

namespace nocturne {
namespace {

using namespace jp_text;

// sub_82408178(page): the bestiary page's state machine. Its state is the
// halfword at +48 (0 set up, 1 load, ...), the step within state 1 at +50,
// the entry shown at +52, its sprite set's index at +104 and "set up" at +56.
constexpr uint32_t kPageFn = 0x82408178u;
constexpr uint32_t kPageState = 48;
constexpr uint32_t kPageStep = 50;
constexpr uint32_t kPageEntry = 52;
constexpr uint32_t kPageSprites = 104;
constexpr uint32_t kPageFlags = 56;
constexpr uint32_t kPageSetUp = 0x800000;
// sub_82407C28(sprite, page, enemy): fills the page from sprite 10 on.
constexpr uint32_t kDetailFn = 0x82407C28u;
// sub_82406DC0(sprite, list): the list of names, 14 to a screen.
constexpr uint32_t kListFn = 0x82406DC0u;

// The sprite pool: 56-byte sprites, linked by their first word.
constexpr uint32_t kSprites = 0x831751E0u;
constexpr uint32_t kSpriteSize = 56;
constexpr uint32_t kSpriteX = 12;
constexpr uint32_t kSpriteY = 14;
constexpr uint32_t kSpriteUV = 16;       // u8 u, u8 v
constexpr uint32_t kSpritePage = 18;     // texture page
constexpr uint32_t kSpriteW = 28;        // u8
constexpr uint32_t kSpriteH = 29;        // u8
constexpr uint32_t kSpriteClut = 30;
constexpr uint32_t kSpritePriority = 42;
constexpr uint32_t kSpriteHidden = 54;   // 8 hidden, 0 shown
constexpr uint16_t kSmallFontPage = 406;
constexpr uint16_t kTextPage = 417;      // VRAM y 256.., where the rows are drawn

// The bestiary's order (u16 enemy id per entry) and which entries the player
// has seen (a bit each) and has seen drop their second item.
constexpr uint32_t kOrder = 0x82D3D950u;
constexpr uint32_t kSeen = 0x831747D0u;
constexpr uint32_t kSeenDrop = 0x831747F0u;
// The enemy definitions: 36 bytes, u16 fields. JP's have the name first.
constexpr uint32_t kEnemyDefs = 0x831D0CCCu;  // pointer
constexpr uint32_t kEnemyDefSize = 36;
constexpr uint32_t kDefHp = 0;
constexpr uint32_t kDefWeak = 5, kDefStrong = 6, kDefImmune = 7, kDefAbsorb = 8;
constexpr uint32_t kDefLevel = 9, kDefExp = 10, kDefDrop2 = 11, kDefDrop1 = 12;
constexpr uint32_t kEnemyNamesPtr = 0x82DFFBB0u;  // JP's while in Japanese

// World's small-font labels (the same bytes as JP's) and drop names.
constexpr uint32_t kLabelLevel = 0x82D3E6D0u;    // LV
constexpr uint32_t kLabelHp = 0x82D3E6D4u;       // HP
constexpr uint32_t kLabelExp = 0x82D3E6DCu;      // EXP
constexpr uint32_t kLabelDrop = 0x82D3E6E0u;     // DROP ITEMS
constexpr uint32_t kLabelNumber = 0x82D3E6E4u;   // No.
constexpr uint32_t kLabelUnknown = 0x82D3E6E8u;  // ??
constexpr uint32_t kUnknownHp = 0x82214194u;     // ????
constexpr uint32_t kUnknownDrop = 0x8221419Cu;   // ????????
constexpr uint32_t kEquipmentNames = 0x82DFD700u;  // [language] -> {name, desc}[]
constexpr uint32_t kArmorNames = 0x82E45E40u;
constexpr uint32_t kArmorFirst = 0xA9;  // drops >= 0x80 + this are armor

// Guest functions.
constexpr uint32_t kSmallTextFn = 0x824002E8u;  // (sprite, x, y, text, page) -> next sprite
constexpr uint32_t kNumberFn = 0x824039C0u;     // (sprite, value, digits)
constexpr uint32_t kFillFn = 0x82403AC0u;       // (sprite, text, count, 1)
constexpr uint32_t kClearVramFn = 0x82400620u;  // (x, y, w, h)
constexpr uint32_t kGlyphFn = 0x822554A0u;      // (sjis, 1) -> 12x16 4bpp glyph
constexpr uint32_t kGpu = 0x83133B00u;          // its second method uploads to VRAM

// JP's text for the page.
constexpr uint32_t kJpHeaders = 0x82FF12B8u;    // 耐性 無効 弱点 吸収
constexpr uint32_t kJpNone = 0x82FF12E0u;       // 無し
constexpr uint32_t kJpElements = 0x82FF10BCu;   // 聖 闇 炎 雷 氷 水 石 毒 呪 切 物
constexpr uint32_t kJpElementMasks = 0x82FF12C8u;
constexpr uint32_t kElements = 11;
constexpr uint32_t kJpDescriptions = 0x82FF0950u;
constexpr uint32_t kJpOrder = 0x82FF0820u;
constexpr uint32_t kJpUnknownName = 0x82FF1248u;
constexpr uint32_t kJpMiscDrops = 0x82FF1258u;  // drops below 0x80
constexpr uint32_t kMiscDrops = 16;             // the ones any enemy drops
constexpr uint32_t kEntries = 146;
constexpr uint32_t kLineGlyphs = 16;
constexpr uint32_t kGlyphBytes = 96;

struct Text {
  std::vector<std::string> headers;
  std::string none;
  std::vector<std::string> elements;
  std::vector<uint16_t> masks;
  std::vector<std::string> descriptions;  // by World entry
};
Text g_text;
uint32_t g_unknown_name = 0;
uint32_t g_misc_drops = 0;   // [kMiscDrops] -> name
uint32_t g_glyphs = 0;       // guest buffer the glyphs are uploaded from
uint32_t g_glyphs_size = 0;

uint32_t Call(PPCContext& ctx, uint8_t* base, uint32_t fn, std::initializer_list<uint32_t> args) {
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t i = 0;
  for (const uint32_t a : args)
    regs[i++]->u64 = a;
  CallGuest(fn, ctx, base);
  return ctx.r3.u32;
}

uint32_t Next(uint32_t sprite) {
  return sprite ? Load32(sprite) : 0;
}

uint32_t Nth(uint32_t sprite, uint32_t n) {
  while (sprite && n--)
    sprite = Next(sprite);
  return sprite;
}

void Set16(uint32_t sprite, uint32_t field, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(Guest(sprite + field), value);
}

void Set8(uint32_t sprite, uint32_t field, uint8_t value) {
  *Guest(sprite + field) = value;
}

uint16_t Def(uint32_t id, uint32_t field) {
  return Load16(Load32(kEnemyDefs) + kEnemyDefSize * id + 2 * field);
}

// Draws Shift-JIS `text` into VRAM at (x, y), 12 px (3 units) a glyph, as
// JP's sub_824654B8. Returns where the next glyph goes in the buffer.
uint32_t DrawKanji(PPCContext& ctx, uint8_t* base, const std::string& text, uint32_t buffer,
                   uint32_t x, uint32_t y) {
  for (size_t i = 0; i + 1 < text.size(); i += 2) {
    if (buffer + kGlyphBytes > g_glyphs + g_glyphs_size)
      break;
    const uint32_t code = uint8_t(text[i]) << 8 | uint8_t(text[i + 1]);
    const uint32_t glyph = Call(ctx, base, kGlyphFn, {code, 1});
    if (!glyph)
      continue;
    std::memcpy(Guest(buffer), Guest(glyph), kGlyphBytes);
    const uint32_t gpu = Load32(kGpu);
    Call(ctx, base, Load32(Load32(gpu) + 4), {gpu, buffer, x, y, 12, 16, 0});
    buffer += kGlyphBytes;
    x += 3;
  }
  return buffer;
}

// One row of elements, or 無し.
uint32_t DrawElements(PPCContext& ctx, uint8_t* base, uint16_t flags, uint32_t buffer, uint32_t y) {
  if ((flags & 0xFFE0) == 0)
    return DrawKanji(ctx, base, g_text.none, buffer, 8, y);
  uint32_t x = 8;
  for (uint32_t k = 0; k < kElements; ++k) {
    if (flags & g_text.masks[k]) {
      buffer = DrawKanji(ctx, base, g_text.elements[k], buffer, x, y);
      x += 3;
    }
  }
  return buffer;
}

// State 1, step 3 of the page: JP's rows and description, into VRAM.
void DrawRows(PPCContext& ctx, uint8_t* base, uint32_t page) {
  const uint32_t entry = Load16(page + kPageEntry);
  const uint32_t id = Load16(kOrder + 2 * entry);
  Call(ctx, base, kClearVramFn, {0, 384, 256, 96});
  uint32_t buffer = g_glyphs;
  for (uint32_t r = 0; r < 4; ++r)
    buffer = DrawKanji(ctx, base, g_text.headers[r], buffer, 0, 384 + 16 * r);
  buffer = DrawElements(ctx, base, Def(id, kDefStrong), buffer, 384);
  buffer = DrawElements(ctx, base, Def(id, kDefImmune), buffer, 400);
  buffer = DrawElements(ctx, base, Def(id, kDefWeak), buffer, 416);
  buffer = DrawElements(ctx, base, Def(id, kDefAbsorb), buffer, 432);
  // The description: 16 glyphs a line.
  const std::string& text = entry < g_text.descriptions.size() ? g_text.descriptions[entry] : "";
  const size_t line = 2 * kLineGlyphs;
  buffer = DrawKanji(ctx, base, text.substr(0, std::min(line, text.size())), buffer, 0, 448);
  if (text.size() > line)
    DrawKanji(ctx, base, text.substr(line, line), buffer, 0, 464);
}

// After state 0 has set the sprites up: JP's layout of them.
void LayOutSprites(uint32_t page) {
  const uint32_t first = kSprites + kSpriteSize * Load32(page + kPageSprites);
  // The window's two edges (texture coordinates of their quads).
  for (const uint32_t i : {8u, 9u}) {
    const uint32_t s = Nth(first, i);
    if (!s)
      return;
    const uint8_t u = static_cast<uint8_t>(108 * (int(i) - 9) + 0x80);
    const uint8_t u2 = static_cast<uint8_t>(108 * (int(i) - 9) - 20);
    Set8(s, 16, u);
    Set8(s, 40, u);
    Set8(s, 28, u2);
    Set8(s, 52, u2);
  }
  if (const uint32_t portrait = Nth(first, 10))
    Set16(portrait, kSpriteX, 28);
  auto text_sprite = [](uint32_t s, uint16_t x, uint16_t y, uint8_t v, uint8_t w, uint8_t h) {
    if (!s)
      return;
    Set16(s, kSpriteClut, 16);
    Set16(s, kSpritePage, kTextPage);
    Set8(s, kSpriteUV, 0);
    Set8(s, kSpriteUV + 1, v);
    Set8(s, kSpriteW, w);
    Set8(s, kSpriteH, h);
    Set16(s, kSpriteX, x);
    Set16(s, kSpriteY, y);
    Set16(s, kSpritePriority, 18);
    Set16(s, kSpriteHidden, 8);
  };
  text_sprite(Nth(first, 11), 132, 56, 128, 128, 64);  // the rows, VRAM y 384
  text_sprite(Nth(first, 12), 32, 180, 192, 224, 32);  // the description, VRAM y 448
}

// Places `count` small-font sprites from `sprite` in a row at `x`, 8 px apart.
uint32_t Place(uint32_t sprite, uint32_t count, uint16_t x, uint16_t y) {
  for (uint32_t i = 0; i < count && sprite; ++i, x += 8) {
    Set16(sprite, kSpritePage, kSmallFontPage);
    Set16(sprite, kSpriteX, x);
    Set16(sprite, kSpriteY, y);
    sprite = Next(sprite);
  }
  return sprite;
}

uint32_t DropName(uint16_t drop) {
  if (drop < 0x80)
    return drop < kMiscDrops ? Load32(g_misc_drops + 4 * drop) : 0;
  const uint32_t item = drop - 0x80;
  if (item >= kArmorFirst)
    return Load32(Load32(kArmorNames + 4) + 8 * (item - kArmorFirst));
  return Load32(Load32(kEquipmentNames + 4) + 8 * item);
}

// sub_82407C28 in Japanese, as JP's sub_82465050.
void DrawDetail(PPCContext& ctx, uint8_t* base) {
  if (!JpTextShown() || !g_glyphs) {
    CallGuest(kDetailFn, ctx, base);
    return;
  }
  const uint32_t portrait = ctx.r3.u32;
  const uint32_t page = ctx.r4.u32;
  const uint32_t id = ctx.r5.u32 & 0xFFFF;
  const uint32_t entry = Load16(page + kPageEntry);

  Set16(portrait, kSpriteHidden, 0);
  const uint32_t rows = Next(portrait);
  Set16(rows, kSpriteHidden, 0);
  const uint32_t description = Next(rows);
  Set16(description, kSpriteHidden, 0);
  uint32_t s = Next(description);

  auto text = [&](uint32_t at, uint16_t x, uint16_t y, uint32_t str, uint16_t page_id) {
    return Call(ctx, base, kSmallTextFn, {at, x, y, str, page_id});
  };

  s = text(s, 28, 44, Load32(Load32(kEnemyNamesPtr) + 4 * id), kSmallFontPage);
  s = text(s, 180, 44, Load32(kLabelNumber), kSmallFontPage);
  Call(ctx, base, kNumberFn, {s, entry + 1, 3});
  s = Place(s, 3, 204, 44);

  // LV and HP: a digit hidden as a leading blank takes no room.
  auto place = [&](uint32_t d, uint16_t x, uint16_t y) {
    Set16(d, kSpritePage, kSmallFontPage);
    Set16(d, kSpriteX, x);
    Set16(d, kSpriteY, y);
    return Load16(d + kSpriteHidden) != 8;
  };
  s = text(s, 32, 56, Load32(kLabelLevel), kSmallFontPage);
  if (entry)
    Call(ctx, base, kNumberFn, {s, Def(id, kDefLevel), 2});
  else
    Call(ctx, base, kFillFn, {s, Load32(kLabelUnknown), 2, 1});
  {
    const uint32_t d0 = s, d1 = Next(d0);
    place(d1, place(d0, 50, 56) ? 58 : 50, 56);
    s = Next(d1);
  }

  s = text(s, 72, 56, Load32(kLabelHp), kSmallFontPage);
  const int16_t hp = static_cast<int16_t>(Def(id, kDefHp));
  if (hp <= 9999)
    Call(ctx, base, kNumberFn, {s, static_cast<uint32_t>(static_cast<int32_t>(hp)), 4});
  else
    Call(ctx, base, kFillFn, {s, kUnknownHp, 4, 1});
  {
    const uint32_t d0 = s, d1 = Next(d0), d2 = Next(d1), d3 = Next(d2);
    uint16_t x = place(d0, 90, 56) ? 98 : 90;
    if (place(d1, x, 56))
      x += 8;
    if (place(d2, x, 56))
      x += 8;
    place(d3, x, 56);
    s = Next(d3);
  }

  s = text(s, 132, 164, Load32(kLabelExp), kSmallFontPage);
  Call(ctx, base, kNumberFn, {s, Def(id, kDefExp), 4});
  s = Place(s, 4, 156, 164);

  s = text(s, 132, 124, Load32(kLabelDrop), kSmallFontPage);
  const uint32_t drop1 = DropName(Def(id, kDefDrop1));
  s = text(s, 136, 136, drop1 ? drop1 : kUnknownDrop, kSmallFontPage);
  const uint16_t drop2_id = Def(id, kDefDrop2);
  const bool seen = (*Guest(kSeenDrop + (entry >> 3)) >> (entry & 7)) & 1;
  const uint32_t drop2 = (seen || !drop2_id) ? DropName(drop2_id) : 0;
  s = text(s, 136, 148, drop2 ? drop2 : kUnknownDrop, kSmallFontPage);
  for (; s; s = Next(s))
    Set16(s, kSpriteHidden, 8);
  ctx.r3.u64 = 0;
}

// sub_82406DC0 in Japanese, as JP's sub_824642E0: the left column 8 px in,
// JP's mark for an enemy not seen yet.
void DrawList(PPCContext& ctx, uint8_t* base) {
  if (!JpTextShown() || !g_unknown_name) {
    CallGuest(kListFn, ctx, base);
    return;
  }
  uint32_t s = ctx.r3.u32;
  const uint32_t list = ctx.r4.u32;
  uint32_t entry = 2 * Load16(list + 132);
  uint32_t y = 4;
  for (uint32_t i = 0; i < 14; ++i, ++entry) {
    uint32_t x = 120;
    if (!(i & 1)) {
      x = 8;
      y += 12;
    }
    const uint16_t id = entry < kEntries ? Load16(kOrder + 2 * entry) : 0xFFFF;
    if (id == 0xFFFF)
      continue;
    const bool seen = (*Guest(kSeen + (entry >> 3)) >> (entry & 7)) & 1;
    s = seen ? Call(ctx, base, kSmallTextFn,
                    {s, x, y, Load32(Load32(kEnemyNamesPtr) + 4 * id), kSmallFontPage})
             : Call(ctx, base, kSmallTextFn, {s, x, y, g_unknown_name, 401});
  }
  for (; s; s = Next(s))
    Set16(s, kSpriteHidden, 8);
  ctx.r3.u64 = 0;
}

// sub_82408178 in Japanese: JP's layout after state 0, JP's rows at state 1
// step 3; the rest is World's.
void Page(PPCContext& ctx, uint8_t* base) {
  const uint32_t page = ctx.r3.u32;
  if (!JpTextShown() || !g_glyphs) {
    CallGuest(kPageFn, ctx, base);
    return;
  }
  if (Load16(page + kPageState) == 1 && Load16(page + kPageStep) == 3) {
    DrawRows(ctx, base, page);
    Set16(page, kPageStep, 4);
    return;
  }
  const bool set_up = Load32(page + kPageFlags) & kPageSetUp;
  CallGuest(kPageFn, ctx, base);
  if (!set_up && (Load32(page + kPageFlags) & kPageSetUp))
    LayOutSprites(page);
}

}  // namespace

namespace jp_text {

bool InstallJpBestiary(const JpImage& jp, Arena& arena) {
  Text text;
  for (uint32_t r = 0; r < 4; ++r)
    text.headers.push_back(jp.CString(jp.U32(kJpHeaders + 4 * r)));
  text.none = jp.CString(kJpNone);
  for (uint32_t k = 0; k < kElements; ++k) {
    text.elements.push_back(jp.CString(jp.U32(kJpElements + 4 * k)));
    text.masks.push_back(jp.U16(kJpElementMasks + 2 * k));
  }
  for (uint32_t entry = 0; entry < kEntries; ++entry) {
    const uint16_t id = Load16(kOrder + 2 * entry);
    uint32_t jp_entry = 0;
    while (jp_entry < kEntries && jp.U16(kJpOrder + 2 * jp_entry) != id)
      ++jp_entry;
    text.descriptions.push_back(
        jp_entry < kEntries ? jp.CString(jp.U32(kJpDescriptions + 4 * jp_entry)) : "");
  }
  std::vector<uint32_t> drops;
  for (uint32_t i = 0; i < kMiscDrops; ++i)
    drops.push_back(arena.Put(jp.MenuString(jp.U32(kJpMiscDrops + 4 * i)), false));
  g_unknown_name = arena.Put(jp.MenuString(kJpUnknownName), false);
  g_misc_drops = arena.PutArray(drops);
  g_glyphs_size = 128 * kGlyphBytes;
  g_glyphs = arena.Put(std::string(g_glyphs_size, '\0'), false);
  g_text = std::move(text);
  for (const uint32_t d : drops) {
    if (!d)
      return false;
  }
  return g_unknown_name && g_misc_drops && g_glyphs;
}

}  // namespace jp_text
}  // namespace nocturne

// Hooks (config/nocturnerecomp_jp_text_hooks.toml).

bool JpBestiaryPage(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kPageFn, &nocturne::Page);
}

bool JpBestiaryDetail(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kDetailFn, &nocturne::DrawDetail);
}

bool JpBestiaryList(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kListFn, &nocturne::DrawList);
}

// sub_82408178, state 2 (the window opening): JP's window is narrower.
void JpBestiaryWindowWidth(PPCRegister& r6) {
  if (nocturne::JpTextShown())
    r6.u64 = 0x74;
}
