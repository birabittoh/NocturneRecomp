// The randomizer's code patches, compiled: scripts/gen-rando-hooks.py turns
// every instruction the randomizer core can write (and the code caves it
// writes) into native code behind a [[midasm_hook]] at the patched address
// (config/nocturnerecomp_rando_hooks.toml). A hook runs the patched version while the
// world in guest memory has changed its instruction, and lets the recompiled
// one run otherwise.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace nocturne::rando_hooks {

struct Report {
  // Sites running a patched instruction: (address, (variant + 1) << 16 | immediate).
  std::vector<std::pair<uint32_t, uint32_t>> active_sites;
  std::vector<uint32_t> unknown;  // patched sites holding a value no hook knows
};

// Call once with the guest memory base, before guest code runs: hooks for
// sites with two ways out run the original instruction through it too.
void Init(uint8_t* base);

// Call after writing a world into guest memory, before guest code runs again.
// `written(addr)` is the mask of the bytes the world writes in the instruction
// word at `addr` (0xFF000000: the first; 0: none); each such site runs the
// patch it holds from now on. Bytes the world doesn't write are taken to be
// the original instruction's, whatever guest memory holds there.
void Activate(uint8_t* base, const std::function<uint32_t(uint32_t)>& written, Report* report);

// Back to the recompiled instructions everywhere. Call before restoring the
// base image.
void Deactivate();

// Whether `addr` lies in an instruction a hook handles.
bool IsSite(uint32_t addr);

}  // namespace nocturne::rando_hooks
