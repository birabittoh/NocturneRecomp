// Builds MottZilla's SOTN_XB_RANDO core (ext/SOTN_XB_RANDO, the Source/ files
// other than main.cpp, which is that project's Win32 GUI) into this exe.
// xb_rando.cpp #includes its own xb_candlesetup.h and xb_relic_locations.cpp,
// so it is compiled as one translation unit here, unmodified.
//
// Its two hard-wired file paths are redirected by a fopen wrapper rather than
// by editing the upstream source:
//   * "spoilers.txt" (written to the working directory) goes to the path set
//     with SetSpoilersPath.
// Its Win32 GUI hooks (LogMsg, DoMessages, ...) are provided by rando_host.cpp.
//
// Randomizer by MottZilla: https://github.com/MottZilla/SOTN_XB_RANDO

#include "rando_core.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifndef _WIN32
#include "compat/itoa.h"
#endif

namespace nocturne::rando::detail {
namespace {
std::mutex g_spoilers_mutex;
std::string g_spoilers_path;
}  // namespace

void SetSpoilersPath(const std::string& path) {
  std::lock_guard lock(g_spoilers_mutex);
  g_spoilers_path = path;
}

std::FILE* OpenForRando(const char* path, const char* mode) {
  if (std::strcmp(path, "spoilers.txt") == 0) {
    std::lock_guard lock(g_spoilers_mutex);
    if (!g_spoilers_path.empty()) {
      return std::fopen(g_spoilers_path.c_str(), mode);
    }
  }
  return std::fopen(path, mode);
}
}  // namespace nocturne::rando::detail

#define fopen(path, mode) nocturne::rando::detail::OpenForRando(path, mode)

#include "xb_rando.cpp"
