#pragma once

// Reading and extracting an Xbox Live Arcade package (an STFS container with a
// LIVE, CON or PIRS header). Ported from the SDK's GameDataSelector
// (src/system/game_data_selector.cpp), where it lives in an anonymous namespace.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace nocturne {

// `fraction` outside [0,1] means unknown. When empty, an SDL progress window
// shows it.
using ExtractProgress =
    std::function<void(const std::string& title, float fraction, const std::string& detail)>;

// Moves a directory out of the way instead of deleting what a player put
// there; an empty one is removed.
bool MoveAside(const std::filesystem::path& dir);

// fs::rename, retried for a few seconds: Windows refuses to rename a folder
// while another process (an antivirus, the search indexer) has a file in it
// open, as happens for a moment right after the files are written.
void RenameRetrying(const std::filesystem::path& from, const std::filesystem::path& to,
                    std::error_code& ec);

// Case insensitive, as package file names are.
bool SameFileName(std::string_view a, std::string_view b);

// Whether `path` (a path, or an Android content URI) starts with an STFS magic.
bool IsXblaPackage(const std::string& path);

struct PackageHeader {
  uint32_t title_id = 0;
  // UTF-8, from the header's first display name.
  std::string display_name;
};

// Reads the package's title id and display name. Returns why it could not, for
// the player, or empty.
std::string ReadPackageHeader(const std::string& package, PackageHeader& out);

// Reads the file `name` from the root of `package` into `out`. Returns why it
// could not, for the player, or empty.
std::string ReadPackageFile(const std::string& package, std::string_view name,
                            std::vector<uint8_t>& out);

// Bytes extraction would write; 0 when the package cannot be read.
uint64_t MeasurePackage(const std::string& package);

// Extracts `package` into `out_dir`, which is replaced; a non empty one is
// moved aside, not deleted. Refuses a package whose root lacks
// `required_entry`. Goes through a sibling directory, so an interrupted run
// never leaves `out_dir` half written. Returns why it failed, for the player,
// or empty.
std::string ExtractPackage(const std::string& package, const std::filesystem::path& out_dir,
                           std::string_view required_entry, const ExtractProgress& progress);

}  // namespace nocturne
