#include "xbla_package.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/ui/progress_window.h>

#include "icon.generated.h"
#include "intro_text.h"
#include "progress_theme.h"

// Ported from the SDK's GameDataSelector (src/system/game_data_selector.cpp).

namespace nocturne {
namespace {

namespace fs = std::filesystem;

std::string FormatBytes(uint64_t bytes) {
  constexpr uint64_t kMiB = 1024ull * 1024;
  constexpr uint64_t kGiB = kMiB * 1024;
  const bool gib = bytes >= kGiB;
  const uint64_t unit = gib ? kGiB : kMiB;
  const uint64_t tenths = (bytes * 10 + unit / 2) / unit;
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + (gib ? " GiB" : " MiB");
}

class Progress {
 public:
  Progress(const ExtractProgress& callback, std::string label, uint64_t total)
      : callback_(callback), label_(std::move(label)), total_(total) {
    if (!callback_)
      window_.emplace("Preparing game files", ProgressTheme(), kIconPNG, kIconPNGSize);
    Draw();
  }

  ~Progress() {
    bytes_ = total_;
    Draw();
  }

  void Add(uint64_t n) {
    bytes_ += n;
    const uint64_t now = SDL_GetTicks();
    if (now - last_draw_ms_ >= 33)
      Draw();
    if (now - last_log_ms_ >= 2000) {
      last_log_ms_ = now;
      REXLOG_INFO("{}: {}", label_, Detail());
    }
  }

 private:
  std::string Detail() const {
    if (total_ == 0)
      return FormatBytes(bytes_);
    const uint64_t pct = std::min<uint64_t>(100, bytes_ * 100 / total_);
    return std::to_string(pct) + "%   " + FormatBytes(bytes_) + " of " + FormatBytes(total_);
  }

  void Draw() {
    last_draw_ms_ = SDL_GetTicks();
    const float fraction =
        total_ ? std::min(1.0f, float(double(bytes_) / double(total_))) : -1.0f;
    if (callback_)
      callback_(label_, fraction, Detail());
    else
      window_->Draw(label_, fraction, Detail());
  }

  ExtractProgress callback_;
  std::optional<rex::ui::ProgressWindow> window_;
  std::string label_;
  uint64_t total_ = 0;
  uint64_t bytes_ = 0;
  uint64_t last_draw_ms_ = 0;
  uint64_t last_log_ms_ = 0;
};

// Bounds checked reads over an untrusted package: every offset and length in
// it comes from header fields. SDL_IOStream so an Android content URI is read
// where it lives.
class FileReader {
 public:
  explicit FileReader(const std::string& path) {
    io_ = SDL_IOFromFile(path.c_str(), "rb");
    if (!io_) {
      REXLOG_ERROR("Failed to open {}: {}", path, SDL_GetError());
      return;
    }
    const Sint64 size = SDL_GetIOSize(io_);
    if (size > 0)
      size_ = uint64_t(size);
  }

  ~FileReader() {
    if (io_)
      SDL_CloseIO(io_);
  }

  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  bool ok() const { return size_ > 0; }

  bool Read(uint64_t off, void* dst, uint64_t len) {
    if (len == 0)
      return true;
    if (off > size_ || len > size_ - off)
      return false;
    if (SDL_SeekIO(io_, Sint64(off), SDL_IO_SEEK_SET) < 0)
      return false;
    auto* p = static_cast<char*>(dst);
    while (len > 0) {
      const size_t n = SDL_ReadIO(io_, p, size_t(len));
      if (n == 0)
        return false;
      p += n;
      len -= n;
    }
    return true;
  }

  std::optional<uint8_t> U8(uint64_t off) {
    uint8_t v;
    return Read(off, &v, 1) ? std::optional(v) : std::nullopt;
  }

  std::optional<uint16_t> U16BE(uint64_t off) {
    uint8_t b[2];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return uint16_t((b[0] << 8) | b[1]);
  }

  std::optional<uint16_t> U16LE(uint64_t off) {
    uint8_t b[2];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return uint16_t(b[0] | (b[1] << 8));
  }

  std::optional<uint32_t> U24LE(uint64_t off) {
    uint8_t b[3];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16);
  }

  std::optional<uint32_t> U32BE(uint64_t off) {
    uint8_t b[4];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
  }

  bool MagicAt(uint64_t off, std::string_view magic) {
    std::string buf(magic.size(), '\0');
    return Read(off, buf.data(), magic.size()) && buf == magic;
  }

