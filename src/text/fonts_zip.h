// nocturnerecomp - read DATA\fonts\fonts.zip, and add entries to it.
//
// Enough of the zip format for the game's fonts.zip: a single disk, no zip64,
// entries stored or deflated. Shared by jp_artwork (which swaps entries in
// guest memory) and font_overlay (which serves the zip with entries JP's
// release lacks).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nocturne {

struct ZipEntry {
  std::string name;  // without any directory
  uint32_t crc = 0;
  uint32_t size = 0;
  uint16_t method = 0;
  size_t data = 0;  // offset of the compressed bytes
  uint32_t compressed = 0;
};

// The entries of a single-disk zip without zip64. Empty when it is anything
// else.
std::vector<ZipEntry> ParseZip(const std::vector<uint8_t>& zip);

// An entry's uncompressed bytes. Empty when it can't be read.
std::vector<uint8_t> Extract(const std::vector<uint8_t>& zip, const ZipEntry& entry);

// The zip with `data` added as a stored entry called `name`. Empty when the
// zip can't be read.
std::vector<uint8_t> AddStoredEntry(const std::vector<uint8_t>& zip, std::string_view name,
                                    const std::vector<uint8_t>& data);

}  // namespace nocturne
