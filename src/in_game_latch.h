// The game's "a save is loaded" latch, shared by everything that has to tell
// the front end (title, main menu, file select) from gameplay.
//
// The app object is a pointer *stored at* kAppObjectPtrAddr, not the object
// itself; the latch is the byte at +29 inside it. The front-end watchdog
// clears it on its way out, and the settings screen reads it to decide which
// page it exits to. It is set once a save (or a new game) is loaded.
#pragma once

#include <cstdint>

namespace nocturne {

#ifdef NOCTURNE_TU
inline constexpr uint32_t kAppObjectPtrAddr = 0x82E4F5C8u;
#else
inline constexpr uint32_t kAppObjectPtrAddr = 0x82E4F808u;
#endif
inline constexpr uint32_t kInGameLatchOffset = 29;

// Address of the latch, or 0 before the app object exists. `base` is the
// guest memory base.
inline uint32_t InGameLatchAddress(const uint8_t* base) {
  const uint8_t* p = base + kAppObjectPtrAddr;
  const uint32_t app_object =
      (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  return app_object ? app_object + kInGameLatchOffset : 0;
}

inline bool SaveLoaded(const uint8_t* base) {
  const uint32_t latch = InGameLatchAddress(base);
  return latch && base[latch] != 0;
}

}  // namespace nocturne
