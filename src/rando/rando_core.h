// Hooks between the vendored SOTN_XB_RANDO core (rando_core.cpp) and the
// host side (rando_host.cpp). See rando_core.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nocturne::rando::detail {

// The path to pass to XB_Randomize: it opens the buffer set by SetMemoryXex
// instead of a file.
inline constexpr const char* kMemoryXexPath = "<memory>/Rando.xex";

// The buffer XB_Randomize patches, in the layout of the decrypted xex file.
// Accesses at or past `limit` fail and set MemoryXexOutOfRange.
void SetMemoryXex(std::vector<uint8_t>* data, size_t limit);
bool MemoryXexOutOfRange();

// What XB_SaveSpoilers wrote to "spoilers.txt" during the last run.
std::string TakeSpoilers();

// Seeds the calling thread's rand() state; call on the thread that runs
// XB_Randomize, before it.
void SeedGeneratorRng(uint32_t seed);

}  // namespace nocturne::rando::detail
