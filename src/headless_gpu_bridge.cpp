// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "headless_gpu_bridge.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <xxhash.h>

#include <rex/graphics/register_file.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xvideo.h>

namespace nocturne {

using rex::graphics::PacketAction;
using rex::graphics::PacketCategory;
using rex::graphics::PacketDisassembler;
using rex::graphics::PacketInfo;
using rex::graphics::RegisterFile;

HeadlessGpuBridge::HeadlessGpuBridge(PacketCallback packet_callback)
    : packet_callback_(std::move(packet_callback)) {}

rex::system::KernelState::HeadlessGpuHooks HeadlessGpuBridge::BuildHooks() {
  rex::system::KernelState::HeadlessGpuHooks hooks;
  hooks.on_ring_buffer_init = [this](uint32_t ptr, uint32_t size_log2) {
    OnRingBufferInit(ptr, size_log2);
  };
  hooks.on_rptr_writeback_enabled = [this](uint32_t ptr, uint32_t block_size_log2) {
    OnRPtrWriteBackEnabled(ptr, block_size_log2);
  };
  hooks.on_mmio_write = [this](uint32_t addr, uint32_t value) { OnMmioWrite(addr, value); };
  hooks.on_mmio_read = [this](uint32_t addr) { return OnMmioRead(addr); };
  return hooks;
}

void HeadlessGpuBridge::OnRingBufferInit(uint32_t ptr, uint32_t size_log2) {
  ring_buffer_base_.store(ptr, std::memory_order_release);
  ring_buffer_size_log2_.store(size_log2, std::memory_order_release);
}

void HeadlessGpuBridge::OnRPtrWriteBackEnabled(uint32_t ptr, uint32_t /*block_size_log2*/) {
  ring_buffer_rptr_writeback_ptr_.store(ptr, std::memory_order_release);
}

uint32_t HeadlessGpuBridge::OnMmioRead(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      return 0;
  }
}

void HeadlessGpuBridge::ApplyScratchRegisterWrite(uint32_t index, uint32_t value) {
  // Mirrors CommandProcessor::WriteRegister: writes to SCRATCH_REG0-7
  // (0x0578-0x057F) whose bit is set in SCRATCH_UMSK (0x01DC) are mirrored
  // into guest memory at SCRATCH_ADDR (0x01DD) + reg*4. The guest D3D
  // runtime's swap-completion interrupt machinery depends on this: it embeds
  // type-0 writes of {completion fn, arg} to SCRATCH_REG4/5 in the command
  // stream, the mirror lands them in the interrupt-callback block its
  // graphics ISR reads on a source-1 (swap) interrupt, and the ISR then
  // calls that fn to count the swap as completed.
  if (index == 0x01DC) {
    scratch_umsk_.store(value, std::memory_order_release);
  } else if (index == 0x01DD) {
    scratch_addr_.store(value, std::memory_order_release);
    if (value) {
      scratch_block_addr_.store(value, std::memory_order_release);
    }
  } else if (index >= 0x0578 && index <= 0x057F) {
    uint32_t scratch_reg = index - 0x0578;
    uint32_t umsk = scratch_umsk_.load(std::memory_order_acquire);
    if ((1u << scratch_reg) & umsk) {
      uint32_t scratch_addr = scratch_addr_.load(std::memory_order_acquire);
      if (scratch_addr) {
        rex::memory::store_and_swap<uint32_t>(
            rex::system::kernel_memory()->TranslatePhysical((scratch_addr & 0x1FFFFFFF) + scratch_reg * 4),
            value);
      }
    }
  }
}

bool HeadlessGpuBridge::StreamCpuInterruptSlotArmed() {
  uint32_t scratch_addr = scratch_block_addr_.load(std::memory_order_acquire);
  if (!scratch_addr) {
    return false;
  }
  // Slot layout: scratch write-back block + 16 holds the completion-callback
  // function pointer the guest ISR invokes on a source-1 interrupt; 0 means
  // "nothing armed" (ISR silently ignores) and 0x0BADF00D is the explicit
  // disarmed filler (ISR traps with "Unanticipated CPU_INTERRUPT").
  uint32_t slot = rex::memory::load_and_swap<uint32_t>(
      rex::system::kernel_memory()->TranslatePhysical((scratch_addr & 0x1FFFFFFF) + 16));
  // Besides "empty" (0) and "disarmed" (0x0BADF00D), require the slot to
  // plausibly be a guest function pointer (4-aligned, in the 0x8xxxxxxx
  // virtual code range): a misdecoded packet stomping the scratch state can
  // make this read return arbitrary bytes, and dispatching then makes the
  // guest ISR call a junk pointer.
  return slot != 0 && slot != 0x0BADF00D && (slot & 0x3) == 0 && (slot >> 28) == 0x8;
}

