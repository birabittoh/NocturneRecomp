#pragma once

// Asks the player for a file to read or a place to write one. Desktop uses
// SDL's dialogs. Android goes through NocturneRecompActivity's Storage Access
// Framework bridge, staging every transfer through a plain cache file so no
// content:// URI reaches the save or content code. One request at a time.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace rex::ui {
class WindowedAppContext;
}

namespace nocturne {

// `read` runs on the UI thread with a local copy of the picked file, which
// may be gone once it returns; nullopt when the player cancelled.
void PickFileToRead(rex::ui::WindowedAppContext& context,
                    std::function<void(std::optional<std::filesystem::path>)> read);

// `write` runs on the UI thread and fills the local file it is given,
// returning an error or empty. `done` then gets that error, the copy's, ""
// on success, or nullopt when the player cancelled. `staging_dir` holds the
// file on its way out where the destination is not a local path.
void PickFileToWrite(rex::ui::WindowedAppContext& context, const std::string& suggested_name,
                     const std::filesystem::path& staging_dir,
                     std::function<std::string(const std::filesystem::path&)> write,
                     std::function<void(std::optional<std::string>)> done);

}  // namespace nocturne
