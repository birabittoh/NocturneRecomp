#include "jp_artwork.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "target.h"

REXCVAR_DEFINE_STRING(artwork, "auto", "Game",
                      "Whose artwork to show when both releases are installed: world, jp, or "
                      "auto (JP's while the language is Japanese)")
    .allowed({"auto", "jp", "world"});

namespace nocturne {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kLanguageJapanese = 2;

// The UI image bank (see achievement_icons.cpp): count, then 16-byte entries
// with the name pointer at +8 and the image object at +16. The image object
// holds its D3DTexture at +16, whose fetch constant starts at +0x1C; its
// second dword is the texture's physical base address.
constexpr uint32_t kImageBankPtrAddr = 0x82E7A570u;
constexpr uint32_t kBankEntryStride = 16;
constexpr uint32_t kBankNameOffset = 8;
constexpr uint32_t kBankImageOffset = 16;
constexpr uint32_t kBankCapacity = 1024;
constexpr uint32_t kImageTextureOffset = 16;
constexpr uint32_t kTextureFetchOffset = 0x1C;
// The fetch constant's second dword: the base address, in the file relative
// to the data section.
constexpr uint32_t kTextureBaseOffset = kTextureFetchOffset + 4;

// sub_82234E80's resident fonts.zip table: a name pointer per entry, and a
// 16-byte row per entry of {uncompressed size, buffer, loaded}.
constexpr uint32_t kFontsNamesAddr = 0x82E448B8u;
constexpr uint32_t kFontsRowsAddr = 0x831D0E44u;
constexpr uint32_t kFontsRowStride = 16;
constexpr uint32_t kFontsEntries = 297;

// How often, in frames, what is in memory is checked against the setting
// when nothing changed.
constexpr uint32_t kRecheckFrames = 60;

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// The file's real name in dir, matched without case: the folder comes from
// an extraction and the host file system may be case sensitive.
fs::path FindFile(const fs::path& dir, std::string_view name) {
  std::error_code ec;
  for (const auto& item : fs::directory_iterator(dir, ec)) {
    if (item.is_regular_file(ec) &&
        rex::string::utf8_equal_case(item.path().filename().string(), name))
      return item.path();
  }
  return {};
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return {};
  std::vector<uint8_t> data(static_cast<size_t>(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return in ? data : std::vector<uint8_t>{};
}

uint16_t Le16(const std::vector<uint8_t>& d, size_t at) {
  return static_cast<uint16_t>(d[at] | d[at + 1] << 8);
}

uint32_t Le32(const std::vector<uint8_t>& d, size_t at) {
  return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | static_cast<uint32_t>(d[at + 3]) << 24;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t at) {
  return static_cast<uint32_t>(d[at]) << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];
}

std::string BaseName(std::string_view path) {
  const size_t slash = path.find_last_of("\\/");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
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

// ---------------------------------------------------------------------------
// fonts.zip
// ---------------------------------------------------------------------------

struct ZipEntry {
  std::string name;
  uint32_t crc = 0;
  uint32_t size = 0;
  uint16_t method = 0;
  size_t data = 0;  // offset of the compressed bytes
  uint32_t compressed = 0;
};

// The entries of a single-disk zip without zip64. Empty when it is anything
// else.
std::vector<ZipEntry> ParseZip(const std::vector<uint8_t>& d) {
  constexpr size_t kEndSize = 22;
  if (d.size() < kEndSize)
    return {};
  size_t end = d.size() - kEndSize;
  while (Le32(d, end) != 0x06054B50) {
    if (end == 0 || d.size() - end > kEndSize + 0xFFFF)
      return {};
    --end;
  }
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

// ---------------------------------------------------------------------------
// uiresource.xpr
// ---------------------------------------------------------------------------

struct XprTexture {
  std::string name;
  size_t data = 0;  // file offset of the texture's bytes
  size_t extent = 0;  // up to the next texture, or the end of the file
};

std::vector<XprTexture> ParseXpr(const std::vector<uint8_t>& d) {
  if (d.size() < 16 || std::memcmp(d.data(), "XPR2", 4) != 0)
    return {};
  const size_t data_start = 12 + Be32(d, 4);
  const uint32_t count = Be32(d, 12);
  std::vector<XprTexture> textures;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t row = 16 + 16 * size_t{i};
    if (row + 16 > d.size())
      return {};
    if (std::memcmp(&d[row], "TX2D", 4) != 0)
      continue;
    const size_t header = 12 + Be32(d, row + 4);
    const size_t name = 12 + Be32(d, row + 12);
    if (header + kTextureBaseOffset + 4 > d.size() || name >= d.size())
      return {};
    const auto* text = reinterpret_cast<const char*>(&d[name]);
    textures.push_back({std::string(text, strnlen(text, d.size() - name)),
                        data_start + (Be32(d, header + kTextureBaseOffset) & ~0xFFFu), 0});
  }
  for (auto& texture : textures) {
    size_t next = d.size();
    for (const auto& other : textures) {
      if (other.data > texture.data)
        next = std::min(next, other.data);
    }
    texture.extent = next > texture.data ? next - texture.data : 0;
  }
  return textures;
}

// ---------------------------------------------------------------------------
// What differs, and where it lives in guest memory
// ---------------------------------------------------------------------------

// The span of one texture or entry the releases disagree on, with both
// releases' bytes for it. `probe` is the first byte they differ at: what the
// guest holds there says which release is in memory now; the whole span is
// checked before it is replaced.
struct Swap {
  std::string name;
  std::vector<uint8_t> world;
  std::vector<uint8_t> jp;
  size_t offset = 0;  // into the texture (textures only)
  size_t probe = 0;
  bool reported = false;
};

std::vector<Swap> g_textures;
std::vector<Swap> g_entries;  // whole entries; the sizes may differ
bool g_loaded = false;
std::atomic<bool> g_dirty{true};

std::optional<size_t> FirstDifference(const std::vector<uint8_t>& a,
                                      const std::vector<uint8_t>& b) {
  const size_t n = std::min(a.size(), b.size());
  const auto it = std::mismatch(a.begin(), a.begin() + n, b.begin()).first;
  if (it != a.begin() + n)
    return static_cast<size_t>(it - a.begin());
  return a.size() == b.size() ? std::nullopt : std::optional<size_t>(n);
}

void LoadTextures(const fs::path& world_path, const fs::path& jp_path) {
  const auto world = ReadFile(world_path);
  const auto jp = ReadFile(jp_path);
  const auto layout = ParseXpr(world);
  if (layout.empty() || world.size() != jp.size() || ParseXpr(jp).size() != layout.size()) {
    REXLOG_WARN("jp artwork: {} and {} do not share a layout", world_path.string(),
                jp_path.string());
    return;
  }
  for (const auto& texture : layout) {
    const auto begin = world.begin() + texture.data;
    const auto end = begin + texture.extent;
    const auto first = std::mismatch(begin, end, jp.begin() + texture.data).first;
    if (first == end)
      continue;
    // Only the differing span is written, so nothing past the texture's real
    // data (the file pads up to the next one) is ever touched.
    size_t last = texture.data + texture.extent;
    while (world[last - 1] == jp[last - 1])
      --last;
    const size_t from = static_cast<size_t>(first - world.begin());
    Swap swap;
    swap.name = texture.name;
    swap.offset = from - texture.data;
    swap.world.assign(world.begin() + from, world.begin() + last);
    swap.jp.assign(jp.begin() + from, jp.begin() + last);
    g_textures.push_back(std::move(swap));
  }
}

void LoadEntries(const fs::path& world_path, const fs::path& jp_path) {
  const auto world = ReadFile(world_path);
  const auto jp = ReadFile(jp_path);
  const auto world_entries = ParseZip(world);
  const auto jp_entries = ParseZip(jp);
  if (world_entries.empty() || jp_entries.empty()) {
    REXLOG_WARN("jp artwork: cannot read {} and {}", world_path.string(), jp_path.string());
    return;
  }
  for (const auto& w : world_entries) {
    const auto j = std::find_if(jp_entries.begin(), jp_entries.end(), [&](const ZipEntry& e) {
      return rex::string::utf8_equal_case(e.name, w.name);
    });
    if (j == jp_entries.end() || (j->crc == w.crc && j->size == w.size))
      continue;
    Swap swap;
    swap.name = w.name;
    swap.world = Extract(world, w);
    swap.jp = Extract(jp, *j);
    const auto probe = FirstDifference(swap.world, swap.jp);
    if (swap.world.empty() || swap.jp.empty() || !probe) {
      REXLOG_WARN("jp artwork: cannot extract {}", w.name);
      continue;
    }
    swap.probe = *probe;
    g_entries.push_back(std::move(swap));
  }
}

bool WantJp() {
  const std::string choice = REXCVAR_GET(artwork);
  if (choice == "jp")
    return true;
  if (choice == "world")
    return false;
  return rex::cvar::Query<uint32_t>("user_language") == kLanguageJapanese;
}

// The texture called name in the UI image bank, as a host pointer to its
// bytes, or null while the bank has no such texture. The fetch constant holds
// the loader's address in the 0xE0000000 aperture, whose physical page is one
// page (0x1000) higher (the same fixup as DecodeTextureFetch's base_address).
// Without it the swap read and wrote a page early, which is what striped the
// logo.
uint8_t* FindTexture(uint8_t* base, const std::string& name) {
  const uint32_t bank = rex::memory::load_and_swap<uint32_t>(base + kImageBankPtrAddr);
  if (!bank)
    return nullptr;
  const uint32_t count =
      std::min(rex::memory::load_and_swap<uint32_t>(base + bank), kBankCapacity);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = bank + i * kBankEntryStride;
    const uint32_t name_ptr = rex::memory::load_and_swap<uint32_t>(base + entry + kBankNameOffset);
    if (!name_ptr || std::strcmp(reinterpret_cast<const char*>(base + name_ptr), name.c_str()))
      continue;
    const uint32_t image = rex::memory::load_and_swap<uint32_t>(base + entry + kBankImageOffset);
    const uint32_t texture =
        image ? rex::memory::load_and_swap<uint32_t>(base + image + kImageTextureOffset) : 0;
    if (!texture)
      return nullptr;
    const uint32_t address =
        rex::memory::load_and_swap<uint32_t>(base + texture + kTextureBaseOffset) & ~0xFFFu;
    if (!address)
      return nullptr;
    const uint32_t physical = (address & 0x1FFFFFFFu) + (((address >> 20) + 512u) & 0x1000u);
    auto* ks = rex::system::kernel_state();
    return ks && ks->memory() ? ks->memory()->TranslatePhysical(physical) : nullptr;
  }
  return nullptr;
}

// Puts `want`'s bytes at dst if the other release's are there now. Returns
// false while the guest holds neither (not loaded yet, or something else).
bool Put(Swap& swap, uint8_t* dst, bool want_jp) {
  const auto& from = want_jp ? swap.world : swap.jp;
  const auto& to = want_jp ? swap.jp : swap.world;
  if (dst[swap.probe] == to[swap.probe])
    return true;
  // The probe byte alone can match by chance; only replace what really is the
  // other release's.
  if (dst[swap.probe] != from[swap.probe] || std::memcmp(dst, from.data(), from.size()) != 0) {
    if (!swap.reported)
      REXLOG_WARN("jp artwork: {} holds neither release's bytes, leaving it", swap.name);
    swap.reported = true;
    return false;
  }
  std::memcpy(dst, to.data(), to.size());
  return true;
}

}  // namespace

bool JpArtworkAvailable() {
  return JpOverlaysAvailable();
}

void BindJpArtwork() {
  if (!JpArtworkAvailable())
    return;
  const fs::path world_media = GameDataRoot() / "MEDIA";
  const fs::path jp_media = JpDir() / "MEDIA";
  const fs::path world_ui = FindFile(world_media, "uiresource.xpr");
  const fs::path jp_ui = FindFile(jp_media, "uiresource.xpr");
  if (!world_ui.empty() && !jp_ui.empty())
    LoadTextures(world_ui, jp_ui);
  const fs::path world_zip = FindFile(GameDataRoot() / "DATA" / "fonts", "fonts.zip");
  const fs::path jp_zip = FindFile(JpDir() / "DATA" / "fonts", "fonts.zip");
  if (!world_zip.empty() && !jp_zip.empty())
    LoadEntries(world_zip, jp_zip);
  g_loaded = !g_textures.empty() || !g_entries.empty();
  REXLOG_INFO("jp artwork: {} texture(s) and {} fonts.zip entr(ies) differ", g_textures.size(),
              g_entries.size());

  auto mark = [](std::string_view, std::string_view) { g_dirty.store(true); };
  rex::cvar::RegisterChangeCallback("artwork", mark);
  rex::cvar::RegisterChangeCallback("user_language", mark);
}

void ApplyPendingArtwork(uint8_t* base) {
  if (!g_loaded || !base)
    return;
  // A change is applied on the next frame. Otherwise look again now and then,
  // in case the game reloaded one of the files over what was put there.
  static uint32_t frames = 0;
  if (!g_dirty.load(std::memory_order_acquire) && ++frames % kRecheckFrames != 0)
    return;
  // The table's names are static: pair its rows with the entries once.
  static const std::vector<std::pair<uint32_t, Swap*>> rows = [base] {
    std::vector<std::pair<uint32_t, Swap*>> found;
    for (uint32_t i = 0; i < kFontsEntries; ++i) {
      const uint32_t name_ptr =
          rex::memory::load_and_swap<uint32_t>(base + kFontsNamesAddr + 4 * i);
      if (!name_ptr)
        continue;
      const std::string name = BaseName(reinterpret_cast<const char*>(base + name_ptr));
      for (auto& swap : g_entries) {
        if (rex::string::utf8_equal_case(swap.name, name))
          found.emplace_back(kFontsRowsAddr + i * kFontsRowStride, &swap);
      }
    }
    return found;
  }();

  // Stays dirty until everything the guest has loaded holds the choice; the
  // bank and the fonts table fill in during boot.
  g_dirty.store(false);
  const bool want_jp = WantJp();
  bool done = true;

  for (auto& swap : g_textures) {
    uint8_t* texture = FindTexture(base, swap.name);
    if (!texture || !Put(swap, texture + swap.offset, want_jp))
      done = false;
  }

  for (const auto& [row, swap] : rows) {
    const uint32_t buffer = rex::memory::load_and_swap<uint32_t>(base + row + 4);
    if (!buffer || !rex::memory::load_and_swap<uint32_t>(base + row + 8)) {
      done = false;
      continue;
    }
    // The buffer was sized for World's entry; JP's has to fit in it.
    const auto& to = want_jp ? swap->jp : swap->world;
    if (to.size() > swap->world.size()) {
      if (!swap->reported)
        REXLOG_WARN("jp artwork: JP's {} is bigger than World's, leaving it", swap->name);
      swap->reported = true;
      continue;
    }
    if (!Put(*swap, base + buffer, want_jp))
      continue;
    rex::memory::store_and_swap<uint32_t>(base + row, static_cast<uint32_t>(to.size()));
  }

  if (!done)
    g_dirty.store(true);
}

}  // namespace nocturne
