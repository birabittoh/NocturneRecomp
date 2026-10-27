#pragma once

// Which release a package or folder is, from the SHA-256 of its default.xex.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace nocturne {

// Release::languages: the text a release ships, one bit per res/lang file.
enum : uint32_t {
  kTextJa = 1u << 0,
  kTextEn = 1u << 1,
  kTextFr = 1u << 2,
  kTextIt = 1u << 3,
  kTextDe = 1u << 4,
  kTextEs = 1u << 5,
  kTextAll = (1u << 6) - 1,
};

// The bit for a res/lang code, or 0.
uint32_t TextLanguageBit(std::string_view code);

struct Release {
  // The library folder an install goes to: "xbla-world", "xbla-jp".
  const char* id;
  const char* name;
  const char* file;
  const char* file_sha256;
  bool supported;
  uint32_t languages;
  // Whether the recompiled image can run it. A release that is supported but
  // not startable is identified and extracted into the library, then listed
  // as not playable yet.
  bool startable;
};

// The release whose identifying file has this hash, or null.
const Release* FindRelease(std::string_view file_sha256);

// The release with this library id, or null.
const Release* FindReleaseById(std::string_view id);

// Lowercase hex.
std::string Sha256Hex(std::span<const uint8_t> data);

}  // namespace nocturne
