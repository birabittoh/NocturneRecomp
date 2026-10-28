#include "fonts_zip.h"

#include <array>
#include <utility>

namespace nocturne {
namespace {

uint16_t Le16(const std::vector<uint8_t>& d, size_t at) {
  return static_cast<uint16_t>(d[at] | d[at + 1] << 8);
}

uint32_t Le32(const std::vector<uint8_t>& d, size_t at) {
  return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | static_cast<uint32_t>(d[at + 3]) << 24;
}

void PutLe16(std::vector<uint8_t>& d, size_t at, uint32_t v) {
  d[at] = static_cast<uint8_t>(v);
  d[at + 1] = static_cast<uint8_t>(v >> 8);
}

void PutLe32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
  PutLe16(d, at, v);
  PutLe16(d, at + 2, v >> 16);
}

std::string BaseName(std::string_view path) {
  const size_t slash = path.find_last_of("\\/");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// The end of central directory record's offset, or npos.
size_t FindEnd(const std::vector<uint8_t>& d) {
  constexpr size_t kEndSize = 22;
  if (d.size() < kEndSize)
    return std::string::npos;
  size_t end = d.size() - kEndSize;
  while (Le32(d, end) != 0x06054B50) {
    if (end == 0 || d.size() - end > kEndSize + 0xFFFF)
      return std::string::npos;
    --end;
  }
  return end;
}

uint32_t Crc32(const std::vector<uint8_t>& data) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t b : data)
    crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ---------------------------------------------------------------------------
// Raw deflate (RFC 1951), enough for the fonts.zip entries
// ---------------------------------------------------------------------------

class Inflater {
 public:
  Inflater(const uint8_t* in, size_t size, std::vector<uint8_t>& out)
      : in_(in), size_(size), out_(out) {}

  bool Run() {
    bool last = false;
    while (!last) {
      last = Bits(1);
      const int type = Bits(2);
      bool ok = false;
      if (type == 0)
        ok = Stored();
      else if (type == 1)
        ok = Fixed();
      else if (type == 2)
        ok = Dynamic();
      if (!ok || overrun_)
        return false;
    }
    return true;
  }

 private:
  struct Huffman {
    std::array<uint16_t, 16> count{};
    std::array<uint16_t, 288> symbol{};
  };

  int Bits(int need) {
    int value = bit_buf_;
    while (bit_count_ < need) {
      if (pos_ >= size_) {
        overrun_ = true;
        return 0;
      }
      value |= in_[pos_++] << bit_count_;
      bit_count_ += 8;
    }
    bit_buf_ = value >> need;
    bit_count_ -= need;
    return value & ((1 << need) - 1);
  }

  bool Stored() {
    bit_buf_ = 0;
    bit_count_ = 0;
    if (pos_ + 4 > size_)
      return false;
    const unsigned len = in_[pos_] | in_[pos_ + 1] << 8;
    const unsigned nlen = in_[pos_ + 2] | in_[pos_ + 3] << 8;
    pos_ += 4;
    if (len != (~nlen & 0xFFFF) || pos_ + len > size_)
      return false;
    out_.insert(out_.end(), in_ + pos_, in_ + pos_ + len);
    pos_ += len;
    return true;
  }

  static bool Build(Huffman& h, const uint16_t* lengths, int n) {
    h.count.fill(0);
    for (int i = 0; i < n; ++i)
      ++h.count[lengths[i]];
    if (h.count[0] == n)
      return true;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left = (left << 1) - h.count[len];
      if (left < 0)
        return false;
    }
    std::array<uint16_t, 16> offs{};
    for (int len = 1; len < 15; ++len)
      offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; ++i) {
      if (lengths[i])
        h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    return true;
  }

  int Decode(const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= Bits(1);
      if (overrun_)
        return -1;
      const int count = h.count[len];
      if (code - count < first)
        return h.symbol[index + (code - first)];
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    return -1;
  }

  bool Codes(const Huffman& lencode, const Huffman& distcode) {
    static constexpr uint16_t kLenBase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                            15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                            67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr uint16_t kDistBase[] = {
        1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,
                                             6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int symbol = Decode(lencode);
      if (symbol < 0)
        return false;
      if (symbol < 256) {
        out_.push_back(static_cast<uint8_t>(symbol));
        continue;
      }
      if (symbol == 256)
        return true;
      symbol -= 257;
      if (symbol >= 29)
        return false;
      const size_t len = kLenBase[symbol] + Bits(kLenExtra[symbol]);
      const int dist_symbol = Decode(distcode);
      if (dist_symbol < 0 || dist_symbol >= 30)
        return false;
      const size_t dist = kDistBase[dist_symbol] + Bits(kDistExtra[dist_symbol]);
      if (overrun_ || dist > out_.size())
        return false;
      const size_t from = out_.size() - dist;
      for (size_t i = 0; i < len; ++i)
        out_.push_back(out_[from + i]);
    }
  }

  bool Fixed() {
    static const auto tables = [] {
      std::pair<Huffman, Huffman> t;
      std::array<uint16_t, 288> lengths{};
      int i = 0;
      for (; i < 144; ++i)
        lengths[i] = 8;
      for (; i < 256; ++i)
        lengths[i] = 9;
      for (; i < 280; ++i)
        lengths[i] = 7;
      for (; i < 288; ++i)
        lengths[i] = 8;
      Build(t.first, lengths.data(), 288);
      std::array<uint16_t, 30> dist;
      dist.fill(5);
      Build(t.second, dist.data(), 30);
      return t;
    }();
    return Codes(tables.first, tables.second);
  }

