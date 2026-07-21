// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <set>
#include <unordered_set>
#include <vector>

#include <rex/graphics/packet_disassembler.h>
#include <rex/system/kernel_state.h>

namespace nocturne {

// Implements the SotN-specific side of headless GPU emulation, driven by the
// SDK's generic rex::system::KernelState::HeadlessGpuHooks. With no
// GraphicsSystem loaded, nothing else owns the GPU register range or drains
// the ring buffer, so this class:
//  - Mirrors CP_RB_WPTR writes into the guest's read-pointer write-back
//    address, making the (nonexistent) GPU look infinitely fast.
//  - Walks and decodes newly-submitted PM4 packets (including the contents
//    of PM4_INDIRECT_BUFFER targets, where the real per-frame draw stream
//    lives), forwarding each one to a registered PacketCallback.
//  - Mirrors SCRATCH_UMSK/SCRATCH_ADDR/SCRATCH_REG0-7 writes into guest
//    memory, matching CommandProcessor::WriteRegister -- the guest D3D
//    runtime's swap-completion interrupt machinery depends on this mirror.
//  - Dispatches (or, if the guest's interrupt-callback slot isn't armed yet,
//    defers and later retries) the source-1 "swap completion" graphics
//    interrupt on PM4_INTERRUPT packets.
//
// Register this class with rex::system::kernel_state()->SetHeadlessGpuHooks
// once, before the guest thread starts submitting GPU commands.
class HeadlessGpuBridge {
 public:
  using PacketCallback =
      std::function<void(const rex::graphics::PacketInfo& info, const uint8_t* packet_base)>;

  explicit HeadlessGpuBridge(PacketCallback packet_callback);

  rex::system::KernelState::HeadlessGpuHooks BuildHooks();

 private:
  void OnRingBufferInit(uint32_t ptr, uint32_t size_log2);
  void OnRPtrWriteBackEnabled(uint32_t ptr, uint32_t block_size_log2);
  void OnMmioWrite(uint32_t addr, uint32_t value);
  uint32_t OnMmioRead(uint32_t addr);

  void ApplyScratchRegisterWrite(uint32_t index, uint32_t value);
  bool StreamCpuInterruptSlotArmed();
  void DispatchStreamCpuInterrupt(uint32_t cpu_mask);
  void DecodeRingBuffer(uint32_t new_wptr);
  void ForwardPacket(const rex::graphics::PacketInfo& info, const uint8_t* packet_base,
                     bool duplicate_ib_content);

  PacketCallback packet_callback_;

  std::atomic<uint32_t> ring_buffer_rptr_writeback_ptr_{0};
  std::atomic<uint32_t> ring_buffer_base_{0};
  std::atomic<uint32_t> ring_buffer_size_log2_{0};
  // Persistent cursor for the PM4 decode walk: packets can span multiple
  // CP_RB_WPTR writes, so this must survive across calls rather than
  // resetting to the most recent wptr each time.
  std::atomic<uint32_t> ring_buffer_decode_pos_{0};

  std::atomic<uint32_t> scratch_umsk_{0};
  std::atomic<uint32_t> scratch_addr_{0};
  // Last nonzero SCRATCH_ADDR ever written. The guest legitimately zeroes
  // the register in some phases while still arming the interrupt-callback
  // slot inside the block directly with the CPU, so the armed-slot check
  // must key off the block's location, not the register's current value.
  std::atomic<uint32_t> scratch_block_addr_{0};

  // Deferred command-stream CPU interrupts (PM4_INTERRUPT, source 1 = swap
  // completion): queued when decoded while the guest's interrupt-callback
  // slot is disarmed, retried opportunistically on later CP_RB_WPTR writes
  // (which happen continuously while headless, since nothing else drains
  // the ring) once it becomes armed.
  std::atomic<uint32_t> pending_cpu_interrupts_{0};
  std::atomic<uint32_t> pending_cpu_interrupt_mask_{0};
};

}  // namespace nocturne
