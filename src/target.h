// nocturnerecomp - ReXGlue Recompiled Project
//
// Which release's data game_data_root holds.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace nocturne {

// Latches the target from the resolved game directory. Call once, before the
// runtime is constructed.
void DetectTarget(const std::filesystem::path& game_data_root);

// The directory DetectTarget latched.
const std::filesystem::path& GameDataRoot();

// The platform and region that directory was installed from, as "Xbox Live
// Arcade, World", or empty when unknown. Hashes default.xex on first call.
const std::string& TargetRelease();

// That directory's library id ("xbla-world", "xbla-jp"), or empty when unknown.
const std::string& TargetReleaseId();

// What a mod.toml `target` may name to match this run: the platform ("xbla")
// and the release id. RuntimeConfig::game_targets, so the SDK leaves
// out mods made for another version. Empty before DetectTarget.
std::vector<std::string> GameTargets();

// For readers of mods.toml that run without the runtime's filtered list.
bool ModRunsOnTarget(const std::filesystem::path& mod_root);

}  // namespace nocturne