void HeadlessGpuBridge::DispatchStreamCpuInterrupt(uint32_t cpu_mask) {
  // Armed-slot gate: on hardware the stream's preceding WAIT_REG_MEM
  // handshake guarantees the CPU has armed its completion-callback slot
  // before the interrupt fires. This walk ignores waits, so enforce the
  // same invariant directly. When the slot is disarmed at decode time, the
  // interrupt is deferred (retried on a later call once the slot arms)
  // rather than dropped, since the CPU may arm the slot after submitting
  // the stream but before our synchronous decode reaches the interrupt
  // packet.
  if (StreamCpuInterruptSlotArmed()) {
    for (uint32_t n = 0; n < 6; n++) {
      if (cpu_mask & (1u << n)) {
        rex::system::kernel_state()->DispatchGraphicsInterruptCallback(1, n);
      }
    }
    return;
  }
  pending_cpu_interrupt_mask_.store(cpu_mask, std::memory_order_release);
  // Cap the deferred backlog: hardware coalesces level-triggered interrupts,
  // and batch-flushing thousands of stale ones when the slot finally
  // re-arms would advance the guest's swaps-completed count far past
  // reality in one step.
  constexpr uint32_t kMaxPendingCpuInterrupts = 3;
  uint32_t pending = pending_cpu_interrupts_.load(std::memory_order_acquire);
  while (pending < kMaxPendingCpuInterrupts &&
         !pending_cpu_interrupts_.compare_exchange_weak(pending, pending + 1,
                                                        std::memory_order_release,
                                                        std::memory_order_acquire)) {
  }
}

namespace {
// A decoded packet whose register writes land outside the Xenos register
// file (0x5003 dwords) cannot be a real packet -- it's non-PM4 data (float
// constants, vertex data, stale buffer contents) misinterpreted as a packet
// header. Everything decoded after such a packet is equally garbage.
bool PacketActionsPlausible(const PacketInfo& info) {
  for (const auto& action : info.actions) {
    if (action.type == PacketAction::Type::kRegisterWrite &&
        action.register_write.index >= 0x5003) {
      return false;
    }
  }
  return true;
}
}  // namespace

void HeadlessGpuBridge::ForwardPacket(const PacketInfo& info, const uint8_t* packet_base,
                                      bool duplicate_ib_content) {
  for (const auto& action : info.actions) {
    if (action.type == PacketAction::Type::kRegisterWrite) {
      ApplyScratchRegisterWrite(action.register_write.index, action.register_write.value);
    }
  }
  if (info.type_info && std::strcmp(info.type_info->name, "PM4_INTERRUPT") == 0) {
    DispatchStreamCpuInterrupt(rex::memory::load_and_swap<uint32_t>(packet_base + 4));
  }
  // Duplicate-content indirect buffers still forward their swap packets (the
  // consumer's present pacing needs every frame boundary), just not the
  // redundant draw content.
  if (packet_callback_ && (!duplicate_ib_content ||
                          (info.type_info && info.type_info->category == PacketCategory::kSwap))) {
    packet_callback_(info, packet_base);
  }
}

void HeadlessGpuBridge::OnMmioWrite(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;
  ApplyScratchRegisterWrite(r, value);

  if (r == 0x01C5) {  // CP_RB_WPTR
    DecodeRingBuffer(value);
  }

  // Opportunistically retry any deferred swap-completion interrupt: this
  // fires on every CP_RB_WPTR write, which happens continuously while
  // headless (nothing else drains the ring), so the retry latency is
  // negligible in practice.
  if (pending_cpu_interrupts_.load(std::memory_order_acquire) > 0 &&
      StreamCpuInterruptSlotArmed()) {
    pending_cpu_interrupts_.fetch_sub(1, std::memory_order_release);
    uint32_t mask = pending_cpu_interrupt_mask_.load(std::memory_order_acquire);
    for (uint32_t n = 0; n < 6; n++) {
      if (mask & (1u << n)) {
        rex::system::kernel_state()->DispatchGraphicsInterruptCallback(1, n);
      }
    }
  }
}

