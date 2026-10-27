// nocturnerecomp - play JP's voices while World's folder is mounted.
//
// With both releases installed the runtime mounts World as game:, so the
// guest reads World's English DATA\sound banks. When jp_voices is on, JP's
// DATA\sound folder is mounted as a device of its own and World's resolved
// DATA\sound path is linked to it, the same directory-level shadow as
// font_overlay.h (and for the same reason: the open path only follows links
// on the parent directory). JP's banks are XACT version 42; xact_compat.cpp
// converts them as they are loaded.
//
// Nothing happens when JP is not installed, or when JP is the mounted folder
// already (its voices are then the only ones there are).
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

// Before the guest starts: the banks are opened at boot, so the setting
// takes effect on the next launch.
void InstallJpVoices(rex::Runtime* runtime);

}  // namespace nocturne