  bool CopyTo(std::ostream& out, uint64_t off, uint64_t len, Progress* progress) {
    std::vector<char> chunk(64 * 1024);
    while (len > 0) {
      const uint64_t n = std::min<uint64_t>(len, chunk.size());
      if (!Read(off, chunk.data(), n))
        return false;
      out.write(chunk.data(), std::streamsize(n));
      if (!out)
        return false;
      off += n;
      len -= n;
      if (progress)
        progress->Add(n);
    }
    return true;
  }

 private:
  SDL_IOStream* io_ = nullptr;
  uint64_t size_ = 0;
};

// Joins archive supplied names under `base`, refusing anything that escapes it.
std::optional<fs::path> SafeJoin(const fs::path& base, const std::vector<std::string>& parts) {
  if (parts.empty())
    return std::nullopt;
  fs::path rel;
  for (const std::string& part : parts) {
    if (part.empty() || part == "." || part == ".." ||
        part.find_first_of("/\\:") != std::string::npos)
      return std::nullopt;
    const fs::path p(part);
    if (p.has_root_name() || p.has_root_directory() || std::distance(p.begin(), p.end()) != 1)
      return std::nullopt;
    rel /= p;
  }
  return base / rel;
}

constexpr uint64_t kStfsBlockSize = 0x1000;
constexpr uint32_t kStfsEndOfChain = 0xFFFFFF;
constexpr int kStfsHashLevel0 = 170;
constexpr int kStfsHashLevel1 = 28900;  // 170^2

// Absolute offsets inside the StfsHeader (XContentHeader + XContentMetadata).
constexpr uint64_t kStfsOffHeaderSize = 0x340;        // be u32
constexpr uint64_t kStfsOffTitleId = 0x360;           // be u32
constexpr uint64_t kStfsOffVolumeDescriptor = 0x379;  // StfsVolumeDescriptor
constexpr uint64_t kStfsOffVolumeType = 0x3A9;        // be u32, 0 = STFS, 1 = SVOD
constexpr uint64_t kStfsOffDisplayName = 0x411;       // UTF-16BE, 0x80 bytes
constexpr size_t kStfsDisplayNameBytes = 0x80;

struct StfsEntry {
  std::string name;
  bool is_directory = false;
  uint32_t start_block = 0;
  uint32_t parent = 0xFFFF;
  uint64_t size = 0;
};

struct StfsInfo {
  bool read_only_format = false;
  bool root_active_index = false;
  int blocks_per_hash_table = 1;
  int block_step[2] = {0, 0};
  uint32_t file_table_block_count = 0;
  uint32_t file_table_block_number = 0;
  uint32_t total_block_count = 0;
  uint64_t data_base = 0;
};

bool IsStfsMagic(FileReader& reader) {
  return reader.MagicAt(0, "LIVE") || reader.MagicAt(0, "CON ") || reader.MagicAt(0, "PIRS");
}

std::optional<StfsInfo> ParseStfsInfo(FileReader& reader) {
  if (!IsStfsMagic(reader))
    return std::nullopt;

  const auto header_size = reader.U32BE(kStfsOffHeaderSize);
  const auto volume_type = reader.U32BE(kStfsOffVolumeType);
  const uint64_t vd = kStfsOffVolumeDescriptor;
  const auto flags = reader.U8(vd + 2);
  const auto file_table_block_count = reader.U16LE(vd + 3);
  const auto file_table_block_number = reader.U24LE(vd + 5);
  const auto total_block_count = reader.U32BE(vd + 0x1C);

  if (!header_size || !volume_type || !flags || !file_table_block_count ||
      !file_table_block_number || !total_block_count) {
    REXLOG_ERROR("STFS: header is truncated");
    return std::nullopt;
  }
  if (*volume_type != 0) {
    REXLOG_ERROR("STFS: SVOD packages are not supported");
    return std::nullopt;
  }

  StfsInfo info;
  info.read_only_format = (*flags & 0x1) != 0;
  info.root_active_index = ((*flags >> 1) & 0x1) != 0;
  info.file_table_block_count = *file_table_block_count;
  info.file_table_block_number = *file_table_block_number;
  info.total_block_count = *total_block_count;
  info.blocks_per_hash_table = info.read_only_format ? 1 : 2;
  info.block_step[0] = kStfsHashLevel0 + info.blocks_per_hash_table;
  info.block_step[1] = kStfsHashLevel1 + (kStfsHashLevel0 + 1) * info.blocks_per_hash_table;
  info.data_base = ((uint64_t(*header_size) + kStfsBlockSize - 1) / kStfsBlockSize) * kStfsBlockSize;
  return info;
}

uint64_t StfsBlockToOffset(uint32_t block_index, const StfsInfo& info) {
  uint64_t block = block_index;
  int64_t base = kStfsHashLevel0;
  for (int i = 0; i < 3; ++i) {
    block += ((block_index + base) / base) * info.blocks_per_hash_table;
    if (int64_t(block_index) < base)
      break;
    base *= kStfsHashLevel0;
  }
  return info.data_base + (block << 12);
}

uint64_t StfsHashOffset(uint32_t block_index, int level, const StfsInfo& info) {
  int64_t block;
  if (level == 0) {
    if (int64_t(block_index) < kStfsHashLevel0) {
      block = 0;
    } else {
      block = int64_t(block_index / kStfsHashLevel0) * info.block_step[0];
      block += (int64_t(block_index / kStfsHashLevel1) + 1) * info.blocks_per_hash_table;
      if (int64_t(block_index) >= kStfsHashLevel1)
        block += info.blocks_per_hash_table;
    }
  } else if (level == 1) {
    if (int64_t(block_index) < kStfsHashLevel1)
      block = info.block_step[0];
    else
      block = int64_t(block_index / kStfsHashLevel1) * info.block_step[1] +
              info.blocks_per_hash_table;
  } else {
    block = info.block_step[1];
  }
  return info.data_base + (uint64_t(block) << 12);
}

// Follows the level-0 hash table to the next block of a chain. The hash
// record's info word is big-endian, and which copy of a hash table is live is
// resolved top-down through the levels this package's block count has.
std::optional<uint32_t> StfsNextBlock(FileReader& reader, uint32_t block_index,
                                      const StfsInfo& info) {
  uint64_t secondary = info.root_active_index ? kStfsBlockSize : 0;
  const uint64_t off0 = StfsHashOffset(block_index, 0, info);

  if (info.read_only_format) {
    secondary = 0;
  } else if (info.total_block_count > uint32_t(kStfsHashLevel0)) {
    const uint64_t off1 = StfsHashOffset(block_index, 1, info);
    if (info.total_block_count > uint32_t(kStfsHashLevel1)) {
      const uint64_t off2 = StfsHashOffset(block_index, 2, info);
      const uint32_t rec2 = (block_index / kStfsHashLevel1) % kStfsHashLevel0;
      const auto word = reader.U32BE(off2 + secondary + rec2 * 0x18 + 0x14);
      if (!word)
        return std::nullopt;
      secondary = (*word & 0x40000000) ? kStfsBlockSize : 0;
    }
    const uint32_t rec1 = (block_index / kStfsHashLevel0) % kStfsHashLevel0;
    const auto word = reader.U32BE(off1 + secondary + rec1 * 0x18 + 0x14);
    if (!word)
      return std::nullopt;
    secondary = (*word & 0x40000000) ? kStfsBlockSize : 0;
  }

  const uint32_t rec0 = block_index % kStfsHashLevel0;
  const auto word = reader.U32BE(off0 + secondary + rec0 * 0x18 + 0x14);
  if (!word)
    return std::nullopt;
  return *word & 0xFFFFFF;
}

std::vector<StfsEntry> ParseStfsFileTable(FileReader& reader, const StfsInfo& info) {
  std::vector<StfsEntry> entries;
  uint32_t table_block = info.file_table_block_number;

  for (uint32_t bi = 0; bi < info.file_table_block_count; ++bi) {
    if (table_block == kStfsEndOfChain)
      break;
    const uint64_t block_off = StfsBlockToOffset(table_block, info);

    for (uint64_t m = 0; m < kStfsBlockSize / 0x40; ++m) {
      const uint64_t off = block_off + m * 0x40;
      const auto first = reader.U8(off);
      if (!first || *first == 0)
        break;
      const auto name_flags = reader.U8(off + 0x28);
      if (!name_flags)
        break;
      const uint8_t name_len = *name_flags & 0x3F;
      if (name_len == 0)
        break;

      std::string name(name_len, '\0');
      const auto start_block = reader.U24LE(off + 0x2F);
      const auto parent = reader.U16BE(off + 0x32);
      const auto size = reader.U32BE(off + 0x34);
      if (!reader.Read(off, name.data(), name_len) || !start_block || !parent || !size)
        break;

      StfsEntry entry;
      entry.name = std::move(name);
      entry.is_directory = (*name_flags & 0x80) != 0;
      entry.start_block = *start_block;
      entry.parent = *parent;
      entry.size = *size;
      entries.push_back(std::move(entry));
    }

    const auto next = StfsNextBlock(reader, table_block, info);
    if (!next) {
      REXLOG_WARN("STFS: file table chain broke after block {}", table_block);
      break;
    }
    table_block = *next;
  }
  return entries;
}

// Streams one file's block chain to `out`.
bool StfsCopyFile(FileReader& reader, const StfsEntry& entry, const StfsInfo& info,
                  std::ostream& out, Progress* progress) {
  uint32_t block_index = entry.start_block;
  uint64_t remaining = entry.size;
  uint64_t guard = 0;
  const uint64_t max_blocks = uint64_t(info.total_block_count) + 1;

  while (remaining > 0 && block_index != kStfsEndOfChain) {
    if (++guard > max_blocks) {
      REXLOG_WARN("STFS: block chain for '{}' is cyclic", entry.name);
      return false;
    }
    const uint64_t n = std::min<uint64_t>(remaining, kStfsBlockSize);
    if (!reader.CopyTo(out, StfsBlockToOffset(block_index, info), n, progress))
      return false;
    remaining -= n;
    if (remaining > 0) {
      const auto next = StfsNextBlock(reader, block_index, info);
      if (!next)
        return false;
      block_index = *next;
    }
  }
  if (remaining > 0) {
    REXLOG_WARN("STFS: chain for '{}' ended {} bytes short", entry.name, remaining);
    return false;
  }
  return true;
}

// An entry's path components, walking its parent chain.
std::optional<std::vector<std::string>> StfsEntryPath(const std::vector<StfsEntry>& entries,
                                                      size_t index) {
  std::vector<std::string> parts = {entries[index].name};
  uint32_t p = entries[index].parent;
  int depth = 0;
  while (p != 0xFFFF) {
    if (p >= entries.size() || ++depth > 100)
      return std::nullopt;
    parts.push_back(entries[p].name);
    p = entries[p].parent;
  }
  std::reverse(parts.begin(), parts.end());
  return parts;
}

uint64_t SumFileBytes(const std::vector<StfsEntry>& entries) {
  uint64_t total = 0;
  for (const auto& entry : entries) {
    if (!entry.is_directory)
      total += entry.size;
  }
  return total;
}

// The parsed package, or why it could not be.
struct Package {
  StfsInfo info;
  std::vector<StfsEntry> entries;
};

std::string OpenPackage(FileReader& reader, Package& out) {
  if (!reader.ok())
    return TrErr("err_read");
  const auto info = ParseStfsInfo(reader);
  if (!info)
    return TrErr("err_not_xbla");
  out.info = *info;
  out.entries = ParseStfsFileTable(reader, *info);
  if (out.entries.empty())
    return TrErr("err_not_xbla");
  return {};
}

const StfsEntry* FindRootFile(const Package& package, std::string_view name) {
  for (const StfsEntry& entry : package.entries) {
    if (!entry.is_directory && entry.parent == 0xFFFF && SameFileName(entry.name, name))
      return &entry;
  }
  return nullptr;
}

}  // namespace

