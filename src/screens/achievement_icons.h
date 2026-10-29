// nocturnerecomp - publish the achievement icons into the game's own UI image
// bank, so guest widgets can draw them like any other UI texture.
//
// The achievement art was never shipped in the game's resource banks -- none of
// uiresource/titleresource/sprites contains the ACHIEVEMENT_1..ACHIEVEMENT_12
// names CScreenAchievement looks up, which is a large part of why that screen is
// dead code in the retail game (see achievements_screen.h). The art does exist,
// though: it is in the XEX's own XDBF metadata, which is where the SDK's
// AchievementIconCache gets the icons it draws in the ImGui overlay.
//
// So the icons are made native rather than composited on top: decode the XDBF
// image host-side, build a real guest texture out of it, wrap that in the same
// 20-byte image object the resource loader produces, and append it to the bank
// under the name the screen already asks for. From that point the guest's own
// lookup returns a normal image and the widgets draw it themselves -- fades,
// draw ordering and scaling all included.
#pragma once

#include <cstdint>
#include <span>

namespace rex::system {
struct AchievementInfo;
}  // namespace rex::system

namespace nocturne {

// Registers an ACHIEVEMENT_<n> entry in the guest image bank for each
// achievement, numbered from 1 in the order given. Returns how many were added.
//
// Safe to call more than once; entries are only added the first time. Needs no
// guest thread context -- everything is host-side decoding plus guest memory
// writes.
uint32_t RegisterAchievementIcons(uint8_t* base, uint32_t bank,
                                  std::span<const rex::system::AchievementInfo> achievements);

}  // namespace nocturne
