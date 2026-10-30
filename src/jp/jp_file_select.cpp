// nocturnerecomp - the PS1 file select (SEL, sub_82397068) in Japanese, as JP
// shows it. See jp_text.h.
//
// World reworded this screen for the Xbox ("Memory Unit") and dropped its
// language branch, so it is English in every language. JP's is Japanese: its
// messages, the menu options, each slot's status, and the area the slot was
// saved in, as kanji text where World draws a two-line English name in the
// small font. The drawing routines are the same in both releases (World's
// sub_8239AEC8 and sub_8239ADB0 take Shift-JIS), so JP's screen is World's
// with JP's strings put through them.
#include "generated/nocturnerecomp_init.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/function_hooks.h"
#include "jp/jp_text.h"
#include "jp/jp_text_internal.h"

namespace nocturne {
namespace {

using namespace jp_text;

// sub_8239B508(text, line): draws a line of text (sub_8239AEC8) unless that
// line already shows it; the cache holds the pointer last drawn on each line.
constexpr uint32_t kDrawLineFn = 0x8239B508u;
// sub_8239AEC8(text, line): ASCII or Shift-JIS, proportional.
constexpr uint32_t kDrawTextFn = 0x8239AEC8u;
// sub_8239ADB0(text, line): Shift-JIS only, 12 px a glyph.
constexpr uint32_t kDrawKanjiFn = 0x8239ADB0u;
// sub_8239ACF8(line): blanks a line and its cache entry.
constexpr uint32_t kClearLineFn = 0x8239ACF8u;
constexpr uint32_t kLineCache = 0x82F2CE08u;  // u32[line]
constexpr uint32_t kTextX = 0x82F2CDFCu;
constexpr uint32_t kTextY = 0x82F2CE00u;
constexpr uint32_t kTextOrigin = 384;
// The blank World draws on an empty line.
constexpr uint32_t kBlank = 0x82211AC4u;

// The messages, two lines (4 and 5) under the slots. World draws line 4, then
// usually line 5 right after; JP's wording splits differently, so a message
// is told apart by both lines. 0 for World's line 5: it draws line 4 only.
// 0 for JP's line 2: blank.
struct Message {
  uint32_t world4;
  uint32_t world5;
  uint32_t jp1;
  uint32_t jp2;
};
constexpr Message kMessages[] = {
    {0x82211DC0u, 0x82211DD8u, 0x82266B24u, 0x82266B44u},  // Checking Memory Unit.
    {0x82211DF4u, 0x82211E10u, 0x82266B64u, 0x82266B7Cu},  // You won't be able to save...
    {0x82211E2Cu, 0x822021F9u, 0x82266B9Cu, 0x82266BB4u},  // Select file to be loaded.
    {0x82211E48u, 0x82211E5Cu, 0x82266BC8u, 0x82266BE0u},  // Press Start button...
    {0x82211E70u, 0x82211E80u, 0x82266BFCu, 0x82266BE0u},  // Select New Game...
    {0x82211E9Cu, 0x82211EBCu, 0x82266C18u, 0x82266C38u},  // Input "RICHTER"...
    {0x82211ED4u, 0x82211EE4u, 0x82266C54u, 0},            // No data found...
    {0x82211EFCu, 0x82211F0Cu, 0x82266C6Cu, 0x82266BB4u},  // Select the file you wish to rename.
    {0x82211F24u, 0x82211DD8u, 0x82266C88u, 0},            // Changing Name.
    {0x82211F34u, 0, 0x82266CA0u, 0},                      // Name changed.
    {0x82211F44u, 0, 0x82266CB8u, 0},                      // Loading error.
    {0x82211F54u, 0, 0x82266CCCu, 0},                      // Save error.
    {0x82211F64u, 0x82211F70u, 0x82266CE0u, 0x82266BB4u},  // Select file to be copied.
    {0x82211F64u, 0x82211FDCu, 0x82266D70u, 0x82266BB4u},  // Select file to be erased.
    {0x82211F80u, 0x82211F94u, 0x82266CF8u, 0},            // Where do you want to copy to?
    {0x82211FA4u, 0, 0x82266D10u, 0x82266D2Cu},            // OK to overwrite data?
    {0x82211FBCu, 0x82211DD8u, 0x82266D40u, 0},            // Copying data.
    {0x82211FCCu, 0, 0x82266D5Cu, 0},                      // File copied.
    {0x82211FECu, 0, 0x82266D84u, 0},                      // Is it OK to erase file?
    {0x82212008u, 0, 0x82266DA0u, 0},                      // File erased.
    {0x82212018u, 0, 0x82266DB0u, 0},                      // Delete error.
    {0x82212028u, 0x82211DD8u, 0x82266DC8u, 0},            // Loading Memory Unit.
    {0x82212040u, 0, 0x82266DE4u, 0},                      // Version error.
    {0x82212050u, 0x82212064u, 0x82266DFCu, 0x82266E14u},  // Format Memory Unit in slot 1?
    {0x82212050u, 0x822120A0u, 0x82266E60u, 0x82266E14u},  // Format Memory Unit in slot 2?
    {0x82212074u, 0, 0x82266E2Cu, 0},                      // Formatting completed.
    {0x8221208Cu, 0, 0x82266E44u, 0},                      // Formatting error.
};

// The menu (sub_82395798, line 9): World's option, JP's.
struct Option {
  uint32_t world;
  uint32_t jp;
};
constexpr Option kOptions[] = {
    {0x82211AA4u, 0x8226695Cu},  // － New Game －
    {0x82211A90u, 0x82266948u},  // － Change Name －
    {0x82211A80u, 0x82266934u},  // － Copy File －
    {0x82211A6Cu, 0x82266920u},  // － Erase File －
};

// A slot's three lines (sub_82394FF8(card, slot, line); JP's sub_823F2FC8):
// the player's name, then the area (World: blank), then the status. The
// per-slot records are 234 words a card.
constexpr uint32_t kSlotsPerCard = 234;
constexpr uint32_t kSlotName = 0x8314C78Cu;       // 8 bytes a slot, 936 a card
constexpr uint32_t kSlotArea = 0x8314C4F8u;       // u32: area id
constexpr uint32_t kSlotRichter = 0x8314C750u;    // u32: playing as Richter
constexpr uint32_t kSlotFlags = 0x8314C714u;      // u32: 1 cleared, 2 another play
constexpr uint32_t kNameCodes = 0x82D591E0u;      // 3-byte entries, one per name glyph
constexpr uint32_t kNameCodesEnd = 0x82D59240u;
constexpr uint32_t kNameStrings = 0x82D5C5F8u;    // [code index] -> string, 32 for none
// World's Japanese area names (the same as JP's 0x830B6858), which World never
// reads: JP draws them on the slot's second line.
constexpr uint32_t kAreaNames = 0x82E03FF0u;
constexpr uint32_t kJpRichter = 0x82266A70u;
constexpr uint32_t kJpClear = 0x82266A7Cu;
constexpr uint32_t kJpAnotherPlay = 0x82266A88u;

// The copies of JP's strings, by JP address.
struct Copy {
  uint32_t jp;
  uint32_t guest;
};
std::vector<Copy> g_copies;
uint32_t g_blank = 0;
uint32_t g_name_buffer = 0;  // the slot name being put together

// World's line 4 as last drawn, to tell its messages apart on line 5.
uint32_t g_world_line4 = 0;

uint32_t Copied(uint32_t jp) {
  if (!jp)
    return g_blank;
  for (const Copy& c : g_copies) {
    if (c.jp == jp)
      return c.guest;
  }
  return g_blank;
}

void Call2(uint32_t fn, PPCContext& ctx, uint8_t* base, uint32_t a, uint32_t b) {
  ctx.r3.u64 = a;
  ctx.r4.u64 = b;
  CallGuest(fn, ctx, base);
}

void DrawLine(PPCContext& ctx, uint8_t* base, uint32_t text, uint32_t line) {
  Call2(kDrawLineFn, ctx, base, text, line);
}

// sub_8239B508 in Japanese: JP's wording for the messages.
void DrawMessageLine(PPCContext& ctx, uint8_t* base) {
  const uint32_t text = ctx.r3.u32;
  const uint32_t line = ctx.r4.u32;
  if (!JpTextShown() || (line != 4 && line != 5)) {
    CallGuest(kDrawLineFn, ctx, base);
    return;
  }
  if (line == 4) {
    g_world_line4 = text;
    // The first message that starts this way; line 5 corrects it if needed.
    for (const Message& m : kMessages) {
      if (m.world4 != text)
        continue;
      DrawLine(ctx, base, Copied(m.jp1), 4);
      if (!m.world5)
        DrawLine(ctx, base, Copied(m.jp2), 5);
      return;
    }
  } else {
    for (const Message& m : kMessages) {
      if (m.world4 != g_world_line4 || m.world5 != text)
        continue;
      DrawLine(ctx, base, Copied(m.jp1), 4);
      DrawLine(ctx, base, Copied(m.jp2), 5);
      return;
    }
  }
  CallGuest(kDrawLineFn, ctx, base);
}

// Draws `text` with `fn` on `line` unless the line already shows it, as
// World's sub_82394FF8 does for its own lines.
void DrawSlotLine(PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t text, uint32_t line) {
  Store32(kTextX, kTextOrigin);
  Store32(kTextY, 0);
  if (Load32(kLineCache + 4 * line) == text)
    return;
  ctx.r3.u64 = line;
  CallGuest(kClearLineFn, ctx, base);
  Store32(kLineCache + 4 * line, text);
  Store32(kTextX, kTextOrigin);
  Store32(kTextY, 0);
  Call2(fn, ctx, base, text, line);
}

// sub_82394FF8 in Japanese, as JP's sub_823F2FC8: the area name on the
// second line, and JP's words for the status.
void DrawSlot(PPCContext& ctx, uint8_t* base) {
  if (!JpTextShown() || !g_name_buffer) {
    CallGuest(0x82394FF8u, ctx, base);
    return;
  }
  const uint32_t card = ctx.r3.u32;
  const uint32_t slot = ctx.r4.u32;
  const uint32_t line = ctx.r5.u32;
  const uint32_t record = kSlotsPerCard * card + slot;

  // The name, glyph by glyph, as World builds it.
  std::string name;
  for (uint32_t i = 0; i < 8; ++i) {
    const uint8_t code = *Guest(kSlotName + 936 * card + 10 * slot + i);
    uint32_t index = 0;
    uint32_t at = kNameCodes;
    while (at < kNameCodesEnd && *Guest(at) != code) {
      at += 3;
      ++index;
    }
    if (at >= kNameCodesEnd)
      index = 32;
    for (uint32_t s = Load32(kNameStrings + 4 * index); *Guest(s); ++s)
      name.push_back(static_cast<char>(*Guest(s)));
  }
  name.resize(std::min<size_t>(name.size(), 111));
  std::memcpy(Guest(g_name_buffer), name.c_str(), name.size() + 1);
  Store32(kTextX, kTextOrigin);
  Store32(kTextY, 0);
  Call2(kDrawKanjiFn, ctx, base, g_name_buffer, line);

  DrawSlotLine(ctx, base, kDrawKanjiFn, Load32(kAreaNames + 4 * Load32(kSlotArea + 4 * record)),
               line + 1);

  if (Load32(kSlotRichter + 4 * record)) {
    DrawSlotLine(ctx, base, kDrawKanjiFn, Copied(kJpRichter), line + 2);
  } else {
    const uint32_t flags = Load32(kSlotFlags + 4 * record);
    if (flags & 1)
      DrawSlotLine(ctx, base, kDrawKanjiFn, Copied(kJpClear), line + 2);
    else
      DrawLine(ctx, base, (flags & 2) ? Copied(kJpAnotherPlay) : kBlank, line + 2);
  }
}

// sub_82395228: World's small-font English area name beside the slot. JP has
// none (its name is the slot's second line).
void DrawSlotArea(PPCContext& ctx, uint8_t* base) {
  if (!JpTextShown())
    CallGuest(0x82395228u, ctx, base);
}

}  // namespace

namespace jp_text {

bool InstallJpFileSelect(const JpImage& jp, Arena& arena) {
  g_copies.clear();
  auto keep = [&](uint32_t address) {
    if (!address)
      return;
    for (const Copy& c : g_copies) {
      if (c.jp == address)
        return;
    }
    g_copies.push_back({address, arena.PutString(jp, address)});
  };
  for (const Message& m : kMessages) {
    keep(m.jp1);
    keep(m.jp2);
  }
  for (const Option& o : kOptions)
    keep(o.jp);
  keep(kJpRichter);
  keep(kJpClear);
  keep(kJpAnotherPlay);
  g_blank = arena.Put("", true);
  g_name_buffer = arena.Put(std::string(112, '\0'), false);
  for (const Copy& c : g_copies) {
    if (!c.guest)
      return false;
  }
  return g_blank && g_name_buffer;
}

}  // namespace jp_text
}  // namespace nocturne

// Hooks (config/nocturnerecomp_jp_text_hooks.toml).

bool JpFileSelectDrawLine(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kDrawLineFn, &nocturne::DrawMessageLine);
}

bool JpFileSelectDrawSlot(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, 0x82394FF8u, &nocturne::DrawSlot);
}

bool JpFileSelectDrawSlotArea(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, 0x82395228u, &nocturne::DrawSlotArea);
}

// sub_8239AEC8, on entry: the menu's options in JP's words.
void JpFileSelectMenuOption(PPCRegister& r3) {
  if (!nocturne::JpTextShown())
    return;
  for (const auto& option : nocturne::kOptions) {
    if (r3.u32 == option.world) {
      r3.u64 = nocturne::Copied(option.jp);
      return;
    }
  }
}
