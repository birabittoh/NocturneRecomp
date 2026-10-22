// Hooks between the vendored SOTN_XB_RANDO core (rando_core.cpp) and the
// host side (rando_host.cpp). See rando_core.cpp.
#pragma once

#include <string>

namespace nocturne::rando::detail {

// Where XB_SaveSpoilers writes its file instead of "spoilers.txt" in the
// working directory.
void SetSpoilersPath(const std::string& path);

}  // namespace nocturne::rando::detail
