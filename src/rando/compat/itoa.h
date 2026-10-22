// itoa is an MSVC extension; xb_rando.cpp uses it with base 10 and 16.
#pragma once

#include <cstdio>

static inline char* itoa(int value, char* buf, int base) {
  std::snprintf(buf, 34, base == 16 ? "%x" : "%d", value);
  return buf;
}
