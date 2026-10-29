// nocturnerecomp - ReXGlue Recompiled Project
//
// The guest image (World's default.xex, its code stripped) is built at
// startup when NOCTURNE_EMBED_GUEST_IMAGE is on, from the game folder's own
// default.xex, World's or JP's, and a recipe linked into the executable
// (scripts/guest_recipe.py). No game data is linked in. It is served from a
// VFS device of its own and the module is loaded from there.

#pragma once

#include <filesystem>
#include <string>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace nocturne {

// Builds the image from `xex_path`, mounts it and returns the guest path to
// load it from; empty if `xex_path` isn't a release the recipe knows.
std::string MountGuestImage(rex::filesystem::VirtualFileSystem* file_system,
                            const std::filesystem::path& xex_path);

}  // namespace nocturne
