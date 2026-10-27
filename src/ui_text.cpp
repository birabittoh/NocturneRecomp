#include "ui_text.h"

#include <array>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/system/mod_registry.h>

#include "lang.generated.h"

namespace nocturne {
namespace {

struct Language {
  uint32_t id;
  toml::table tables;
};

// The shipped languages by XLanguage id, parsed once.
const std::vector<Language>& Languages() {
  static const std::vector<Language> languages = [] {
    // res/lang file code -> XLanguage id.
    static constexpr std::array<std::pair<std::string_view, uint32_t>, 6> kIds = {
        {{"en", 1}, {"ja", 2}, {"de", 3}, {"fr", 4}, {"es", 5}, {"it", 6}}};
    std::vector<Language> out;
    for (const LangFile& file : kLangFiles) {
      uint32_t id = 0;
      for (const auto& [code, xlanguage] : kIds) {
        if (code == file.code)
          id = xlanguage;
      }
      if (id == 0) {
        REXLOG_WARN("[ui_text] res/lang/{}.toml has no XLanguage id; ignoring it", file.code);
        continue;
      }
      try {
        out.push_back(
            {id, toml::parse(std::string_view(reinterpret_cast<const char*>(file.text), file.size))});
      } catch (const toml::parse_error& e) {
        REXLOG_ERROR("[ui_text] res/lang/{}.toml: {}", file.code, e.description());
      }
    }
    return out;
  }();
  return languages;
}

const Language* FindLanguage(uint32_t id) {
  for (const Language& language : Languages()) {
    if (language.id == id)
      return &language;
  }
  return nullptr;
}

// "<id>\n<section>\n<key>" -> value.
using ModStrings = std::map<std::string, std::string, std::less<>>;
ModStrings& Mods() {
  static ModStrings strings;
  return strings;
}
std::mutex g_mods_mutex;

std::string ModKey(uint32_t id, std::string_view section, std::string_view key) {
  std::string out = std::to_string(id);
  out += '\n';
  out += section;
  out += '\n';
  out += key;
  return out;
}

const char* FindIn(uint32_t id, const char* section, const char* key) {
  if (const char* mod = FindModUiString(id, section, key))
    return mod;
  const Language* language = FindLanguage(id);
  if (!language)
    return nullptr;
  const toml::table* table = language->tables[section].as_table();
  if (const auto* text = table ? (*table)[key].as_string() : nullptr)
    return text->get().c_str();
  return nullptr;
}

}  // namespace

std::vector<uint32_t> ShippedLanguageIds() {
  std::vector<uint32_t> ids;
  for (const Language& language : Languages())
    ids.push_back(language.id);
  return ids;
}

const char* LanguageName(uint32_t language_id) {
  const Language* language = FindLanguage(language_id);
  const auto* name = language ? language->tables["name"].as_string() : nullptr;
  return name ? name->get().c_str() : nullptr;
}

const char* TrInLanguage(uint32_t language_id, const char* section, const char* key) {
  return FindIn(language_id, section, key);
}

uint32_t UiLanguageId() {
  return uint32_t(std::strtoul(rex::cvar::GetFlagByName("user_language").c_str(), nullptr, 10));
}

const char* TrCurrent(const char* section, const char* key) {
  return FindIn(UiLanguageId(), section, key);
}

const char* TrOr(const char* section, const char* key, const char* fallback) {
  if (const char* text = TrCurrent(section, key))
    return text;
  const char* english = FindIn(1, section, key);
  return english ? english : fallback;
}

const char* Tr(const char* section, const char* key) {
  return TrOr(section, key, key);
}

std::string Tr(const char* section, const char* key, std::string_view arg) {
  std::string text = Tr(section, key);
  if (const size_t at = text.find("{}"); at != std::string::npos)
    text.replace(at, 2, arg);
  return text;
}

// `text` as UTF-16, converted once. Node addresses are stable, so a pointer
// handed out stays valid.
std::string TrArgs(const char* section, const char* key,
                   std::initializer_list<std::string_view> args) {
  std::string text = Tr(section, key);
  size_t at = 0;
  for (std::string_view arg : args) {
    if (at = text.find("{}", at); at == std::string::npos)
      break;
    text.replace(at, 2, arg);
    at += arg.size();
  }
  return text;
}

const char16_t* CachedUtf16(const char* text) {
  static std::map<std::string, std::u16string, std::less<>> cache;
  static std::mutex mutex;
  const std::lock_guard lock(mutex);
  auto it = cache.find(std::string_view(text));
  if (it == cache.end())
    it = cache.emplace(text, Utf8ToUtf16(text)).first;
  return it->second.c_str();
}

const char16_t* TrNativeCurrent(const char* key) {
  const char* text = TrCurrent("native", key);
  return text ? CachedUtf16(text) : nullptr;
}

const char16_t* TrNative(const char* key) {
  const char* text = TrOr("native", key, nullptr);
  return text ? CachedUtf16(text) : u"";
}

const char* FindModUiString(uint32_t language_id, std::string_view section, std::string_view key) {
  const std::lock_guard lock(g_mods_mutex);
  const auto it = Mods().find(ModKey(language_id, section, key));
  return it != Mods().end() ? it->second.c_str() : nullptr;
}

void AddUiString(uint32_t language_id, std::string_view section, std::string_view key,
                 std::string_view value) {
  if (section.empty() || key.empty() || value.empty()) {
    REXLOG_WARN("[ui_text] ignoring a string with an empty field ({}.{} for language {})", section,
                key, language_id);
    return;
  }
  const std::lock_guard lock(g_mods_mutex);
  if (!Mods().emplace(ModKey(language_id, section, key), value).second) {
    REXLOG_WARN("[ui_text] ignoring duplicate string {}.{} for language {} (already registered)",
                section, key, language_id);
  }
}

void RegisterUiStringListener(rex::system::ModRegistry* registry) {
  if (!registry)
    return;
  registry->Subscribe(
      "settings.ui_string", [](const rex::system::ModRegistry::EventPayload& payload) {
        std::string_view text(reinterpret_cast<const char*>(payload.bytes.data()),
                              payload.bytes.size());
        const size_t eq = text.find('=');
        const size_t dot = text.substr(0, eq).find('.');
        if (eq == std::string_view::npos || dot == std::string_view::npos) {
          REXLOG_WARN("[ui_text] ignoring malformed settings.ui_string payload (want "
                      "\"section.key=value\")");
          return;
        }
        AddUiString(uint32_t(payload.u64), text.substr(0, dot), text.substr(dot + 1, eq - dot - 1),
                    text.substr(eq + 1));
      });
}

std::u16string Utf8ToUtf16(std::string_view s) {
  std::u16string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const auto byte_at = [&](size_t offset) {
      return static_cast<unsigned char>(s[i + offset]);
    };
    const unsigned char c0 = byte_at(0);
    uint32_t cp;
    size_t len;
    if (c0 < 0x80) {
      cp = c0;
      len = 1;
    } else if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
      cp = ((c0 & 0x1Fu) << 6) | (byte_at(1) & 0x3Fu);
      len = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
      cp = ((c0 & 0x0Fu) << 12) | ((byte_at(1) & 0x3Fu) << 6) | (byte_at(2) & 0x3Fu);
      len = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
      cp = ((c0 & 0x07u) << 18) | ((byte_at(1) & 0x3Fu) << 12) | ((byte_at(2) & 0x3Fu) << 6) |
           (byte_at(3) & 0x3Fu);
      len = 4;
    } else {
      ++i;
      continue;
    }
    if (cp <= 0xFFFF) {
      out.push_back(static_cast<char16_t>(cp));
    } else if (cp <= 0x10FFFF) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FFu)));
    }
    i += len;
  }
  return out;
}

}  // namespace nocturne
