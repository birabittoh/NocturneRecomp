// Stand-in for <windows.h> when compiling SOTN_XB_RANDO's xb_rando.cpp on a
// non-Windows host (the randomizer core only needs HANDLE from it). On
// Windows the real header is used and this directory is not on the include
// path.
#pragma once

#include <cstdint>

typedef void* HANDLE;
