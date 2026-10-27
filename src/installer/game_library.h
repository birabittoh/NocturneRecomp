#pragma once

// The installed versions of the game. Each one is a flat game tree in its own
// folder inside the library root (the resolved game_data_root, "xbla-world",
// "xbla-jp"), extracted there from its XBLA package. The pick is the
// game_version cvar, kept in settings.toml, since launchers own
// game_data_root.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nocturne {

struct GameVersion {
  // The folder name inside the root.
  std::string key;
  // Release::id, empty when not known.
  std::string id;
  // The release name, empty when not known.
  std::string release;
  // release_id.h bits, 0 when not known.
  uint32_t languages = 0;
  std::filesystem::path dir;
  // A game directory given on the command line, not kept as a link.
  bool temporary = false;
  // Conversion or patching still has work on it.
  bool needs_phases = false;
  // Unpacked by an older conversion: only unpacking the game again fixes it.
  bool outdated = false;
  // Release::startable; an unknown release is let through.
  bool startable = true;
};

// Where SaveSelectedVersion writes, and where MigrateLibrary drops the
// settings older builds kept.
void BindLibrarySettings(const std::filesystem::path& user_settings_path);

// Moves single version installs (the root itself, or legacy `candidates`)
// into version folders, and drops the linked folders older builds kept (their
// files are left where they are). Finishes a move an earlier launch was killed
// in. Cheap when there is nothing to do.
void MigrateLibrary(const std::filesystem::path& root,
                    const std::vector<std::filesystem::path>& candidates);

// The versions in the root, in release order.
std::vector<GameVersion> ScanLibrary(const std::filesystem::path& root);

// The version `key` names (a folder name, or any game directory, which is
// then added as a temporary row), or -1.
int FindVersion(std::vector<GameVersion>& versions, std::string_view key);

// A free folder for `id` in the root: <id>, then <id>-2... "other" for none.
std::filesystem::path FreeVersionDir(const std::filesystem::path& root, std::string id);

// Records what `dir` holds, so later scans do not hash its executable.
void WriteVersionInfo(const std::filesystem::path& dir, const std::string& id,
                      const std::string& release, uint32_t languages);

// The release name nocturne-version.toml records, or empty.
std::string ReadVersionRelease(const std::filesystem::path& dir);
// The release id it records, or empty.
std::string ReadVersionId(const std::filesystem::path& dir);

// Keeps <root>/default.xex a copy of the World version's executable
// for the build and the launcher, or removes it when none is left. A
// default.xex we did not put there is never touched.
void RefreshRootExecutable(const std::filesystem::path& root,
                           const std::vector<GameVersion>& versions);

std::string SelectedVersion();
// Sets game_version and saves it; empty clears it.
void SaveSelectedVersion(const std::string& key);

// For the start screen: "Xbox Live Arcade, World", or the folder name.
std::string VersionLabel(const GameVersion& version);

}  // namespace nocturne
