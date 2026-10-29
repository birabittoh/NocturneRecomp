// Front end vs. running game, shared by everything that has to tell the
// title / main menu from file select and gameplay.
//
// The app keeps a current-state pointer next to the two states it switches
// between (set up by sub_824FAEF8): the front end (title, main menu, its
// settings and achievements screens) and the game (the PS1 game: file
// select, name entry, gameplay, and the Xbox pause menu over it). The UI
// manager's update (sub_825AAE90) and pause handling (sub_825ABBD8) branch
// on exactly these comparisons. Logged live: the current state switches to
// the game on New Game a frame before the PS1 side resets, and back to the
// front end the moment Quit to Title is chosen.
//
// Don't use the PS1 side's own state for this: g_Status (hp_max) and the room
// index keep their last gameplay values after Quit to Title until the PS1
// game starts again.
//
// Also here: the app object's byte at +29 (InGameLatchAddress), which the
// front-end screens use to tell their main-menu instance from their pause-menu
// one. It is raised while the Xbox pause menu (or a screen opened from it) is
// up and is 0 during plain gameplay, so it is *not* a "save is loaded" flag.
#pragma once

#include <cstdint>

namespace nocturne {

inline constexpr uint32_t kAppObjectPtrVanilla = 0x82E4F808u;
inline constexpr uint32_t kAppObjectPtrAddr = kAppObjectPtrVanilla;
// The state words follow the app object pointer Mods get them as the "app.state" registry address: three
// BE pointers, current / game / front end (rexmod/player_status.h reads it).
inline constexpr uint32_t kCurrentStatePtrAddr = kAppObjectPtrAddr + 4;
inline constexpr uint32_t kGameStatePtrAddr = kAppObjectPtrAddr + 8;
inline constexpr uint32_t kFrontEndStatePtrAddr = kAppObjectPtrAddr + 12;
inline constexpr uint32_t kInGameLatchOffset = 29;

inline uint32_t ReadGuestPtr(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// True from New Game (or Load) until Quit to Title: file select, gameplay and
// the pause menu. `base` is the guest memory base.
inline bool GameRunning(const uint8_t* base) {
  const uint32_t current = ReadGuestPtr(base, kCurrentStatePtrAddr);
  return current && current == ReadGuestPtr(base, kGameStatePtrAddr);
}

// True at the title / main menu. False before the app has set its states up
// (the very first frames), which is neither.
inline bool InFrontEnd(const uint8_t* base) {
  const uint32_t current = ReadGuestPtr(base, kCurrentStatePtrAddr);
  return current && current == ReadGuestPtr(base, kFrontEndStatePtrAddr);
}

// Address of the menu latch, or 0 before the app object exists.
inline uint32_t InGameLatchAddress(const uint8_t* base) {
  const uint32_t app_object = ReadGuestPtr(base, kAppObjectPtrAddr);
  return app_object ? app_object + kInGameLatchOffset : 0;
}

}  // namespace nocturne
