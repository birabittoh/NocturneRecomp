#include "intro_text.h"

#include <array>
#include <cstdlib>

#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>

#include <rex/cvar.h>

#include "lang.generated.h"
#include "settings.h"

namespace nocturne {
namespace {

// IntroText order.
constexpr std::array<const char*, size_t(IntroText::kCount)> kKeys = {
    "start",           "extract",           "select_file",      "quit",
    "language",        "ready",             "ready_release",    "found",
    "found_release",   "press_extract",     "need_files",       "select_iso_only",
    "copy_hint",       "selected",          "title",            "unsupported_language",
    "delete_files",    "cancel",            "confirm_delete",   "not_enough_space",
    "add_version",     "delete_version",    "already_installed", "linked",
    "needs_update",    "library_ready",    "hint_select",      "hint_back",
    "hint_quit",       "dialog_needs_mouse", "outdated",        "outdated_question",
    "skip",            "extract_again",
};
constexpr std::array<const char*, kInstallPhaseCount> kPhaseKeys = {"phase_extract"};
constexpr std::array<const char*, 3> kProgressKeys = {"progress_extracting", "progress_deleting",
                                                      "progress_mods"};

// res/lang file code <-> XLanguage id.
constexpr std::array<std::pair<std::string_view, uint32_t>, 6> kIds = {
    {{"en", 1}, {"ja", 2}, {"de", 3}, {"fr", 4}, {"es", 5}, {"it", 6}}};

uint32_t IdOfCode(std::string_view code) {
  for (const auto& [name, id] : kIds) {
    if (code == name)
      return id;
  }
  return 0;
}

const char* CodeOfId(uint32_t id) {
  for (const auto& [name, xlanguage] : kIds) {
    if (id == xlanguage)
      return name.data();
  }
  return "en";
}

std::filesystem::path g_user_settings_path;

const char* Lookup(const char* key) {
  return Tr("intro", key);
}

int FromSystem() {
  int count = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&count);
  if (!locales)
    return -1;
  const auto ids = ShippedLanguageIds();
  int found = -1;
  for (int i = 0; i < count && found < 0; ++i) {
    const uint32_t id = IdOfCode(locales[i]->language);
    for (size_t j = 0; j < ids.size(); ++j) {
      if (id != 0 && ids[j] == id)
        found = int(j);
    }
  }
  SDL_free(locales);
  return found;
}

}  // namespace

const char* Tr(IntroText id) {
  return Lookup(kKeys[size_t(id)]);
}

std::string Tr(IntroText id, std::string_view arg) {
  std::string text = Tr(id);
  if (const size_t at = text.find("{}"); at != std::string::npos)
    text.replace(at, 2, arg);
  return text;
}

std::string TrErr(const char* key, std::string_view a, std::string_view b) {
  std::string text = Lookup(key);
  size_t at = 0;
  for (std::string_view arg : {a, b}) {
    if (at = text.find("{}", at); at == std::string::npos)
      break;
    text.replace(at, 2, arg);
    at += arg.size();
  }
  return text;
}

const char* TrPhase(Phase phase) {
  return Lookup(kPhaseKeys[size_t(phase)]);
}

std::string TrProgress(const std::string& title) {
  for (const char* key : kProgressKeys) {
    const char* english = TrInLanguage(1, "intro", key);
    if (english && title == english)
      return Lookup(key);
  }
  return title;
}

const char* TrIn(const char* code, const char* section, const char* key) {
  if (const char* text = TrInLanguage(IdOfCode(code), section, key))
    return text;
  return TrInLanguage(1, section, key);
}

int IntroLanguageCount() {
  return int(ShippedLanguageIds().size());
}

const char* IntroLanguageName(int index) {
  const char* name = LanguageName(ShippedLanguageIds()[size_t(index)]);
  return name ? name : "?";
}

const char* IntroLanguageCodeAt(int index) {
  return CodeOfId(ShippedLanguageIds()[size_t(index)]);
}

int IntroLanguage() {
  const auto ids = ShippedLanguageIds();
  const uint32_t current = UiLanguageId();
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == current)
      return int(i);
  }
  // A mod language: the picker shows English as the closest shipped one.
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == 1)
      return int(i);
  }
  return 0;
}

void SetIntroLanguage(int index) {
  if (index < 0 || index >= IntroLanguageCount() || index == IntroLanguage())
    return;
  const std::string id = std::to_string(ShippedLanguageIds()[size_t(index)]);
  rex::cvar::SetFlagByName("user_language", id, /*persist=*/true);
  // The config file is read-only; settings.toml is where user choices live.
  if (!g_user_settings_path.empty())
    rex::cvar::SaveConfigSubset(g_user_settings_path, {"user_language"});
}

void BindIntroLanguageConfig(const std::filesystem::path& user_settings_path) {
  g_user_settings_path = user_settings_path;
  // Applies live, so setting it below must not count as a pending restart.
  MarkCvarLive("user_language");
  // No language chosen yet: start in the system's, text included.
  if (rex::cvar::GetFlagSource("user_language") != rex::cvar::Source::kDefault)
    return;
  const int system = FromSystem();
  if (system >= 0)
    rex::cvar::SetFlagByName("user_language",
                             std::to_string(ShippedLanguageIds()[size_t(system)]),
                             /*persist=*/false);
}

const char* SdkUiText(const char* key) {
  return TrOr("sdk_ui", key, nullptr);
}

std::string IntroGlyphText() {
  std::string text;
  for (const LangFile& file : kLangFiles)
    text.append(reinterpret_cast<const char*>(file.text), file.size);
  return text;
}

}  // namespace nocturne
