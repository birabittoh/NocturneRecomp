#include "generated/nocturnerecomp_init.h"

#include <cstring>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

// ---------------------------------------------------------------------------
// XACT content version 42 banks on the version 41 runtime.
//
// The game's XACT runtime reads version 41 banks (World's DATA/sound). The JP
// release's banks are version 42, which differs only by one extra header
// field; every offset in a bank is absolute and the body layout is the same.
// So a 42 header is rewritten to 41 in place, before the runtime reads it,
// by dropping that field and sliding the rest of the header over it. The body
// stays where it is, so its offsets stay valid. Version 41 banks pass
// through untouched, and a converted header reads as 41, so it's idempotent.
//
// Wired up in config/nocturnerecomp_xact_hooks.toml.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kSoundBankMagic = 0x4B424453;  // "KBDS"
constexpr uint32_t kWaveBankMagic = 0x444E4257;   // "DNBW"
constexpr uint32_t kVersion41 = 41;
constexpr uint32_t kVersion42 = 42;

uint8_t* GuestPtr(uint32_t guest_address) {
  return REX_KERNEL_STATE()->memory()->TranslateVirtual(guest_address);
}

uint32_t ReadU32(const uint8_t* p) {
  return rex::memory::load_and_swap<uint32_t>(p);
}

uint16_t ReadU16(const uint8_t* p) {
  return rex::memory::load_and_swap<uint16_t>(p);
}

// Sound bank: magic, u16 content version, then version 42 adds a u16 tool
// version before the CRC. The header ends after the 64-byte bank name, at
// 0x94 in version 42 (0x92 in 41); bodies start well past it.
void ConvertSoundBank(uint32_t guest_buffer) {
  if (!guest_buffer)
    return;
  uint8_t* bank = GuestPtr(guest_buffer);
  if (ReadU32(bank) != kSoundBankMagic || ReadU16(bank + 4) != kVersion42)
    return;
  constexpr size_t kHeaderEnd42 = 0x94;
  std::memmove(bank + 6, bank + 8, kHeaderEnd42 - 8);
  rex::memory::store_and_swap<uint16_t>(bank + 4, kVersion41);
  REXLOG_INFO("xact: converted a version 42 sound bank at {:08X} to 41", guest_buffer);
}

// Wave bank: magic, u32 version, then version 42 adds a u32 header version
// before the five {offset, length} segments.
void ConvertWaveBank(uint32_t guest_buffer) {
  if (!guest_buffer)
    return;
  uint8_t* bank = GuestPtr(guest_buffer);
  if (ReadU32(bank) != kWaveBankMagic || ReadU32(bank + 4) != kVersion42)
    return;
  constexpr size_t kSegmentsSize = 5 * 8;
  std::memmove(bank + 8, bank + 12, kSegmentsSize);
  rex::memory::store_and_swap<uint32_t>(bank + 4, kVersion41);
  REXLOG_INFO("xact: converted a version 42 wave bank at {:08X} to 41", guest_buffer);
}

}  // namespace

// sub_82561C38 entry: r4 is the sound bank's file image.
void XactSoundBankCompat(PPCRegister& r4) {
  ConvertSoundBank(r4.u32);
}

// sub_825623A8 entry: r4 is the in-memory wave bank's file image. This path
// reads the segment table straight away, with no version check.
void XactWaveBankCompat(PPCRegister& r4) {
  ConvertWaveBank(r4.u32);
}

// sub_82562980, before the signature/version check: a streaming wave bank's
// header buffer, just read from the file, is at this+0x140.
void XactStreamingWaveBankCompat(PPCRegister& r31) {
  ConvertWaveBank(rex::memory::load_and_swap<uint32_t>(GuestPtr(r31.u32 + 0x140)));
}
