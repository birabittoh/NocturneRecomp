// nocturnerecomp - the text this project authors itself: the settings and
// randomizer overlays and the rows it adds to the game's Options screen.
//
// It lives in res/lang/*.toml, one file per language and one table per
// screen, and is looked up by (section, key) in the language the game is
// set to (the `user_language` cvar), then in English. The game's own text
// follows the same cvar, so the interface and the story always agree.
//
// Mods can replace any string, or translate the lot for a language they added
// (see settings.h): mod text wins over the shipped files, and the first mod to
// set a string keeps it.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace rex::system {
class ModRegistry;
}  // namespace rex::system

namespace nocturne {

// The XLanguage id (1 English, 2 Japanese, 3 German, 4 French, 5 Spanish,
// 6 Italian, or a mod's own) the interface is in.
uint32_t UiLanguageId();

// The current language's text for `key`, else English's. The key itself when
// neither has it, which shows as a typo rather than a crash.
const char* Tr(const char* section, const char* key);

// `fallback` when neither has the key.
const char* TrOr(const char* section, const char* key, const char* fallback);

// The first "{}" of the text replaced by `arg`.
std::string Tr(const char* section, const char* key, std::string_view arg);

// Each "{}" of the text replaced by the next argument.
std::string TrArgs(const char* section, const char* key,
                   std::initializer_list<std::string_view> args);

// The text only if the current language itself (or a mod for it) has it; null
// otherwise, with no English fallback.
const char* TrCurrent(const char* section, const char* key);

// The shipped languages' XLanguage ids, in res/lang file order.
std::vector<uint32_t> ShippedLanguageIds();

// A shipped language's name for itself (its file's `name`), or null.
const char* LanguageName(uint32_t language_id);

// `key` in that language only (mod text included), with no English fallback.
const char* TrInLanguage(uint32_t language_id, const char* section, const char* key);

// `key` of the [native] table as UTF-16, for the guest's own screens (the
// game draws it in the language's font). Null only under TrNativeCurrent when
// the current language has no such string.
const char16_t* TrNative(const char* key);
const char16_t* TrNativeCurrent(const char* key);

// A string a mod set for `language_id` (nullptr if none did), for text that
// has another source to fall back to, such as the achievements' own.
const char* FindModUiString(uint32_t language_id, std::string_view section, std::string_view key);

// Mod supplied text. The first mod to set a string keeps it; later ones are
// ignored with a warning.
void AddUiString(uint32_t language_id, std::string_view section, std::string_view key,
                 std::string_view value);

// Subscribes to the "settings.ui_string" mod-registry event (see settings.h).
void RegisterUiStringListener(rex::system::ModRegistry* registry);

// `text` as UTF-16, converted once and kept for the life of the process.
const char16_t* CachedUtf16(const char* text);

// Decodes UTF-8 to UTF-16 (astral code points as a surrogate pair); an invalid
// lead byte is skipped rather than corrupting the rest of the string.
std::u16string Utf8ToUtf16(std::string_view s);

}  // namespace nocturne
