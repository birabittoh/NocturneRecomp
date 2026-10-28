// nocturnerecomp - ReXGlue Recompiled Project
//
// The guest image (default.xex, its code stripped: scripts/gen-guest-image.py)
// is linked into the executable when NOCTURNE_EMBED_GUEST_IMAGE is on. It is
// served from a VFS device of its own and the module is loaded from there, so
// the game folder's default.xex is only read to tell the versions apart
// (src/installer/release_id.h).

#pragma once

#include <string>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace nocturne {

// Mounts the embedded image and returns the guest path to load it from.
std::string MountGuestImage(rex::filesystem::VirtualFileSystem* file_system);

}  // namespace nocturne