bool MoveAside(const fs::path& dir) {
  std::error_code ec;
  if (fs::is_directory(dir, ec) && fs::is_empty(dir, ec))
    return fs::remove(dir, ec);
  for (int i = 0; i < 100; ++i) {
    fs::path aside = dir;
    aside += i ? ".old" + std::to_string(i) : ".old";
    if (fs::exists(aside, ec))
      continue;
    RenameRetrying(dir, aside, ec);
    if (ec)
      return false;
    REXLOG_WARN("Moved the unusable {} to {}", dir.string(), aside.string());
    return true;
  }
  return false;
}

void RenameRetrying(const fs::path& from, const fs::path& to, std::error_code& ec) {
  for (int attempt = 0;; ++attempt) {
    fs::rename(from, to, ec);
    if (!ec || attempt == 20 || !fs::exists(from))
      return;
    if (attempt == 0)
      REXLOG_WARN("Could not rename {} to {} ({}), retrying", from.string(), to.string(),
                  ec.message());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

bool SameFileName(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
  });
}

bool IsXblaPackage(const std::string& path) {
  FileReader reader(path);
  return reader.ok() && IsStfsMagic(reader);
}

std::string ReadPackageHeader(const std::string& package, PackageHeader& out) {
  FileReader reader(package);
  if (!reader.ok())
    return TrErr("err_read");
  if (!IsStfsMagic(reader))
    return TrErr("err_not_xbla");
  const auto title_id = reader.U32BE(kStfsOffTitleId);
  uint8_t name[kStfsDisplayNameBytes];
  if (!title_id || !reader.Read(kStfsOffDisplayName, name, sizeof(name)))
    return TrErr("err_truncated");
  out.title_id = *title_id;
  out.display_name.clear();
  for (size_t i = 0; i + 1 < sizeof(name); i += 2) {
    uint32_t c = (uint32_t(name[i]) << 8) | name[i + 1];
    if (c == 0)
      break;
    if (c >= 0xD800 && c <= 0xDFFF)
      c = '?';
    if (c < 0x80) {
      out.display_name += char(c);
    } else if (c < 0x800) {
      out.display_name += char(0xC0 | (c >> 6));
      out.display_name += char(0x80 | (c & 0x3F));
    } else {
      out.display_name += char(0xE0 | (c >> 12));
      out.display_name += char(0x80 | ((c >> 6) & 0x3F));
      out.display_name += char(0x80 | (c & 0x3F));
    }
  }
  return {};
}

