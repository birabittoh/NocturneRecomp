#include "save_slots.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "intro_text.h"

namespace nocturne {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr const char* kDefaultProfile = "B13EBABEBABEBABE";
constexpr const char* kTitleDir = "58410847";
constexpr uint32_t kTitleId = 0x58410847;
constexpr const char* kFilePrefix = "BASLUS-00067DRAX";
constexpr size_t kBlockSize = 0x2000;
constexpr size_t kHeaderSize = 328;
// Block layout.
constexpr size_t kTitleOffset = 0x04;
constexpr size_t kTitleBytes = 0x40;
constexpr size_t kClutOffset = 0x60;
constexpr size_t kIconOffset = 0x80;
// SaveInfo, big-endian: cardIcon (u32) and nRoomsExplored (u16).
constexpr size_t kCardIconOffset = 0x120;
constexpr size_t kRoomsOffset = 0x12A;
// The rooms in both castles; the load menu's percentage is rooms * 100 / this.
constexpr int kRoomCount = 942;

// The game's icon set in the World image (sub_82253BF0, sub_82396CF8):
// 16 CLUTs, then 16 icons of three 4bpp 16x16 frames, the icons' pointer
// table, then which frame (1-based) the port keeps in the block. One range,
// cached as it is. The PS1 kept all three frames in the block, so the BIOS
// animated every icon.
constexpr int kIconCount = 16;
constexpr int kIconFrames = 3;
constexpr size_t kIconFrameBytes = 0x80;
constexpr size_t kIconBytes = kIconFrames * kIconFrameBytes;
constexpr uint32_t kIconSetAddress = 0x82E024C8;
constexpr size_t kIconSetCluts = 0;
constexpr size_t kIconSetIcons = kIconSetCluts + kIconCount * 0x20;
constexpr size_t kIconSetTable = kIconSetIcons + kIconCount * kIconBytes;
constexpr size_t kIconSetStill = kIconSetTable + kIconCount * 4;
constexpr size_t kIconSetBytes = kIconSetStill + kIconCount * 4;
constexpr char kIconCacheMagic[4] = {'N', 'R', 'S', 'I'};

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The set is where this build expects it: a different image (the randomizer's,
// say) may have moved it.
bool IsIconSet(const uint8_t* set) {
  for (int i = 0; i < kIconCount; ++i) {
    const uint32_t icon = LoadBE32(set + kIconSetTable + size_t(i) * 4);
    const uint32_t still = LoadBE32(set + kIconSetStill + size_t(i) * 4);
    if (icon != kIconSetAddress + kIconSetIcons + size_t(i) * kIconBytes || still < 1 ||
        still > uint32_t(kIconFrames))
      return false;
  }
  return true;
}

Bytes ReadFile(const fs::path& path, size_t limit = SIZE_MAX) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  Bytes bytes;
  char buffer[1 << 14];
  while (bytes.size() < limit && file) {
    file.read(buffer, std::streamsize(std::min(sizeof(buffer), limit - bytes.size())));
    bytes.insert(bytes.end(), buffer, buffer + file.gcount());
  }
  return bytes;
}

bool WriteFile(const fs::path& path, const Bytes& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  return file && file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())) &&
         file.flush();
}

// The profile folder holding the game's saves, else the SDK's default one.
fs::path TitleRoot(const fs::path& root) {
  std::error_code ec;
  if (fs::is_directory(root / kDefaultProfile / kTitleDir, ec))
    return root / kDefaultProfile / kTitleDir;
  std::vector<fs::path> found;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    std::error_code entry_ec;
    if (entry.is_directory(entry_ec) &&
        fs::is_directory(entry.path() / kTitleDir / "00000001", entry_ec))
      found.push_back(entry.path() / kTitleDir);
  }
  std::sort(found.begin(), found.end());
  return found.empty() ? root / kDefaultProfile / kTitleDir : found.front();
}

std::string FileName(int slot) {
  return kFilePrefix + std::to_string(slot);
}

fs::path BlockPath(const fs::path& root, int slot) {
  return TitleRoot(root) / "00000001" / FileName(slot) / FileName(slot);
}

fs::path HeaderPath(const fs::path& root, int slot) {
  return TitleRoot(root) / "Headers" / "00000001" / (FileName(slot) + ".header");
}

// The randomizer's record of the world a save was written in.
fs::path RandoRecordPath(const fs::path& root, int slot) {
  return root / "rando_slots" / (FileName(slot) + ".txt");
}

