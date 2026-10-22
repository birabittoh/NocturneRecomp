// Stand-in for <process.h> off Windows: xb_rando.cpp starts its relic
// randomizer with _beginthread and polls XB_ThreadStatus until it finishes.
#pragma once

#include <cstdint>
#include <thread>

static inline uintptr_t _beginthread(void (*fn)(void*), unsigned /*stack*/, void* arg) {
  std::thread(fn, arg).detach();
  return 1;
}
