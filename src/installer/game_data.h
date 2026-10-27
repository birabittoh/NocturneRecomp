#pragma once

// Starts the installed releases (game_library.h) together, and installs new
// ones through the phases of install_pipeline.h, shown on the intro screen. A
// picked source is identified first and installed only when the player presses
// Extract. A library with nothing left to do skips the screen.

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
  kExtract,
  kStart,
  kDelete,
  kConfirmDelete,
  kCancel,
  kAddVersion,
};

// The library holds this many versions.
constexpr int kMaxVersions = 2;

// One installed version, as the start screen lists it.
struct VersionRow {
  std::string label;
  // Under the label: command line path, update needed.
  std::string note;
  // The text it ships (release_id.h bits); 0 offers every language.
  uint32_t languages = 0;
  // Its files are ours to delete.
  bool can_delete = false;
};

// What the prompt is asked to offer. With versions installed it lists them
// (the library); `adding`, or an empty list, asks for a source instead.
struct GameDataPrompt {
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
  // The row Delete applies to, or -1.
  int selected_version = -1;
  // Picking a source for another version, over the library.
  bool adding = false;
  // Asking whether to delete: the question, shown with Cancel and Delete
  // Files.
  std::string confirm;
};

struct GameDataOptions {
  // The config game_data_root is persisted to; relative values resolve against
  // its folder.
  std::filesystem::path config_path;
  // The one launch flag file (StartScreenFlagPath).
  std::filesystem::path start_screen_flag;
  // The resolved game_data_root, the library root; empty for the default.
  std::filesystem::path game_data_root;
};

// Migrates and scans the library. True when it can start with no screen: a
// release is installed, none needs more work, and no screen was asked for.
bool UsePreparedGameData(const GameDataOptions& options);

// What the runtime starts with, once the library is started.
struct GameDirectories {
  // Mounted as game: World's folder, JP's when it is the only release, or the
  // command line's (GameDirectoryOverride).
  std::filesystem::path mounted;
  // JP's folder, or empty when it is not installed. Read for what only JP
  // has, whatever is mounted.
  std::filesystem::path jp;
};
const GameDirectories& StartedGameDirectories();

// Where a start screen request is kept, next to the user settings. A file
// rather than an environment variable, since Android restarts the app without
// handing the old environment on.
std::filesystem::path StartScreenFlagPath(const std::filesystem::path& user_settings_path);

// Makes the next launch show the start screen even with prepared game data, for
// that launch only; false withdraws the request.
void RequestStartScreenOnNextLaunch(const std::filesystem::path& flag, bool request);

// Asks through the intro screen (a message box where it cannot draw) until the
// library is started, or the player quits. Versions that still need work run
// their phases first. Returns at once: the main loop has to run for
// the screen to get input. `done` runs on the UI thread.
void AskForGameData(const GameDataOptions& options, rex::ui::WindowedAppContext& context,
                    std::function<void(bool ready)> done);

// Whether game files are being installed; the window must not
// close then.
bool IsExtractingGameData();

}  // namespace nocturne
