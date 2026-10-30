// nocturnerecomp - what the jp_text*.cpp files share: JP's image, the guest
// arena the copies of its text go in, and guest memory access. See jp_text.h.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

namespace nocturne::jp_text {

inline constexpr uint32_t kImageBase = 0x82000000u;

// PE timestamp of JP's release (see scripts/guest_recipe.py).
inline constexpr uint32_t kJpTimestamp = 0x5B4B7B46u;

// Room for everything copied from JP: about 0x3800 bytes today.
inline constexpr uint32_t kArenaSize = 0x10000;

inline uint32_t ReadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

inline rex::memory::Memory* Memory() {
  return REX_KERNEL_STATE()->memory();
}

inline uint8_t* Guest(uint32_t address) {
  return Memory()->TranslateVirtual<uint8_t*>(address);
}

inline uint32_t Load32(uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(Guest(address));
}

inline uint16_t Load16(uint32_t address) {
  return rex::memory::load_and_swap<uint16_t>(Guest(address));
}

// Writes guest memory, lifting the page protection for read-only data.
inline void WriteGuest(uint32_t guest, const void* data, size_t len) {
  auto* heap = Memory()->LookupHeap(guest);
  uint32_t first = 0, last = 0;
  const bool read_only =
      heap && heap->QueryProtect(guest, &first) &&
      heap->QueryProtect(guest + static_cast<uint32_t>(len) - 1, &last) &&
      (!(first & rex::memory::kMemoryProtectWrite) || !(last & rex::memory::kMemoryProtectWrite));
  uint32_t old = 0;
  const bool lifted =
      read_only && heap->Protect(guest, static_cast<uint32_t>(len),
                                 rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                                 &old);
  std::memcpy(Guest(guest), data, len);
  if (lifted)
    heap->Protect(guest, static_cast<uint32_t>(len), old);
}

inline void Store32(uint32_t address, uint32_t value) {
  uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                      static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
  WriteGuest(address, bytes, sizeof(bytes));
}

// Shift-JIS lead bytes; everything else is one byte wide.
inline bool IsLeadByte(uint8_t c) {
  return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

// JP's image, read from its own default.xex.
class JpImage {
 public:
  bool Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> xex((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    if (xex.empty() || !rex::runtime::XexModule::ExtractBaseImage(xex.data(), xex.size(), data_))
      return false;
    if (Timestamp() != kJpTimestamp) {
      REXLOG_WARN("jp text: {} is not JP's release (0x{:08X})", path.string(), Timestamp());
      return false;
    }
    return true;
  }

  bool Has(uint32_t address, uint32_t size = 1) const {
    return address >= kImageBase && uint64_t(address - kImageBase) + size <= data_.size();
  }

  uint32_t U32(uint32_t address) const {
    return Has(address, 4) ? ReadBE32(&data_[address - kImageBase]) : 0;
  }

  uint16_t U16(uint32_t address) const {
    return Has(address, 2) ? static_cast<uint16_t>(data_[address - kImageBase] << 8 |
                                                   data_[address - kImageBase + 1])
                           : 0;
  }

  // `size` bytes at `address`, or null when they are not all in the image.
  const uint8_t* Bytes(uint32_t address, uint32_t size) const {
    return Has(address, size) ? &data_[address - kImageBase] : nullptr;
  }

  // The bytes from `address` up to the first `00` (Shift-JIS and katakana
  // text), not included.
  std::string CString(uint32_t address) const {
    std::string out;
    while (Has(address) && data_[address - kImageBase])
      out.push_back(static_cast<char>(data_[address++ - kImageBase]));
    return out;
  }

  // The bytes from `address` up to and including `FF 00` (the PS1 menu
  // encoding, where 00 alone is a space and FF 9E/9F a voicing mark).
  std::string MenuString(uint32_t address) const {
    std::string out;
    while (Has(address, 2)) {
      const uint8_t c = data_[address - kImageBase];
      out.push_back(static_cast<char>(c));
      ++address;
      if (c == 0xFF && data_[address - kImageBase] == 0) {
        out.push_back('\0');
        break;
      }
      if (c == 0xFF) {
        out.push_back(static_cast<char>(data_[address - kImageBase]));
        ++address;
      }
    }
    return out;
  }

 private:
  uint32_t Timestamp() const {
    if (data_.size() < 0x40)
      return 0;
    uint32_t pe;
    std::memcpy(&pe, data_.data() + 0x3C, 4);  // little endian, like the rest of the PE header
    return size_t(pe) + 12 <= data_.size() ? ReadBE32(data_.data() + pe + 8) : 0;
  }

  std::vector<uint8_t> data_;
};

// Guest memory for the copies, handed out in order.
class Arena {
 public:
  bool Allocate() {
    base_ = Memory()->SystemHeapAlloc(kArenaSize, 0x100);
    return base_ != 0;
  }

  // A copy of `bytes` (plus a 00 for C strings), or 0 when full.
  uint32_t Put(const std::string& bytes, bool terminate) {
    const uint32_t size = static_cast<uint32_t>(bytes.size()) + (terminate ? 1 : 0);
    if (used_ + size > kArenaSize) {
      full_ = true;
      return 0;
    }
    const uint32_t at = base_ + used_;
    std::memcpy(Guest(at), bytes.data(), bytes.size());
    if (terminate)
      *Guest(at + size - 1) = 0;
    used_ = (used_ + size + 3) & ~3u;
    return at;
  }

  // A copy of JP's C string at `jp_address`.
  uint32_t PutString(const JpImage& jp, uint32_t jp_address) {
    return Put(jp.CString(jp_address), true);
  }

  uint32_t PutArray(const std::vector<uint32_t>& pointers) {
    std::string bytes;
    for (const uint32_t p : pointers) {
      for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<char>(p >> shift));
    }
    return Put(bytes, false);
  }

  bool full() const { return full_; }
  uint32_t used() const { return used_; }

 private:
  uint32_t base_ = 0;
  uint32_t used_ = 0;
  bool full_ = false;
};

// Calls the recompiled guest function at `address` with `ctx`'s registers,
// bypassing a function hook on it (function_hooks.h).
void CallGuest(uint32_t address, PPCContext& ctx, uint8_t* base);

// Each screen's part, called from InstallJpText once JP's image is read. They
// keep the copies they need; false when one could not be made.
bool InstallJpFileSelect(const JpImage& jp, Arena& arena);

}  // namespace nocturne::jp_text