  bool Dynamic() {
    static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                           11, 4,  12, 3, 13, 2, 14, 1, 15};
    const int nlen = Bits(5) + 257;
    const int ndist = Bits(5) + 1;
    const int ncode = Bits(4) + 4;
    if (nlen > 286 || ndist > 30)
      return false;
    std::array<uint16_t, 320> lengths{};
    for (int i = 0; i < ncode; ++i)
      lengths[kOrder[i]] = static_cast<uint16_t>(Bits(3));
    Huffman lencode, distcode;
    if (!Build(lencode, lengths.data(), 19))
      return false;
    int index = 0;
    while (index < nlen + ndist) {
      int symbol = Decode(lencode);
      if (symbol < 0)
        return false;
      if (symbol < 16) {
        lengths[index++] = static_cast<uint16_t>(symbol);
        continue;
      }
      uint16_t len = 0;
      int repeat;
      if (symbol == 16) {
        if (index == 0)
          return false;
        len = lengths[index - 1];
        repeat = 3 + Bits(2);
      } else if (symbol == 17) {
        repeat = 3 + Bits(3);
      } else {
        repeat = 11 + Bits(7);
      }
      if (index + repeat > nlen + ndist)
        return false;
      while (repeat--)
        lengths[index++] = len;
    }
    if (lengths[256] == 0)
      return false;
    if (!Build(lencode, lengths.data(), nlen) || !Build(distcode, lengths.data() + nlen, ndist))
      return false;
    return Codes(lencode, distcode);
  }

  const uint8_t* in_;
  size_t size_;
  size_t pos_ = 0;
  int bit_buf_ = 0;
  int bit_count_ = 0;
  bool overrun_ = false;
  std::vector<uint8_t>& out_;
};

}  // namespace

std::vector<ZipEntry> ParseZip(const std::vector<uint8_t>& d) {
  const size_t end = FindEnd(d);
  if (end == std::string::npos)
    return {};
  const uint16_t count = Le16(d, end + 10);
  size_t at = Le32(d, end + 16);
  std::vector<ZipEntry> entries;
  for (uint16_t i = 0; i < count; ++i) {
    if (at + 46 > d.size() || Le32(d, at) != 0x02014B50)
      return {};
    ZipEntry e;
    e.method = Le16(d, at + 10);
    e.crc = Le32(d, at + 16);
    e.compressed = Le32(d, at + 20);
    e.size = Le32(d, at + 24);
    const uint16_t name_len = Le16(d, at + 28);
    const size_t local = Le32(d, at + 42);
    if (at + 46 + name_len > d.size() || local + 30 > d.size() || Le32(d, local) != 0x04034B50)
      return {};
    e.name = BaseName(std::string_view(reinterpret_cast<const char*>(&d[at + 46]), name_len));
    e.data = local + 30 + Le16(d, local + 26) + Le16(d, local + 28);
    if (e.data + e.compressed > d.size())
      return {};
    entries.push_back(std::move(e));
    at += 46 + name_len + Le16(d, at + 30) + Le16(d, at + 32);
  }
  return entries;
}

std::vector<uint8_t> Extract(const std::vector<uint8_t>& zip, const ZipEntry& e) {
  std::vector<uint8_t> out;
  if (e.method == 0) {
    out.assign(zip.begin() + e.data, zip.begin() + e.data + e.compressed);
  } else if (e.method == 8) {
    out.reserve(e.size);
    if (!Inflater(zip.data() + e.data, e.compressed, out).Run())
      return {};
  }
  return out.size() == e.size ? out : std::vector<uint8_t>{};
}

std::vector<uint8_t> AddStoredEntry(const std::vector<uint8_t>& zip, std::string_view name,
                                    const std::vector<uint8_t>& data) {
  const size_t end = FindEnd(zip);
  if (end == std::string::npos)
    return {};
  const uint32_t dir_size = Le32(zip, end + 12);
  const size_t dir = Le32(zip, end + 16);
  if (dir + dir_size > end)
    return {};
  const uint32_t crc = Crc32(data);
  const auto size = static_cast<uint32_t>(data.size());
  const auto name_len = static_cast<uint32_t>(name.size());

  // The local entries, then the new one; the central directory, then the
  // new one's record; the end record, moved past both.
  std::vector<uint8_t> out(zip.begin(), zip.begin() + dir);
  const size_t local = out.size();
  out.resize(local + 30);
  PutLe32(out, local, 0x04034B50);
  PutLe16(out, local + 4, 20);  // version needed
  PutLe32(out, local + 14, crc);
  PutLe32(out, local + 18, size);
  PutLe32(out, local + 22, size);
  PutLe16(out, local + 26, name_len);
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), data.begin(), data.end());

  const size_t new_dir = out.size();
  out.insert(out.end(), zip.begin() + dir, zip.begin() + dir + dir_size);
  const size_t record = out.size();
  out.resize(record + 46);
  PutLe32(out, record, 0x02014B50);
  PutLe16(out, record + 4, 20);  // version made by
  PutLe16(out, record + 6, 20);  // version needed
  PutLe32(out, record + 16, crc);
  PutLe32(out, record + 20, size);
  PutLe32(out, record + 24, size);
  PutLe16(out, record + 28, name_len);
  PutLe32(out, record + 42, static_cast<uint32_t>(local));
  out.insert(out.end(), name.begin(), name.end());

  const size_t new_end = out.size();
  out.insert(out.end(), zip.begin() + end, zip.end());
  PutLe16(out, new_end + 8, Le16(zip, end + 8) + 1u);
  PutLe16(out, new_end + 10, Le16(zip, end + 10) + 1u);
  PutLe32(out, new_end + 12, static_cast<uint32_t>(out.size() - (zip.size() - end) - new_dir));
  PutLe32(out, new_end + 16, static_cast<uint32_t>(new_dir));
  return out;
}

}  // namespace nocturne