void ReadRandoRecord(const fs::path& root, int slot, SaveSlotInfo& info) {
  std::ifstream f(RandoRecordPath(root, slot));
  if (!f)
    return;
  std::string seed;
  for (std::string line; std::getline(f, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (line == "vanilla=1")
      return;
    if (line.rfind("seed=", 0) == 0)
      seed = line.substr(5);
  }
  info.randomized = true;
  info.seed = seed;
}

bool IsBlock(const Bytes& data) {
  return data.size() == kBlockSize && data[0] == 'S' && data[1] == 'C';
}

// file_time_type's epoch is unspecified and clock_cast is not everywhere this builds.
std::string FormatModified(fs::file_time_type when) {
  const auto system = std::chrono::system_clock::now() +
                      std::chrono::duration_cast<std::chrono::system_clock::duration>(
                          when - fs::file_time_type::clock::now());
  const std::time_t t = std::chrono::system_clock::to_time_t(system);
  std::tm local{};
#if REX_PLATFORM_WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%Y/%m/%d %H:%M", &local);
  return text;
}

// The title as ASCII. A PS1 title may use fullwidth Shift-JIS; its ASCII
// forms (0x8240 space, 0x8260.. letters, 0x824F.. digits, 0x8193 '%') are
// folded back, anything else becomes '?'.
std::string TitleText(const uint8_t* title) {
  std::string text;
  for (size_t i = 0; i < kTitleBytes && title[i]; ++i) {
    const uint8_t c = title[i];
    if (c < 0x80) {
      text += char(c);
      continue;
    }
    if (i + 1 >= kTitleBytes)
      break;
    const uint16_t sjis = uint16_t(c << 8 | title[++i]);
    if (sjis == 0x8140)
      text += ' ';
    else if (sjis >= 0x824F && sjis <= 0x8258)
      text += char('0' + sjis - 0x824F);
    else if (sjis >= 0x8260 && sjis <= 0x8279)
      text += char('A' + sjis - 0x8260);
    else if (sjis >= 0x8281 && sjis <= 0x829A)
      text += char('a' + sjis - 0x8281);
    else if (sjis == 0x8193)
      text += '%';
    else if (sjis == 0x817C)
      text += '-';
    else
      text += '?';
  }
  return text;
}

// "CASTLEVANIA-01 ALUCARD 020%": the name between the first and last words.
// The percentage is not read from it: the port's ASCII rewrite of the PS1's
// fullwidth digits drops some ("020" is 200%, "010\x08" 185%).
void ParseTitle(const std::string& title, SaveSlotInfo& slot) {
  const size_t first = title.find(' ');
  const size_t last = title.rfind(' ');
  if (first == std::string::npos)
    return;
  if (last != first && !title.empty() && title.back() == '%')
    slot.name = title.substr(first + 1, last - first - 1);
  else
    slot.name = title.substr(first + 1);
}

// The map completion, as the load menu shows it: tenths, truncated.
std::string Completion(const Bytes& block) {
  const int rooms = block[kRoomsOffset] << 8 | block[kRoomsOffset + 1];
  const int tenths = rooms * 1000 / kRoomCount;
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "%";
}

// A frame: 4bpp, low nibble first, through a 16 entry BGR555 CLUT. Unlike on
// the PS1 the CLUT entries are big-endian: the port writes them as u16s.
std::vector<uint8_t> IconRgba(const uint8_t* pixels, const uint8_t* clut) {
  std::vector<uint8_t> rgba(size_t(kSaveThumbWidth) * kSaveThumbHeight * 4);
  for (int i = 0; i < kSaveThumbWidth * kSaveThumbHeight; ++i) {
    const uint8_t pair = pixels[i / 2];
    const int index = (i & 1) ? pair >> 4 : pair & 0xF;
    const uint16_t c = uint16_t(clut[index * 2] << 8 | clut[index * 2 + 1]);
    const auto expand = [](int v) { return uint8_t(v << 3 | v >> 2); };
    rgba[size_t(i) * 4 + 0] = expand(c & 0x1F);
    rgba[size_t(i) * 4 + 1] = expand((c >> 5) & 0x1F);
    rgba[size_t(i) * 4 + 2] = expand((c >> 10) & 0x1F);
    // Black with the STP bit clear is the PS1's transparent color.
    rgba[size_t(i) * 4 + 3] = c == 0 ? 0 : 255;
  }
  return rgba;
}

fs::path IconCachePath(const fs::path& root) {
  return root / "cache" / "save_icons.bin";
}

// The cached icon set, or empty before the game has run once.
Bytes ReadIconSet(const fs::path& root) {
  Bytes data = ReadFile(IconCachePath(root), sizeof(kIconCacheMagic) + kIconSetBytes + 1);
  if (data.size() != sizeof(kIconCacheMagic) + kIconSetBytes ||
      std::memcmp(data.data(), kIconCacheMagic, sizeof(kIconCacheMagic)) != 0)
    return {};
  data.erase(data.begin(), data.begin() + sizeof(kIconCacheMagic));
  return IsIconSet(data.data()) ? data : Bytes{};
}

// The icon's three frames from the game's set, else the one the block keeps.
// The BIOS's memory card screen flips three frames every 11 vblanks.
void ReadIcon(const Bytes& block, const Bytes& set, SaveSlotInfo& slot) {
  const uint32_t icon = LoadBE32(&block[kCardIconOffset]);
  if (set.empty() || icon >= uint32_t(kIconCount)) {
    slot.frames.push_back(IconRgba(&block[kIconOffset], &block[kClutOffset]));
    return;
  }
  const uint8_t* clut = &set[kIconSetCluts + icon * 0x20];
  for (int i = 0; i < kIconFrames; ++i)
    slot.frames.push_back(
        IconRgba(&set[kIconSetIcons + icon * kIconBytes + size_t(i) * kIconFrameBytes], clut));
  slot.frame_seconds = 11.0f / 60.0f;
}

// The XContent header the SDK writes for a save, named for `slot`.
Bytes ContentHeader(int slot) {
  Bytes header(kHeaderSize, 0);
  header[3] = 1;
  header[7] = 1;
  const char* display = "Castlevania: SOTN Saved Game";
  for (size_t i = 0; display[i]; ++i)
    header[8 + i * 2 + 1] = uint8_t(display[i]);
  const std::string name = FileName(slot);
  std::memcpy(&header[0x108], name.data(), name.size());
  for (int i = 0; i < 4; ++i)
    header[0x140 + i] = uint8_t(kTitleId >> (24 - 8 * i));
  return header;
}

// A header that came with an imported save, renamed for the slot it goes to.
Bytes RenamedHeader(Bytes header, int slot) {
  if (header.size() != kHeaderSize)
    return ContentHeader(slot);
  const std::string name = FileName(slot);
  std::fill(header.begin() + 0x108, header.begin() + 0x108 + 0x28, 0);
  std::memcpy(&header[0x108], name.data(), name.size());
  return header;
}

// The block and header inside an extracted export, or beside a bare block.
std::string PrepareFrom(const fs::path& file, PreparedSave& out) {
  out.block = ReadFile(file, kBlockSize + 1);
  if (out.block.empty())
    return Tr("saves", "err_read");
  if (!IsBlock(out.block))
    return Tr("saves", "err_not_save");
  std::error_code ec;
  for (const fs::path& header :
       {fs::path(file).concat(".header"),
        file.parent_path().parent_path().parent_path() / "Headers" / "00000001" /
            (file.filename().string() + ".header")}) {
    if (fs::is_regular_file(header, ec)) {
      out.header = ReadFile(header, kHeaderSize + 1);
      break;
    }
  }
  return {};
}

// The shallowest block inside an extracted archive, then the first by name.
fs::path FindBlockIn(const fs::path& dir) {
  std::vector<fs::path> found;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (it->is_regular_file(entry_ec) && it->file_size(entry_ec) == kBlockSize)
      found.push_back(it->path());
  }
  std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
    const auto depth = [](const fs::path& p) { return std::distance(p.begin(), p.end()); };
    return depth(a) != depth(b) ? depth(a) < depth(b) : a < b;
  });
  return found.empty() ? fs::path() : found.front();
}

}  // namespace

