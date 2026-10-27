#pragma once

// The start screen's text: the [intro] table of res/lang/*.toml through
// ui_text.h, plus the language picker the screen shows before any config
// exists. The language is the user_language choice, so the screen, the
// overlays and the game always agree; with none chosen yet it is the system's.

#include <filesystem>
#include <string>
#include <string_view>

#include "install_pipeline.h"
#include "ui_text.h"

namespace nocturne {

// In the order of kKeys in intro_text.cpp.
enum class IntroText {
  kStart,
  kExtract,
  kSelectFile,
  kQuit,
  kLanguage,
  kReady,
  kReadyRelease,
  kFound,
  kFoundRelease,
  kPressExtract,
  kNeedFiles,
  kSelectIsoOnly,
  kCopyHint,
  kSelected,
  kTitle,
  kUnsupportedLanguage,
  kDeleteFiles,
  kCancel,
  kConfirmDelete,
  kNotEnoughSpace,
  kAddVersion,
  kDeleteVersion,
  kAlreadyInstalled,
  kLinked,
  kNeedsUpdate,
  kChooseVersion,
  kHintSelect,
  kHintBack,
  kHintQuit,
  kDialogNeedsMouse,
  kOutdated,
  kOutdatedQuestion,
  kSkip,
  kExtractAgain,
  kNotSupportedYet,
  kNotPlayable,
  kCount,
};

const char* Tr(IntroText id);

// With `arg` in place of the text's "{}".
std::string Tr(IntroText id, std::string_view arg);

// An installer error from the [intro] table; each {} takes the next argument.
std::string TrErr(const char* key, std::string_view a = {}, std::string_view b = {});

const char* TrPhase(Phase phase);

// A progress title the installer reports in English; unknown ones pass through.
std::string TrProgress(const std::string& title);

// `key` in the language `code` ("en", "ja") rather than the one in use, else
// English's; null when neither has it.
const char* TrIn(const char* code, const char* section, const char* key);

// The languages, each named in itself, for the screen's language picker.
int IntroLanguageCount();
const char* IntroLanguageName(int index);
const char* IntroLanguageCodeAt(int index);
int IntroLanguage();

// Sets user_language to that language and saves it to settings.toml.
void SetIntroLanguage(int index);

// Binds the settings.toml SetIntroLanguage saves to, and adopts the system's
// language when none was chosen.
void BindIntroLanguageConfig(const std::filesystem::path& user_settings_path);

// The [sdk_ui] table, the SDK overlay strings the Mods page reuses. Null when
// no language has the key.
const char* SdkUiText(const char* key);

// Every language file's text, for baking the glyphs the screen needs.
std::string IntroGlyphText();

}  // namespace nocturne
