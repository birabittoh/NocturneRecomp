#include "generated/nocturnerecomp_init.h"

#include "jp/jp_text.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

#include "core/target.h"
#include "core/function_hooks.h"
#include "jp/jp_text_internal.h"
#include "jp/jp_voices.h"

REXCVAR_DEFINE_STRING(prologue, "auto", "Game",
                      "Whose prologue to scroll in Japanese when JP is installed: world (World's "
                      "Japanese text), jp (JP's, the original English one), or auto (JP's)")
    .allowed({"auto", "jp", "world"});

namespace nocturne {
namespace jp_text {

void CallGuest(uint32_t address, PPCContext& ctx, uint8_t* base) {
  fnhook::CallOriginal(address, ctx, base);
}

}  // namespace jp_text

namespace {

using namespace jp_text;

// The PS1 side's language index (set by sub_824FAEF8); 1 is Japanese.
constexpr uint32_t kPs1LanguageAddr = 0x8313BC20u;
constexpr uint32_t kPs1Japanese = 1;

// ---------------------------------------------------------------------------
// The glyphs. Every Shift-JIS text path draws from the 16x15 "BIOS font"
// table (sub_8224ADC0 and sub_8224A7A8: symbols from 0x82E06078, kanji rows
// from 0x82E0C1C8), 30 bytes a glyph. It is JP's apart from three glyphs.
// ---------------------------------------------------------------------------

constexpr uint32_t kGlyphSize = 30;
struct GlyphCopy {
  uint32_t world;
  uint32_t jp;
};
constexpr GlyphCopy kGlyphs[] = {
    // 亜 (0x889F): World's holds the bottom of the glyph only, so it draws as
    // a bar ("亜空間" reads "一空間").
    {0x82E0CCEAu, 0x830BF59Au},
    // … (0x8163): JP's dots are centred, World's sit on the baseline.
    {0x82E06492u, 0x830B8CE2u},
    // JP has one symbol World lacks, ↘ (0x84BF), used in Skill of Wolf's
    // description. World has no room for it and its own copy of that text
    // spells the arrow 0xE8FC (顰), which no other text uses; JP's arrow goes
    // in that slot.
    {0x82E3A7D0u, 0x830BC630u},
};

// ---------------------------------------------------------------------------
// Japanese slots: read only while the PS1 side is in Japanese, so they are
// written once.
// ---------------------------------------------------------------------------

// Soul of Bat's name, the relics table's (0x82E04CD0) Japanese array, entry 0.
// World's is the single kanji 烹.
constexpr uint32_t kSoulOfBatNameSlot = 0x82E04BE0u;
constexpr uint32_t kJpSoulOfBatName = 0x82254DECu;

// The Librarian's menu, Japanese (0x82D3DDE8, from off_82D3E010[1]): a column
// byte, then Shift-JIS. World's entry 3 is サウンドテスト, seven glyphs in a
// row buffer of 72 px (sub_823FFE30), so it overflows onto itself. JP's has
// 自由会話 there, a feature World swapped for the sound test, and an unused
// 音楽鑑賞 (music appreciation), which fits. Entry 4 is JP's wording for
// leaving. JP's column byte is 2; World centres its four-glyph entries at 3.
constexpr uint32_t kLibrarianSoundTestSlot = 0x82D3DDF4u;
constexpr uint32_t kJpLibrarianMusic = 0x822694C0u;
constexpr uint32_t kLibrarianLeaveSlot = 0x82D3DDF8u;
constexpr uint32_t kJpLibrarianLeave = 0x822694D8u;
constexpr uint8_t kLibrarianColumn = 3;

// The Librarian's shop. World's Japanese arrays alias the English ones for
// the book and map descriptions (off_82D3E040), the Jewel of Open's name
// (off_82D3E070), the stock labels (off_82D3E0A0); its book and map names
// (off_82D3E088) are worded differently. JP's: Shift-JIS descriptions, names
// in the PS1 menu encoding (0xFF 0x00 ends them), and labels in plain
// half-width katakana that need that ending added.
struct ShopArray {
  uint32_t world_slot;
  uint32_t jp_array;
  uint32_t count;
  enum Kind { kShiftJis, kMenu, kKatakana } kind;
};
constexpr ShopArray kShopArrays[] = {
    {0x82D3E044u, 0x82FF1080u, 6, ShopArray::kShiftJis},
    {0x82D3E08Cu, 0x82FF10A4u, 6, ShopArray::kMenu},
    {0x82D3E074u, 0x82FF10A0u, 1, ShopArray::kMenu},
    {0x82D3E0A4u, 0x82FF0FF8u, 4, ShopArray::kKatakana},
};

// ---------------------------------------------------------------------------
// Shared slots: World reads them in every language, so they hold JP's text
// only while the PS1 side is in Japanese.
// ---------------------------------------------------------------------------

// Enemy names (Faerie Scroll pop-ups, sub_82347BB0/sub_82348B60; the
// bestiary): World reads the English array through this pointer whatever the
// language. JP keeps them in its 40-byte EnemyDefs, the name first; World's
// 36-byte ones are the same 400 enemies without it.
constexpr uint32_t kEnemyNamesPtr = 0x82DFFBB0u;
constexpr uint32_t kJpEnemyDefs = 0x830F3130u;
constexpr uint32_t kJpEnemyDefSize = 40;
constexpr uint32_t kEnemies = 400;

// Bestiary descriptions (sub_82408178, drawn by sub_823FFA50): English in
// every language. JP's are Shift-JIS, two lines of 16 glyphs padded with
// 　 (sub_82465630); World's renderer breaks lines at '@'. The two bestiaries
// list the same enemies in a different order.
constexpr uint32_t kBestiaryDescriptions = 0x82D3DA80u;
constexpr uint32_t kBestiaryOrder = 0x82D3D950u;  // u16 enemy id per slot
constexpr uint32_t kJpBestiaryDescriptions = 0x82FF0950u;
constexpr uint32_t kJpBestiaryOrder = 0x82FF0820u;
constexpr uint32_t kBestiarySlots = 146;
constexpr uint32_t kBestiaryLineGlyphs = 16;

// Tactics (戦術指南) boss names (sub_82406118): English in every language, 29
// entries in JP's order. The array runs on into the element abbreviations.
constexpr uint32_t kTacticsNames = 0x82D3E420u;
constexpr uint32_t kJpTacticsNames = 0x82FF10E8u;
constexpr uint32_t kTacticsBosses = 29;

// The status screen (sub_82243EC0) draws its labels from the English menu
// array in every language. JP draws two of them differently: "BEAT" for
// KILLS and its own katakana for ALUCART (Alucard's status when he has the
// Alucart set). Both are in World's Japanese array already.
constexpr uint32_t kMenuTable = 0x82E00A80u;  // [language] -> array of string pointers
constexpr uint32_t kMenuKills = 11;
constexpr uint32_t kMenuAlucart = 45;

// Voice cues in World's Japanese scripts (`09 hi lo 0A` plays voice
// (hi << 4 | lo) - 768): two lines reuse the English script's cue, which is a
// different recording in JP's banks. Only while JP's voices play.
struct ByteFix {
  uint32_t address;
  uint8_t world;
  uint8_t jp;
};
constexpr ByteFix kVoiceCues[] = {
    {0x82CF2148u, 0xF0, 0xF8},  // Richter: 俺としたことが…。
    {0x82CEBF96u, 0x01, 0x0A},  // Shaft: なぜ、奴を城主として祭り上げた？
};

// The prologue scroll (sub_823A6F38, set up by sub_823A6CA8 with the text).
// In Japanese World scrolls a Japanese translation of its own, through the
// Shift-JIS path; JP's release scrolls the original PS1 English text, through
// the one-byte path, from column 523. The text is runs of characters, `01 n`
// (next line, n columns in), `02 n` (next paragraph) and a 00 at the end.
constexpr uint32_t kJpPrologue = 0x822671A8u;
constexpr uint32_t kPrologueColumn = 0x82F2F908u;  // u16
constexpr uint16_t kJpPrologueColumn = 523;

// ---------------------------------------------------------------------------

struct Swap {
  uint32_t address;
  uint32_t world;
  uint32_t jp;
};

bool g_active = false;
std::vector<Swap> g_swaps;
uint32_t g_jp_prologue = 0;
uint32_t g_applied_language = ~0u;
int g_applied_voices = -1;

// JP's prologue: its commands take an argument byte, which can be 00.
std::string Prologue(const JpImage& jp) {
  std::string out;
  for (uint32_t at = kJpPrologue; jp.Has(at, 2);) {
    const uint8_t c = jp.Bytes(at, 1)[0];
    out.push_back(static_cast<char>(c));
    ++at;
    if (c == 0)
      break;
    if (c == 0x01 || c == 0x02) {
      out.push_back(static_cast<char>(jp.Bytes(at, 1)[0]));
      ++at;
    }
  }
  return out;
}

// JP's bestiary description, laid out for World's renderer: JP draws the
// first 16 glyphs on line 1 and the next 16 on line 2, padding a short line
// with 　; World breaks at '@'.
std::string BestiaryDescription(const std::string& jp) {
  std::vector<std::string> glyphs;
  for (size_t i = 0; i < jp.size();) {
    const size_t n = IsLeadByte(static_cast<uint8_t>(jp[i])) && i + 1 < jp.size() ? 2 : 1;
    glyphs.push_back(jp.substr(i, n));
    i += n;
  }
  auto line = [&](size_t first) {
    size_t last = std::min(glyphs.size(), first + kBestiaryLineGlyphs);
    while (last > first && glyphs[last - 1] == "\x81\x40")  //
      --last;
    std::string out;
    for (size_t i = first; i < last; ++i)
      out += glyphs[i];
    return out;
  };
  std::string out = line(0);
  const std::string second = line(kBestiaryLineGlyphs);
  if (!second.empty())
    out += "@" + second;
  return out;
}

void ApplySwaps(bool japanese) {
  for (const Swap& s : g_swaps)
    Store32(s.address, japanese ? s.jp : s.world);
}

void ApplyVoiceCues(bool jp_voices) {
  for (const ByteFix& fix : kVoiceCues) {
    const uint8_t value = jp_voices ? fix.jp : fix.world;
    WriteGuest(fix.address, &value, 1);
  }
}

}  // namespace

bool InstallJpText(rex::Runtime* runtime) {
  if (!runtime || JpDir().empty())
    return false;
  JpImage jp;
  if (!jp.Load(JpDir() / "default.xex")) {
    REXLOG_WARN("jp text: can't read JP's default.xex; Japanese stays World's");
    return false;
  }
  Arena arena;
  if (!arena.Allocate()) {
    REXLOG_ERROR("jp text: can't allocate 0x{:X} bytes", kArenaSize);
    return false;
  }

  // Japanese slots, written once everything has been copied.
  std::vector<std::pair<uint32_t, uint32_t>> slots;
  slots.emplace_back(kSoulOfBatNameSlot, arena.Put(jp.CString(kJpSoulOfBatName), true));
  for (const auto& [slot, source] : {std::pair{kLibrarianSoundTestSlot, kJpLibrarianMusic},
                                     std::pair{kLibrarianLeaveSlot, kJpLibrarianLeave}}) {
    std::string entry = jp.CString(source);
    if (!entry.empty())
      entry[0] = static_cast<char>(kLibrarianColumn);
    slots.emplace_back(slot, arena.Put(entry, true));
  }
  for (const ShopArray& a : kShopArrays) {
    std::vector<uint32_t> entries;
    for (uint32_t i = 0; i < a.count; ++i) {
      const uint32_t source = jp.U32(a.jp_array + 4 * i);
      switch (a.kind) {
        case ShopArray::kShiftJis:
          entries.push_back(arena.Put(jp.CString(source), true));
          break;
        case ShopArray::kMenu:
          entries.push_back(arena.Put(jp.MenuString(source), false));
          break;
        case ShopArray::kKatakana:
          entries.push_back(arena.Put(jp.CString(source) + "\xFF", true));
          break;
      }
    }
    slots.emplace_back(a.world_slot, arena.PutArray(entries));
  }

  const uint32_t prologue = arena.Put(Prologue(jp), false);
  if (!InstallJpFileSelect(jp, arena) || !InstallJpBestiary(jp, arena)) {
    REXLOG_ERROR("jp text: can't copy the file select's or the bestiary's text; Japanese stays "
                 "World's");
    return false;
  }

  // Shared slots.
  g_swaps.clear();
  {
    std::vector<uint32_t> names;
    for (uint32_t id = 0; id < kEnemies; ++id)
      names.push_back(arena.Put(jp.MenuString(jp.U32(kJpEnemyDefs + kJpEnemyDefSize * id)), false));
    g_swaps.push_back({kEnemyNamesPtr, Load32(kEnemyNamesPtr), arena.PutArray(names)});
  }
  size_t unmatched = 0;
  for (uint32_t slot = 0; slot < kBestiarySlots; ++slot) {
    const uint16_t id = Load16(kBestiaryOrder + 2 * slot);
    uint32_t jp_slot = 0;
    while (jp_slot < kBestiarySlots && jp.U16(kJpBestiaryOrder + 2 * jp_slot) != id)
      ++jp_slot;
    const uint32_t address = kBestiaryDescriptions + 4 * slot;
    if (jp_slot == kBestiarySlots) {
      ++unmatched;
      continue;
    }
    const std::string text =
        BestiaryDescription(jp.CString(jp.U32(kJpBestiaryDescriptions + 4 * jp_slot)));
    g_swaps.push_back({address, Load32(address), arena.Put(text, true)});
  }
  for (uint32_t k = 0; k < kTacticsBosses; ++k) {
    const uint32_t address = kTacticsNames + 4 * k;
    g_swaps.push_back(
        {address, Load32(address), arena.Put(jp.MenuString(jp.U32(kJpTacticsNames + 4 * k)), false)});
  }
  for (const uint32_t index : {kMenuKills, kMenuAlucart}) {
    const uint32_t address = Load32(kMenuTable) + 4 * index;
    g_swaps.push_back({address, Load32(address), Load32(Load32(kMenuTable + 4) + 4 * index)});
  }

  if (arena.full()) {
    REXLOG_ERROR("jp text: 0x{:X} bytes are not enough; Japanese stays World's", kArenaSize);
    g_swaps.clear();
    return false;
  }
  if (unmatched)
    REXLOG_WARN("jp text: {} bestiary entr(ies) have no JP description", unmatched);

  for (const GlyphCopy& g : kGlyphs) {
    if (const uint8_t* glyph = jp.Bytes(g.jp, kGlyphSize))
      WriteGuest(g.world, glyph, kGlyphSize);
  }
  for (const auto& [slot, value] : slots)
    Store32(slot, value);
  g_jp_prologue = prologue;
  g_active = true;
  REXLOG_INFO("jp text: JP's Japanese text in place (0x{:X} bytes, {} shared slots)", arena.used(),
              g_swaps.size());
  return true;
}

void ApplyPendingJpText() {
  if (!g_active)
    return;
  const uint32_t language = Load32(kPs1LanguageAddr);
  if (language != g_applied_language) {
    ApplySwaps(language == kPs1Japanese);
    g_applied_language = language;
  }
  const int voices = JpVoicesPlaying() ? 1 : 0;
  if (voices != g_applied_voices) {
    ApplyVoiceCues(voices);
    g_applied_voices = voices;
  }
}

bool JpTextShown() {
  return g_active && Load32(kPs1LanguageAddr) == kPs1Japanese;
}

bool JpPrologueAvailable() {
  return g_jp_prologue != 0;
}

bool JpPrologueShown() {
  return g_jp_prologue && JpTextShown() && REXCVAR_GET(prologue) != "world";
}

}  // namespace nocturne

// ---------------------------------------------------------------------------
// Hooks (config/nocturnerecomp_jp_text_hooks.toml)
// ---------------------------------------------------------------------------

// sub_823A6CA8, before it keeps the text it was given (r3): JP's prologue
// instead of World's Japanese one.
void JpTextPrologueText(PPCRegister& r3) {
  if (nocturne::JpPrologueShown())
    r3.u64 = nocturne::g_jp_prologue;
}

// sub_823A6CA8, after it picks the language's starting column: JP's.
void JpTextPrologueColumn() {
  if (nocturne::JpPrologueShown())
    rex::memory::store_and_swap<uint16_t>(nocturne::jp_text::Guest(nocturne::kPrologueColumn),
                                          nocturne::kJpPrologueColumn);
}

// sub_823A6F38, at its two `language == 1` tests before drawing a character:
// JP's English text takes the one-byte path.
void JpTextPrologueLanguage(PPCRegister& r) {
  if (nocturne::JpPrologueShown())
    r.u64 = 0;
}