void CacheSaveIcons(const fs::path& user_data_root,
                    const std::function<const uint8_t*(uint32_t)>& guest) {
  const uint8_t* set = guest(kIconSetAddress);
  if (!set || !IsIconSet(set)) {
    REXLOG_WARN("Save icons: no icon set at {:08X}, the start screen keeps them still",
                kIconSetAddress);
    return;
  }
  Bytes data(kIconCacheMagic, kIconCacheMagic + sizeof(kIconCacheMagic));
  data.insert(data.end(), set, set + kIconSetBytes);
  if (ReadFile(IconCachePath(user_data_root), data.size() + 1) == data)
    return;
  std::error_code ec;
  fs::create_directories(IconCachePath(user_data_root).parent_path(), ec);
  if (!WriteFile(IconCachePath(user_data_root), data))
    REXLOG_WARN("Save icons: cannot write {}", rex::path_to_utf8(IconCachePath(user_data_root)));
}

std::vector<SaveSlotInfo> ReadSaveSlots(const fs::path& user_data_root) {
  std::vector<SaveSlotInfo> slots(kSaveSlotCount);
  const Bytes icons = ReadIconSet(user_data_root);
  for (int i = 0; i < kSaveSlotCount; ++i) {
    const fs::path file = BlockPath(user_data_root, i);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec))
      continue;
    SaveSlotInfo& slot = slots[size_t(i)];
    slot.used = true;
    ReadRandoRecord(user_data_root, i, slot);
    if (const auto when = fs::last_write_time(file, ec); !ec)
      slot.modified = FormatModified(when);
    const Bytes block = ReadFile(file, kBlockSize);
    if (!IsBlock(block))
      continue;
    ParseTitle(TitleText(block.data() + kTitleOffset), slot);
    slot.completion = Completion(block);
    ReadIcon(block, icons, slot);
  }
  return slots;
}

