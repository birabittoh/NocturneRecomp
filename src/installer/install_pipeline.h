#pragma once

// The install as independent phases, each deciding for itself whether the
// directory needs it. Today there is one, extracting an XBLA package into game
// files; the hooks for later ones (NeedsConversion, NeedsPatching, IsOutdated)
// stay so a future step for another release can slot in.

#include <filesystem>
#include <functional>
#include <string>

#include "xbla_package.h"

namespace nocturne {

enum class Phase { kExtract };
inline constexpr int kInstallPhaseCount = 1;

enum class PhaseState { kPending, kActive, kDone, kSkipped };

const char* PhaseName(Phase phase);

using PhaseReport = std::function<void(Phase, PhaseState)>;

struct InstallHooks {
  // Per work item; unknown fraction is outside [0,1].
  ExtractProgress progress;
  PhaseReport phase;
};

// Whether `dir` holds game files.
bool IsGameDirectory(const std::filesystem::path& dir);

// Whether the later phases still have work on `dir`.
bool NeedsConversion(const std::filesystem::path& dir);
bool NeedsPatching(const std::filesystem::path& dir);

// Whether `dir` was extracted in a way only extracting the game again fixes.
bool IsOutdated(const std::filesystem::path& dir);

struct SourceInfo {
  // The release, for the player; empty when not known.
  std::string release;
  // Why `picked` cannot be installed, or empty.
  std::string error;
  // The text it ships (release_id.h bits), 0 when not known.
  uint32_t languages = 0;
  // Disk space Install needs at its peak, 0 when it writes next to nothing.
  uint64_t required_bytes = 0;
  // The library folder name (Release::id), empty when not known.
  std::string id;
};

// Identifies `picked` (as Install takes it, or a game directory) without
// extracting anything.
SourceInfo IdentifySource(const std::string& picked);

// Runs the phases on `picked` (an XBLA package, a path or an Android content
// URI), extracting into `assets`. Returns why it failed, for the player, or
// empty with `dir` set to the ready game directory.
std::string Install(const std::string& picked, const std::filesystem::path& assets,
                    const InstallHooks& hooks, std::filesystem::path& dir);

// The phases after extraction alone, for a directory already extracted.
std::string InstallDirectory(const std::filesystem::path& dir, const InstallHooks& hooks);

}  // namespace nocturne
