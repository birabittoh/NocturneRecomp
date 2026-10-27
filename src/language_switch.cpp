// nocturnerecomp - see language_switch.h.
#include "language_switch.h"

#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "function_hooks.h"
#include "help_options_menu.h"
#include "native_options.h"
#include "settings.h"

namespace nocturne {
namespace {

using rex::X_STATUS;

constexpr const char* kLanguageCvar = "user_language";

// The string table: the loaded block, its entry array (block + 4) and the
// entry count (the block's first word).
constexpr uint32_t kStringTablePtrAddr = 0x82E61850u;
constexpr uint32_t kStringEntriesPtrAddr = 0x82E61854u;
constexpr uint32_t kStringCountAddr = 0x82E61858u;
constexpr uint32_t kEntrySize = 12;
constexpr uint32_t kEntryText = 0;
constexpr uint32_t kEntryFontName = 4;
constexpr uint32_t kEntryScale = 8;

// The current font object, and the function that creates one from a file in
// d:\media and stores it there: (file_name).
constexpr uint32_t kFontPtrAddr = 0x82E7A590u;
constexpr uint32_t kLoadFontFnAddr = 0x825CEC78u;

// The font file names, from the boot loader's own table.
constexpr uint32_t kFontEnNameAddr = 0x8200B968u;
constexpr uint32_t kFontJaNameAddr = 0x8200B95Cu;
constexpr uint32_t kFontKoNameAddr = 0x8200B8D8u;
constexpr uint32_t kFontChNameAddr = 0x8200B8B4u;

// The PS1 side's language index (set by sub_824FAEF8).
constexpr uint32_t kPs1LanguageAddr = 0x8313BC20u;

constexpr uint32_t kAllocFnAddr = 0x82576950u;  // (size) -> pointer

// A text widget's own copy of its text (wcsncpy'd in, up to 0x800 chars), its
// scale (current and target; the by-id setter copies the entry's scale into
// both), and the halfword both text setters clear after copying.
constexpr uint32_t kWidgetText = 572;
constexpr uint32_t kWidgetTextChars = 0x800;
constexpr uint32_t kWidgetScale = 536;
constexpr uint32_t kWidgetTargetScale = 540;
constexpr uint32_t kWidgetTextReset = 0x123A;

// A widget's position, and the x the game centres titles on.
constexpr uint32_t kWidgetX = 4;
constexpr int32_t kScreenCenterX = 0x140;

constexpr uint32_t kTextWidthFnAddr = 0x825CF008u;  // (text_widget) -> pixels

// Option lists: item count, the item widgets (list+0x288, i.e. slot 0xA2),
// and an item's inner text widget.
constexpr uint32_t kListItemCount = 0x220;
constexpr uint32_t kListItems = 0x288;
constexpr uint32_t kItemInnerText = 548;

// The list item text setters, all (list, index, string_id, ...). The first
// also takes an x and y, -1 meaning "centre it".
constexpr uint32_t kListItemTextFnAddr = 0x825D28B0u;
constexpr uint32_t kListItemTextCenteredFnAddr = 0x825D29A8u;
constexpr uint32_t kListItemTextFixedFnAddr = 0x825D3D30u;
constexpr uint32_t kListItemTextCentered2FnAddr = 0x825D3DB8u;

constexpr uint32_t kEnglish = 1;

// What the boot loaders pick for each XLanguage id (sub_82576C58 for the code,
// sub_82586AA8 for the font, sub_824FAEF8 for the PS1 index).
struct LanguageFiles {
  const char* code;
  uint32_t font_name;
  uint32_t ps1_index;
};

LanguageFiles FilesFor(uint32_t language) {
  switch (language) {
    case 2:
      return {"ja", kFontJaNameAddr, 1};
    case 3:
      return {"de", kFontEnNameAddr, 3};
    case 4:
      return {"fr", kFontEnNameAddr, 5};
    case 5:
      return {"es", kFontEnNameAddr, 4};
    case 6:
      return {"it", kFontEnNameAddr, 2};
    case 7:
      return {"ko", kFontKoNameAddr, 0};
    case 8:
      return {"ch", kFontChNameAddr, 0};
    case 9:
      return {"pt", kFontEnNameAddr, 0};
    case kEnglish:
    default:
      return {"en", kFontEnNameAddr, 0};
  }
}

rex::Runtime* g_runtime = nullptr;
PPCFunc* g_alloc_fn = nullptr;
PPCFunc* g_load_font_fn = nullptr;
PPCFunc* g_text_width_fn = nullptr;

// Written by the cvar callback (any thread), read by the per-frame hook.
std::atomic<uint32_t> g_requested_language{0};

// The language the game currently has loaded, or 0 until the first frame
// after its boot load. Guest thread only.
uint32_t g_loaded_language = 0;

// Everything loaded so far, kept for the process lifetime (see the header).
// Keyed by language code and by font file name address. Guest thread only.
std::unordered_map<std::string, uint32_t> g_tables;
std::unordered_map<uint32_t, uint32_t> g_fonts;

// One recorded list item text call, replayed as is.
struct ListItemText {
  uint32_t fn;
  uint32_t index;
  uint32_t id;
  uint32_t x;
  uint32_t y;
};

// Text widget -> the string id it was last given.
std::mutex g_widgets_mutex;
std::unordered_map<uint32_t, uint32_t> g_widgets;
// (list << 32 | index) -> the last text call for that item. Same mutex.
std::unordered_map<uint64_t, ListItemText> g_list_items;

uint32_t Read32BE(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

void Write32BE(uint8_t* base, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(base + address, value);
}

uint16_t Read16BE(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint16_t>(base + address);
}

void Write16BE(uint8_t* base, uint32_t address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(base + address, value);
}

uint32_t CallGuest(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3) {
  if (!fn) {
    return 0;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = r3;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// Reads d:\media\<name> the way the game's own open resolves it (parent, then
// child), so a mod or overlay that shadows the file is honoured.
bool ReadMediaFile(const std::string& name, std::vector<uint8_t>& out) {
  auto* vfs = g_runtime ? g_runtime->file_system() : nullptr;
  rex::filesystem::Entry* parent = vfs ? vfs->ResolvePath("d:\\media") : nullptr;
  rex::filesystem::Entry* entry = parent ? parent->GetChild(name) : nullptr;
  if (!entry) {
    return false;
  }
  rex::filesystem::File* file = nullptr;
  if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS ||
      !file) {
    return false;
  }
  out.resize(entry->size());
  size_t read = 0;
  const X_STATUS status = file->ReadSync(out, 0, &read);
  file->Destroy();
  return status == X_STATUS_SUCCESS && read == out.size();
}

// Loads strings_<code>.bin into the game's heap and fixes it up exactly as
// sub_82586AA8 does. Returns the block's guest address, or 0.
uint32_t LoadStringTable(PPCContext& ctx, uint8_t* base, const std::string& code) {
  const std::string name = fmt::format("strings_{}.bin", code);
  std::vector<uint8_t> data;
  if (!ReadMediaFile(name, data) || data.size() < 4) {
    return 0;
  }
  const uint32_t size = static_cast<uint32_t>(data.size());
  const uint32_t count = rex::memory::load_and_swap<uint32_t>(data.data());
  if (count == 0 || count > (size - 4) / kEntrySize) {
    REXLOG_WARN("[language] {} is malformed ({} entries in {} bytes)", name, count, size);
    return 0;
  }

  const uint32_t block = CallGuest(ctx, base, g_alloc_fn, size);
  if (!block) {
    REXLOG_WARN("[language] could not allocate {} bytes for {}", size, name);
    return 0;
  }
  std::memcpy(base + block, data.data(), size);

  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = block + 4 + i * kEntrySize;
    Write32BE(base, entry + kEntryText, Read32BE(base, entry + kEntryText) + block);
    Write32BE(base, entry + kEntryFontName, Read32BE(base, entry + kEntryFontName) + block);
    const auto thousandths = static_cast<int32_t>(Read32BE(base, entry + kEntryScale));
    float scale = static_cast<float>(static_cast<double>(thousandths) * 0.001);
    if (scale == 0.0f) {
      scale = 1.0f;
    }
    Write32BE(base, entry + kEntryScale, std::bit_cast<uint32_t>(scale));
  }
  return block;
}

uint32_t TableFor(PPCContext& ctx, uint8_t* base, const std::string& code) {
  if (auto it = g_tables.find(code); it != g_tables.end()) {
    return it->second;
  }
  const uint32_t table = LoadStringTable(ctx, base, code);
  if (table) {
    g_tables.emplace(code, table);
  }
  return table;
}

// Creates the font through the game's own loader, which also makes it current.
uint32_t FontFor(PPCContext& ctx, uint8_t* base, uint32_t font_name) {
  if (auto it = g_fonts.find(font_name); it != g_fonts.end()) {
    return it->second;
  }
  const uint32_t previous = Read32BE(base, kFontPtrAddr);
  CallGuest(ctx, base, g_load_font_fn, font_name);
  const uint32_t font = Read32BE(base, kFontPtrAddr);
  if (!font || font == previous) {
    Write32BE(base, kFontPtrAddr, previous);
    return 0;
  }
  g_fonts.emplace(font_name, font);
  return font;
}

// True if the widget's text is exactly `text` (both NUL-terminated UTF-16BE).
bool WidgetShows(const uint8_t* base, uint32_t widget, uint32_t text) {
  for (uint32_t i = 0; i < kWidgetTextChars; ++i) {
    const uint16_t c = Read16BE(base, widget + kWidgetText + i * 2);
    if (c != Read16BE(base, text + i * 2)) {
      return false;
    }
    if (c == 0) {
      return true;
    }
  }
  return true;
}

void CopyText(uint8_t* base, uint32_t widget, uint32_t text) {
  for (uint32_t i = 0; i < kWidgetTextChars; ++i) {
    const uint16_t c = i + 1 < kWidgetTextChars ? Read16BE(base, text + i * 2) : 0;
    Write16BE(base, widget + kWidgetText + i * 2, c);
    if (c == 0) {
      break;
    }
  }
  Write16BE(base, widget + kWidgetTextReset, 0);
}

struct Retext {
  uint32_t widget;
  uint32_t id;
  // The widget still has the old entry's scale, i.e. nobody resized it after
  // setting the text, so it should take the new entry's scale too.
  bool table_scale;
  // The screen centred it on x=320 by hand (0x140 - width/2, the way every
  // title is placed), so it has to be re-centred for the new width.
  bool centered;
};

struct ListReplay {
  uint32_t list;
  ListItemText call;
};

uint32_t CenteredX(uint32_t width) {
  return static_cast<uint32_t>(kScreenCenterX - static_cast<int32_t>(width) / 2);
}

void SwitchLanguage(PPCContext& ctx, uint8_t* base, uint32_t language) {
  const LanguageFiles files = FilesFor(language);

  // Same fallback as the boot loader: no strings file for the language means
  // English strings and the English font.
  uint32_t font_name = files.font_name;
  uint32_t table = TableFor(ctx, base, files.code);
  if (!table) {
    REXLOG_WARN("[language] no strings_{}.bin; using English", files.code);
    table = TableFor(ctx, base, "en");
    font_name = kFontEnNameAddr;
  }
  if (!table) {
    REXLOG_WARN("[language] no string table could be loaded; language unchanged");
    return;
  }

  // Work out what to rewrite while the old table and font are still current
  // (FontFor below makes a newly created font current straight away).
  const uint32_t old_entries = Read32BE(base, kStringEntriesPtrAddr);
  const uint32_t old_count = Read32BE(base, kStringCountAddr);
  auto old_text = [&](uint32_t id) {
    return (old_entries && id < old_count) ? Read32BE(base, old_entries + id * kEntrySize)
                                           : 0;
  };

  std::vector<Retext> retext;
  size_t stale = 0;
  {
    std::lock_guard lock(g_widgets_mutex);
    for (auto it = g_widgets.begin(); it != g_widgets.end();) {
      const auto [widget, id] = *it;
      const uint32_t text = old_text(id);
      if (!text || !WidgetShows(base, widget, text)) {
        it = g_widgets.erase(it);
        ++stale;
        continue;
      }
      const uint32_t old_scale = Read32BE(base, old_entries + id * kEntrySize + kEntryScale);
      retext.push_back({widget, id,
                        Read32BE(base, widget + kWidgetScale) == old_scale &&
                            Read32BE(base, widget + kWidgetTargetScale) == old_scale,
                        false});
      ++it;
    }
  }
  for (Retext& r : retext) {
    r.centered = Read32BE(base, r.widget + kWidgetX) ==
                 CenteredX(CallGuest(ctx, base, g_text_width_fn, r.widget));
  }

  std::vector<ListReplay> lists;
  {
    std::lock_guard lock(g_widgets_mutex);
    for (auto it = g_list_items.begin(); it != g_list_items.end();) {
      const auto& [key, call] = *it;
      const uint32_t list = static_cast<uint32_t>(key >> 32);
      const uint32_t index = static_cast<uint32_t>(key);
      const uint32_t item = index < Read32BE(base, list + kListItemCount)
                                ? Read32BE(base, list + kListItems + index * 4)
                                : 0;
      const uint32_t inner = item ? Read32BE(base, item + kItemInnerText) : 0;
      const uint32_t text = old_text(call.id);
      if (!inner || !text || !WidgetShows(base, inner, text)) {
        it = g_list_items.erase(it);
        ++stale;
        continue;
      }
      lists.push_back({list, call});
      ++it;
    }
  }

  const uint32_t font = FontFor(ctx, base, font_name);
  if (!font) {
    REXLOG_WARN("[language] could not load the font for language {}; keeping the current one",
                language);
  }

  const uint32_t count = Read32BE(base, table);
  Write32BE(base, kStringTablePtrAddr, table);
  Write32BE(base, kStringEntriesPtrAddr, table + 4);
  Write32BE(base, kStringCountAddr, count);
  if (font) {
    Write32BE(base, kFontPtrAddr, font);
  }
  Write32BE(base, kPs1LanguageAddr, files.ps1_index);

  for (const Retext& r : retext) {
    if (r.id >= count) {
      continue;
    }
    const uint32_t entry = table + 4 + r.id * kEntrySize;
    CopyText(base, r.widget, Read32BE(base, entry + kEntryText));
    if (r.table_scale) {
      const uint32_t scale = Read32BE(base, entry + kEntryScale);
      Write32BE(base, r.widget + kWidgetScale, scale);
      Write32BE(base, r.widget + kWidgetTargetScale, scale);
    }
    if (r.centered) {
      Write32BE(base, r.widget + kWidgetX,
                CenteredX(CallGuest(ctx, base, g_text_width_fn, r.widget)));
    }
  }

  // The list setters pick the item's scale and position themselves, so
  // running them again is all it takes. They come back through our hook,
  // which just re-records the same call.
  auto* dispatcher = g_runtime->function_dispatcher();
  for (const ListReplay& l : lists) {
    if (l.call.id >= count) {
      continue;
    }
    PPCFunc* fn = dispatcher->GetFunction(l.call.fn);
    if (!fn) {
      continue;
    }
    PPCContext saved = ctx;
    ctx.r3.u32 = l.list;
    ctx.r4.u32 = l.call.index;
    ctx.r5.u32 = l.call.id;
    ctx.r6.u32 = l.call.x;
    ctx.r7.u32 = l.call.y;
    fn(ctx, base);
    ctx = saved;
  }

  // Our own rows on the settings screen, and our Help & Options items, are
  // literal text, not ids.
  RefreshNativeOptionsText(ctx, base);
  RefreshHelpOptionsText(ctx, base);

  REXLOG_INFO("[language] switched to {} ({} widget(s) and {} list item(s) retexted, {} stale)",
              files.code, retext.size(), lists.size(), stale);
}

}  // namespace

void BindLanguageSwitch(rex::Runtime* runtime) {
  g_runtime = runtime;
  if (runtime && runtime->function_dispatcher()) {
    g_alloc_fn = runtime->function_dispatcher()->GetFunction(kAllocFnAddr);
    g_load_font_fn = runtime->function_dispatcher()->GetFunction(kLoadFontFnAddr);
    g_text_width_fn = runtime->function_dispatcher()->GetFunction(kTextWidthFnAddr);
  }

  // The SDK registers it as restart-scoped, which is what put the restart
  // marker and banner on it. It is not any more.
  MarkCvarLive(kLanguageCvar);

  g_requested_language.store(rex::cvar::Query<uint32_t>(kLanguageCvar));
  rex::cvar::RegisterChangeCallback(kLanguageCvar, [](std::string_view, std::string_view value) {
    g_requested_language.store(
        static_cast<uint32_t>(std::strtoul(std::string(value).c_str(), nullptr, 10)));
  });
}

void NoteTextSetById(uint32_t text_widget, uint32_t string_id) {
  if (!text_widget) {
    return;
  }
  std::lock_guard lock(g_widgets_mutex);
  g_widgets[text_widget] = string_id;
}

void ApplyPendingLanguageSwitch(PPCContext& ctx, uint8_t* base) {
  const uint32_t requested = g_requested_language.load();
  if (requested == g_loaded_language) {
    return;
  }

  const uint32_t table = Read32BE(base, kStringTablePtrAddr);
  if (!table) {
    return;  // the boot load hasn't happened yet; it will read the cvar itself
  }

  if (g_loaded_language == 0) {
    // First frame after boot: whatever is loaded now is the cvar's language
    // (or English, if its strings file was missing). Remember it so switching
    // back never loads it twice.
    const LanguageFiles files = FilesFor(requested);
    std::vector<uint8_t> unused;
    const bool fell_back = !ReadMediaFile(fmt::format("strings_{}.bin", files.code), unused);
    g_tables.emplace(fell_back ? "en" : files.code, table);
    g_fonts.emplace(fell_back ? kFontEnNameAddr : files.font_name, Read32BE(base, kFontPtrAddr));
    g_loaded_language = requested;
    return;
  }

  SwitchLanguage(ctx, base, requested);
  // Even on failure: retrying every frame would not help.
  g_loaded_language = requested;
}

namespace {

// Records the call, then runs the original. Shared by all four list setters;
// `fn` tells them apart at replay.
void RecordListItemText(uint32_t fn, PPCContext& ctx, uint8_t* base) {
  {
    std::lock_guard lock(g_widgets_mutex);
    const uint64_t key = (static_cast<uint64_t>(ctx.r3.u32) << 32) | ctx.r4.u32;
    g_list_items[key] = {fn, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
  }
  fnhook::CallOriginal(fn, ctx, base);
}

extern "C" void LanguageSwitch_ListItemText(PPCContext& ctx, uint8_t* base) {
  RecordListItemText(kListItemTextFnAddr, ctx, base);
}
extern "C" void LanguageSwitch_ListItemTextCentered(PPCContext& ctx, uint8_t* base) {
  RecordListItemText(kListItemTextCenteredFnAddr, ctx, base);
}
extern "C" void LanguageSwitch_ListItemTextFixed(PPCContext& ctx, uint8_t* base) {
  RecordListItemText(kListItemTextFixedFnAddr, ctx, base);
}
extern "C" void LanguageSwitch_ListItemTextCentered2(PPCContext& ctx, uint8_t* base) {
  RecordListItemText(kListItemTextCentered2FnAddr, ctx, base);
}

}  // namespace

}  // namespace nocturne

// Entry hooks, wired up in nocturnerecomp_config.toml.
bool LanguageSwitch_ListItemTextHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kListItemTextFnAddr,
                                 &nocturne::LanguageSwitch_ListItemText);
}
bool LanguageSwitch_ListItemTextCenteredHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kListItemTextCenteredFnAddr,
                                 &nocturne::LanguageSwitch_ListItemTextCentered);
}
bool LanguageSwitch_ListItemTextFixedHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kListItemTextFixedFnAddr,
                                 &nocturne::LanguageSwitch_ListItemTextFixed);
}
bool LanguageSwitch_ListItemTextCentered2Hook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kListItemTextCentered2FnAddr,
                                 &nocturne::LanguageSwitch_ListItemTextCentered2);
}
