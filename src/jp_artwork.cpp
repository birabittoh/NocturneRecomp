#include "jp_artwork.h"

#include <algorithm>
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

#include "fonts_zip.h"
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

uint32_t Be32(const std::vector<uint8_t>& d, size_t at) {
  return static_cast<uint32_t>(d[at]) << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];
}

std::string BaseName(std::string_view path) {
  const size_t slash = path.find_last_of("\\/");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
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
