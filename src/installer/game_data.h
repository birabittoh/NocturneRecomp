#pragma once

// Picks the installed version (game_library.h) the runtime starts, and installs
// new ones through the phases of install_pipeline.h, shown on the intro
// screen. A picked source is identified first and installed only when the
// player presses Extract. A saved version with nothing left to do skips the
// screen.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "install_pipeline.h"

namespace rex::ui {
class WindowedAppContext;
}

namespace nocturne {

enum class GameDataChoice {
  kQuit,
  kDiscImage,
  kFolder,
  kExtract,
  kStart,
  kDelete,
  kConfirmDelete,
  kCancel,
  kAddVersion,
  kExtractAgain,
};

// The library holds this many versions.
constexpr int kMaxVersions = 2;

// One installed version, as the start screen lists it.
struct VersionRow {
  std::string label;
  // Under the label: linked path, update needed.
  std::string note;
  // The text it ships (release_id.h bits); 0 offers every language.
  uint32_t languages = 0;
  bool linked = false;
  // Release::startable: false for a release this build cannot run yet.
  bool startable = true;
  // Its files are ours to delete; a linked one is forgotten instead.
  bool can_delete = false;
};

// What the prompt is asked to offer. With versions installed it lists them
// (the library); `adding`, or an empty list, asks for a source instead.
struct GameDataPrompt {
  // False on Android, where SDL has no folder picker.
  bool can_pick_folder = true;
  // Where already extracted files can be copied to be found, or empty.
  std::string copy_hint;
  // Why the previous selection failed, empty on the first call.
  std::string error;
  // The release picked, once identified.
  std::string release;
  // The text it ships (release_id.h bits); 0 offers every language.
  uint32_t languages = 0;
  // A supported source is picked: Extract is enabled.
  bool can_extract = false;
  std::vector<VersionRow> versions;
  // The row the cursor starts on, or -1.
  int selected_version = -1;
  // Picking a source for another version, over the library.
  bool adding = false;
  // Asking whether to delete: the question, shown with Cancel and Delete
  // Files, or Forget for a linked version.
  std::string confirm;
  bool confirm_forget = false;
  // Asking whether to extract the selected version again, shown with Skip and
  // Extract Again.
  std::string outdated;
};

struct GameDataOptions {
  // The config game_data_root is persisted to; relative values resolve against
  // its folder.
  std::filesystem::path config_path;
  // The one launch flag file (StartScreenFlagPath).
  std::filesystem::path start_screen_flag;
  // Where the picked version and the linked ones are saved.
  std::filesystem::path user_settings_path;
  // The resolved game_data_root, the library root; empty for the default.
  std::filesystem::path game_data_root;
};

// Migrates and scans the library, and picks the version to start. True when
// it can start with no screen: the saved version needs no more work and no
// screen was asked for.
bool UsePreparedGameData(const GameDataOptions& options);

// The version folder the runtime mounts, once one is picked.
std::filesystem::path SelectedGameDirectory();

// Where a start screen request is kept, next to the user settings. A file
// rather than an environment variable, since Android restarts the app without
// handing the old environment on.
std::filesystem::path StartScreenFlagPath(const std::filesystem::path& user_settings_path);

// Makes the next launch show the start screen even with prepared game data, for
// that launch only; false withdraws the request.
void RequestStartScreenOnNextLaunch(const std::filesystem::path& flag, bool request);

// Asks through the intro screen (a message box where it cannot draw) until a
// version is started, or the player quits. A saved version that still needs
// work runs its phases first. Returns at once: the main loop has to run for
// the screen to get input. `done` runs on the UI thread.
void AskForGameData(const GameDataOptions& options, rex::ui::WindowedAppContext& context,
                    std::function<void(bool ready)> done);

// Whether game files are being installed; the window must not
// close then.
bool IsExtractingGameData();

}  // namespace nocturne