std::string ReadPackageFile(const std::string& package, std::string_view name,
                            std::vector<uint8_t>& out) {
  FileReader reader(package);
  Package parsed;
  if (std::string error = OpenPackage(reader, parsed); !error.empty())
    return error;
  const StfsEntry* entry = FindRootFile(parsed, name);
  if (!entry)
    return TrErr("err_not_sotn");
  std::ostringstream buffer(std::ios::binary);
  if (!StfsCopyFile(reader, *entry, parsed.info, buffer, nullptr))
    return TrErr("err_truncated");
  const std::string bytes = buffer.str();
  out.assign(bytes.begin(), bytes.end());
  return {};
}

uint64_t MeasurePackage(const std::string& package) {
  FileReader reader(package);
  Package parsed;
  if (!OpenPackage(reader, parsed).empty())
    return 0;
  return SumFileBytes(parsed.entries);
}

std::string ExtractPackage(const std::string& package, const fs::path& out_dir,
                           std::string_view required_entry, const ExtractProgress& progress_callback) {
  FileReader reader(package);
  Package parsed;
  if (std::string error = OpenPackage(reader, parsed); !error.empty())
    return error;
  if (!FindRootFile(parsed, required_entry))
    return TrErr("err_not_sotn");
  const uint64_t total = SumFileBytes(parsed.entries);
  REXLOG_INFO("Package holds {} entries, {}", parsed.entries.size(), FormatBytes(total));

  fs::path partial = out_dir;
  partial += ".partial";
  std::error_code ec;
  fs::remove_all(partial, ec);
  fs::create_directories(partial, ec);
  if (ec)
    return TrErr("err_create", partial.string(), ec.message());

  bool complete = true;
  uint32_t files = 0;
  {
    Progress progress(progress_callback, "Extracting game files...", total);
    for (size_t i = 0; i < parsed.entries.size() && complete; ++i) {
      const StfsEntry& entry = parsed.entries[i];
      if (entry.is_directory)
        continue;
      const auto parts = StfsEntryPath(parsed.entries, i);
      const auto dest = parts ? SafeJoin(partial, *parts) : std::nullopt;
      if (!dest) {
        REXLOG_WARN("STFS: rejected entry path for '{}'", entry.name);
        complete = false;
        break;
      }
      fs::create_directories(dest->parent_path(), ec);
      std::ofstream out(*dest, std::ios::binary);
      if (!out || !StfsCopyFile(reader, entry, parsed.info, out, &progress)) {
        REXLOG_WARN("STFS: failed to extract '{}'", entry.name);
        complete = false;
        break;
      }
      ++files;
    }
  }
  if (!complete) {
    fs::remove_all(partial, ec);
    return TrErr("err_extract_failed");
  }
  if (fs::exists(out_dir, ec) && !MoveAside(out_dir))
    return TrErr("err_move_aside", out_dir.string());
  RenameRetrying(partial, out_dir, ec);
  if (ec)
    return TrErr("err_rename_extracted", ec.message());
  REXLOG_INFO("Extracted {} files into {}", files, out_dir.string());
  return {};
}

}  // namespace nocturne