void HeadlessGpuBridge::DecodeRingBuffer(uint32_t new_wptr) {
  // No real GPU drains the ring buffer, so make it look infinitely fast:
  // immediately mirror the just-submitted write pointer back into the
  // read-pointer write-back address -- this is what unblocks guest code
  // spinning on read/write pointers matching.
  uint32_t writeback_ptr = ring_buffer_rptr_writeback_ptr_.load(std::memory_order_acquire);
  if (writeback_ptr) {
    rex::memory::store_and_swap<uint32_t>(rex::system::kernel_memory()->TranslatePhysical(writeback_ptr),
                                          new_wptr);
  }

  uint32_t base = ring_buffer_base_.load(std::memory_order_acquire);
  if (!base) {
    return;
  }
  // VdInitializeRingBuffer's size_log2 is log2 of the size in *bytes minus
  // 3* -- the real ring is `1 << (size_log2 + 3)` bytes, i.e.
  // `1 << (size_log2 + 1)` dwords.
  uint32_t size_log2 = ring_buffer_size_log2_.load(std::memory_order_acquire);
  uint32_t size_dwords = 1u << (size_log2 + 1);

  // Decode from the persistent decode cursor, not from the wptr seen on the
  // previous call: packets routinely span multiple CP_RB_WPTR writes.
  uint32_t decode_pos = ring_buffer_decode_pos_.load(std::memory_order_acquire);
  uint32_t available = (new_wptr - decode_pos) & (size_dwords - 1);
  if (!available) {
    return;
  }

  // Copy the entire ring (wrapped, starting at decode_pos), padded past one
  // full lap by the largest possible packet body, into a flat local buffer
  // so a speculative packet-body read never goes out of bounds.
  constexpr uint32_t kMaxPacketDwords = 1 + (0x3FFF + 1);
  std::vector<uint32_t> raw(size_dwords + kMaxPacketDwords);
  auto* memory = rex::system::kernel_memory();
  for (uint32_t i = 0; i < raw.size(); ++i) {
    uint32_t ring_index = (decode_pos + i) & (size_dwords - 1);
    std::memcpy(&raw[i], memory->TranslatePhysical(base + ring_index * 4), sizeof(uint32_t));
  }

  const uint8_t* buf = reinterpret_cast<const uint8_t*>(raw.data());
  uint32_t offset_dwords = 0;
  // Per-call dedup: the guest resubmits the same handful of indirect-buffer
  // jumps in a tight spin (nothing ever drains the ring headlessly), which
  // would otherwise redraw the same content redundantly every call.
  std::set<std::pair<uint32_t, uint32_t>> decoded_ibs_this_call;
  std::unordered_set<uint64_t> decoded_ib_content_hashes_this_call;

  while (offset_dwords < available) {
    PacketInfo info;
    if (!PacketDisassembler::DisasmPacket(buf + offset_dwords * 4, &info, memory)) {
      REXGPU_DEBUG("headless pm4: [{}] <unrecognized packet, aborting decode>",
                   decode_pos + offset_dwords);
      break;
    }
    uint32_t packet_dwords = info.count;
    if (offset_dwords + packet_dwords > available) {
      break;
    }
    if (!PacketActionsPlausible(info)) {
      REXGPU_INFO(
          "headless pm4: [{}] <implausible register-write packet, dropping rest of window>",
          decode_pos + offset_dwords);
      offset_dwords = available;
      break;
    }

    bool is_indirect_buffer =
        info.type_info != nullptr && std::strcmp(info.type_info->name, "PM4_INDIRECT_BUFFER") == 0;
    uint32_t ib_ptr = 0, ib_len = 0;
    if (is_indirect_buffer) {
      // Raw dword is a GPU-space pointer -- mask to the 29-bit physical
      // range before treating it as a CPU address.
      ib_ptr = rex::memory::load_and_swap<uint32_t>(buf + (offset_dwords + 1) * 4) & 0x1FFFFFFF;
      ib_len = rex::memory::load_and_swap<uint32_t>(buf + (offset_dwords + 2) * 4) & 0xFFFFF;
    }

    ForwardPacket(info, buf + offset_dwords * 4, /*duplicate_ib_content=*/false);

    bool decode_ib_content = is_indirect_buffer && ib_ptr && ib_len;
    bool duplicate_ib_content = false;
    if (decode_ib_content) {
      duplicate_ib_content = !decoded_ibs_this_call.insert({ib_ptr, ib_len}).second;
      if (!duplicate_ib_content) {
        const uint8_t* ib_content_for_hash =
            reinterpret_cast<const uint8_t*>(memory->TranslatePhysical(ib_ptr));
        uint64_t content_hash = XXH3_64bits(ib_content_for_hash, size_t(ib_len) * 4);
        duplicate_ib_content = !decoded_ib_content_hashes_this_call.insert(content_hash).second;
      }
    }
    if (decode_ib_content) {
      const uint8_t* ib_buf = reinterpret_cast<const uint8_t*>(memory->TranslatePhysical(ib_ptr));
      uint32_t ib_offset = 0;
      // Defensive cap: this walks guest-controlled data, so a misparsed
      // header must not be able to spin forever. Sized to never truncate a
      // legitimate walk (ib_len is capped at 0xFFFFF dwords).
      constexpr uint32_t kMaxIbPackets = 0x100000;
      uint32_t ib_packets = 0;
      while (ib_offset < ib_len && ib_packets++ < kMaxIbPackets) {
        PacketInfo ib_info;
        if (!PacketDisassembler::DisasmPacket(ib_buf + ib_offset * 4, &ib_info, memory)) {
          break;
        }
        if (ib_info.count == 0) {
          break;
        }
        if (ib_offset + ib_info.count > ib_len) {
          break;
        }
        if (!PacketActionsPlausible(ib_info)) {
          break;
        }
        ForwardPacket(ib_info, ib_buf + ib_offset * 4, duplicate_ib_content);
        ib_offset += ib_info.count;
      }
    }

    offset_dwords += packet_dwords;
  }

  ring_buffer_decode_pos_.store((decode_pos + offset_dwords) & (size_dwords - 1),
                                std::memory_order_release);
}

}  // namespace nocturne
