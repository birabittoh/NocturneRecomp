// Hooks between the vendored SOTN_XB_RANDO core (rando_core.cpp) and the
// host side (rando_host.cpp). See rando_core.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rando.h"

namespace nocturne::rando::detail {

// The path to pass to XB_Randomize: it opens the buffer set by SetMemoryXex
// instead of a file.
inline constexpr const char* kMemoryXexPath = "<memory>/Rando.xex";

// The buffer XB_Randomize patches, in the layout of the decrypted xex file.
// Accesses at or past `limit` fail and set MemoryXexOutOfRange. The offset of
// every byte written is appended to `writes`, if given.
void SetMemoryXex(std::vector<uint8_t>* data, size_t limit,
                  std::vector<size_t>* writes = nullptr);
bool MemoryXexOutOfRange();

// What XB_SaveSpoilers wrote to "spoilers.txt" during the last run.
std::string TakeSpoilers();

// Seeds the calling thread's rand() state; call on the thread that runs
// XB_Randomize, before it.
void SeedGeneratorRng(uint32_t seed);

// Runs the core functions behind `enhancements` against the buffer set by
// SetMemoryXex. Caller holds the core.
void WriteEnhancements(const Enhancements& enhancements);

}  // namespace nocturne::rando::detail