std::string ExportSaveSlot(const fs::path& user_data_root, int slot, const fs::path& archive) {
  const fs::path block = BlockPath(user_data_root, slot);
  std::error_code ec;
  if (!fs::is_regular_file(block, ec))
    return Tr("saves", "err_read");
  // The block and its header side by side, named as the game names them.
  const fs::path staging = user_data_root / "cache" / "save_export";
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  fs::copy_file(block, staging / FileName(slot), ec);
  if (fs::is_regular_file(HeaderPath(user_data_root, slot), ec))
    fs::copy_file(HeaderPath(user_data_root, slot), staging / (FileName(slot) + ".header"), ec);
  fs::remove(archive, ec);
  std::string error;
  const bool ok = rex::filesystem::CreateZip(staging, archive, error);
  fs::remove_all(staging, ec);
  if (!ok)
    return error.empty() ? Tr("saves", "err_write", rex::path_to_utf8(archive)) : error;
  return {};
}

std::string PrepareSaveImport(const fs::path& user_data_root, const fs::path& source,
                              PreparedSave& out) {
  const Bytes magic = ReadFile(source, 4);
  if (magic.size() < 4)
    return Tr("saves", "err_read");
  if (std::memcmp(magic.data(), "PK\x03\x04", 4) != 0)
    return PrepareFrom(source, out);
  const fs::path staging = user_data_root / "cache" / "save_import";
  std::error_code ec;
  fs::remove_all(staging, ec);
  std::string error;
  if (!rex::filesystem::ExtractZip(source, staging, error)) {
    fs::remove_all(staging, ec);
    return error;
  }
  const fs::path found = FindBlockIn(staging);
  error = found.empty() ? Tr("saves", "err_no_save_in_zip") : PrepareFrom(found, out);
  fs::remove_all(staging, ec);
  return error;
}

std::string WriteSaveSlot(const fs::path& user_data_root, int slot, const PreparedSave& save) {
  const fs::path block = BlockPath(user_data_root, slot);
  const fs::path header = HeaderPath(user_data_root, slot);
  std::error_code ec;
  fs::create_directories(block.parent_path(), ec);
  fs::create_directories(header.parent_path(), ec);
  // Written aside first, so a failure leaves the old save as it was.
  fs::path temp = block;
  temp += ".importing";
  if (!WriteFile(temp, save.block)) {
    fs::remove(temp, ec);
    return Tr("saves", "err_write", rex::path_to_utf8(block));
  }
  fs::rename(temp, block, ec);
  if (ec) {
    fs::remove(temp, ec);
    return Tr("saves", "err_write", rex::path_to_utf8(block));
  }
  if (!WriteFile(header, RenamedHeader(save.header, slot)))
    return Tr("saves", "err_write", rex::path_to_utf8(header));
  // An imported save carries no world of its own and loads as vanilla.
  fs::remove(RandoRecordPath(user_data_root, slot), ec);
  return {};
}

std::string DeleteSaveSlot(const fs::path& user_data_root, int slot) {
  const fs::path block = BlockPath(user_data_root, slot);
  std::error_code ec;
  fs::remove_all(block.parent_path(), ec);
  if (ec)
    return ec.message();
  fs::remove(HeaderPath(user_data_root, slot), ec);
  fs::remove(RandoRecordPath(user_data_root, slot), ec);
  return {};
}

std::string SaveSlotExportName(int slot) {
  char name[48];
  std::snprintf(name, sizeof(name), "nocturnerecomp-save-%02d.zip", slot + 1);
  return name;
}

}  // namespace nocturne
