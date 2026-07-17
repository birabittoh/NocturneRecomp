// Minimal PPC32 (Xenon) interpreter over rex's PPCContext/guest memory.
//
// Exists so a custom patched xex (see src/rando_xex.h) can change *code* at
// runtime: recompiled functions execute natively and never re-read .text
// from guest memory, so byte patches to instructions are inert unless the
// affected functions are re-executed some other way. Rather than shipping
// pre-recompiled override bodies for one specific patch (the previous
// build-time approach), the affected functions are overridden with a thunk
// that interprets their instructions straight from guest memory -- which
// holds the patched image -- calling back into native recompiled code for
// everything they call (via rex::runtime::ResolveIndirectFunction, so calls
// to other overridden functions land on their interpreter thunks too).
//
// Semantics deliberately mirror the SDK codegen's generated C++ (XenonRecomp
// conventions: 64-bit registers with 32-bit-op zero/sign-extension quirks,
// xer.ca formulas, fpscr flush handling) so an interpreted function behaves
// bit-for-bit like its recompiled equivalent would.
//
// Coverage is the integer + scalar-FP subset game logic uses; no VMX, no
// privileged ops. Use ScanFunction first: it walks all statically reachable
// instructions from the entry point and reports the first unsupported one,
// so callers can fall back to the vanilla native function (with a warning)
// instead of misexecuting. If execution still reaches an unsupported
// instruction at runtime (e.g. via a jump-table target the scan couldn't
// see), the interpreter logs the address/opcode and aborts loudly.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <rex/ppc/context.h>

namespace nocturne {

// Registers guest address ranges outside a function's own [start, end) that
// interpretation may flow into and out of with plain gotos: randomizer code
// caves that hook a function via "b cave ... b back-into-function-body"
// trampolines. Branch-backs land mid-function, so they can't be dispatched
// as calls -- instead, a non-link branch whose target lies in one of these
// ranges (or back inside the function bounds) just continues interpreting
// at the target. PpcScanFunction follows such branches too, so the
// unsupported-instruction check covers the cave code a patched function
// flows through. Call once (not thread-safe) before installing thunks;
// ranges must be disjoint.
void PpcSetInterpretableRanges(std::vector<std::pair<uint32_t, uint32_t>> ranges);

// Walks statically reachable instructions of [start, end) (guest addresses,
// instructions read from guest memory at `base`) and returns true if the
// interpreter supports all of them. On failure, *bad_pc/*bad_insn identify
// the first unsupported instruction.
bool PpcScanFunction(uint8_t* base, uint32_t start, uint32_t end, uint32_t* bad_pc,
                     uint32_t* bad_insn);

// Interprets the guest function at [start, end) until it returns. Calls out
// of the function (bl/bctrl/tail calls) dispatch to whatever host function
// is registered for the target address.
void PpcInterpret(PPCContext& ctx, uint8_t* base, uint32_t start, uint32_t end);

}  // namespace nocturne
