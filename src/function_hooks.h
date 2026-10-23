#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

// Replace-or-wrap a recompiled function through a [[midasm_hook]] on its first instruction.
//
//   [[midasm_hook]]
//   address = 0x825D32B0
//   name = "NativeOptions_ListSetupHook"
//   registers = ["r3"]
//   return_on_true = true
//
//   bool NativeOptions_ListSetupHook(PPCRegister& r3) {
//     return nocturne::fnhook::Enter(r3, 0x825D32B0, &NativeOptions_ListSetup);
//   }
//
// Enter runs the replacement (same signature as a recompiled function) and returns true, so
// the generated code returns straight away. The replacement calls the original body through
// CallOriginal, which arms a one-shot pass-through so that the very next entry at that address
// returns false and lets the recompiled body run.
//
// The hook must list r3 so the real PPCContext of the caller can be recovered from it.
namespace nocturne::fnhook {

inline thread_local uint32_t g_pass_through = 0;

inline PPCContext& ContextFromR3(PPCRegister& r3) {
  return *reinterpret_cast<PPCContext*>(reinterpret_cast<char*>(&r3) - offsetof(PPCContext, r3));
}

inline bool Enter(PPCRegister& r3, uint32_t address, PPCFunc* replacement) {
  if (g_pass_through == address) {
    g_pass_through = 0;
    return false;
  }
  PPCContext& ctx = ContextFromR3(r3);
  replacement(ctx, REX_KERNEL_STATE()->memory()->virtual_membase());
  return true;
}

// Calls the recompiled body of the function hooked at `address`, bypassing its own hook.
inline void CallOriginal(uint32_t address, PPCContext& ctx, uint8_t* base) {
  auto* fn = REX_KERNEL_STATE()->function_dispatcher()->GetFunction(address);
  if (!fn) {
    return;
  }
  g_pass_through = address;
  fn(ctx, base);
  g_pass_through = 0;
}

}  // namespace nocturne::fnhook

// Initialiser for the `g_original_*_fn` pointers: forwards to CallOriginal for `address`.
#define NOCTURNE_ORIGINAL_FN(address) \
  (+[](PPCContext& ctx, uint8_t* base) { ::nocturne::fnhook::CallOriginal(address, ctx, base); })
