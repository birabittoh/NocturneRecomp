// nocturnerecomp - play JP's voices while World's folder is mounted.
//
// With both releases installed the runtime mounts World as game:, so the
// guest reads World's English DATA\sound banks. When the voices setting picks JP
// (or auto, in Japanese), JP's DATA\sound folder (mounted as a device of its own) is linked over World's
// resolved DATA\sound path, the same directory-level shadow as
// font_overlay.h (and for the same reason: the open path only follows links
// on the parent directory). JP's banks are XACT version 42; xact_compat.cpp
// converts them as they are loaded.
//
// Swapping the files is not enough on its own: World's code reads each bank
// with a byte count from its own tables, into buffers sized for World's
// banks, and most of JP's are bigger, so lines at the end of a bank went
// missing. JP's own code is the same apart from those numbers. So the tables
// get JP's file sizes while JP's voices play (World's otherwise), the four
// wave bank buffers are allocated big enough for either release, and sound
// banks are read into buffers of our own (see the hooks in
// nocturnerecomp_jp_voices_hooks.toml).
//
// Nothing happens when JP is not installed, or when JP is the mounted folder
// already (its voices are then the only ones there are): JpVoicesAvailable()
// is false, and the settings hide the voices row.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

// Both releases are installed and World is mounted, so there is a choice.
bool JpVoicesAvailable();

// After the image is mapped and before the guest starts.
void InstallJpVoices(rex::Runtime* runtime);

// Applies a voices or language change: the tables and the link switch over, and
// each bank plays the new choice from its next load. Runs from the per-frame
// hook (GraphicsSettings_PerFrame), on the thread that loads the banks, so a
// load never pairs one release's byte count with the other's file.
void ApplyPendingVoices();

}  // namespace nocturne
