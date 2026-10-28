#include "font_overlay.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "font_patch.h"
#include "fonts_zip.h"
#include "glyph_pack.generated.h"
#include "latin1_font.h"
#include "target.h"

namespace nocturne {
namespace {

// The X_STATUS_* macros cast to a bare X_STATUS, which only names a type
// inside rex; the VFS interfaces we override spell it the same way.
using rex::X_STATUS;

constexpr const char* kGuestFontPath = "game:\\MEDIA\\font_ja.xpr";
constexpr const char* kFontFileName = "font_ja.xpr";
constexpr const char* kStringsFileName = "strings_ja.bin";
constexpr const char* kOverlayMount = "\\Device\\NocturneMediaOverlay";
constexpr const char* kGuestFontsZipPath = "game:\\DATA\\fonts\\fonts.zip";
constexpr const char* kFontsOverlayMount = "\\Device\\NocturneFontsOverlay";

// Serves a byte buffer we own.
class MemoryFile : public rex::filesystem::File {
 public:
  MemoryFile(uint32_t access, rex::filesystem::Entry* entry, const std::vector<uint8_t>* data)
      : File(access, entry), data_(data) {}

  void Destroy() override { delete this; }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override {
    if (byte_offset >= data_->size()) {
      *out_bytes_read = 0;
      return X_STATUS_END_OF_FILE;
    }
    const size_t count = std::min(buffer.size(), data_->size() - byte_offset);
    std::memcpy(buffer.data(), data_->data() + byte_offset, count);
    *out_bytes_read = count;
    return X_STATUS_SUCCESS;
  }

  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t* out_bytes_written) override {
    *out_bytes_written = 0;
    return X_STATUS_ACCESS_DENIED;
  }

 private:
  const std::vector<uint8_t>* data_;
};

// Serves a real file on the host, so every sibling in MEDIA that we are not
// replacing behaves exactly as it did before the shadow was installed.
class HostFile : public rex::filesystem::File {
 public:
  HostFile(uint32_t access, rex::filesystem::Entry* entry, const std::filesystem::path& path)
      : File(access, entry), stream_(path, std::ios::binary) {}

  void Destroy() override { delete this; }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override {
    *out_bytes_read = 0;
    if (!stream_) {
      return X_STATUS_UNSUCCESSFUL;
    }
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
    if (!stream_) {
      return X_STATUS_END_OF_FILE;
    }
    stream_.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(buffer.size()));
    const size_t count = static_cast<size_t>(stream_.gcount());
    *out_bytes_read = count;
    return count ? X_STATUS_SUCCESS : X_STATUS_END_OF_FILE;
  }

  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t* out_bytes_written) override {
    *out_bytes_written = 0;
    return X_STATUS_ACCESS_DENIED;
  }

 private:
  std::ifstream stream_;
};

class MemoryEntry : public rex::filesystem::Entry {
 public:
  MemoryEntry(rex::filesystem::Device* device, rex::filesystem::Entry* parent,
              const std::string& path, std::vector<uint8_t> data)
      : Entry(device, parent, path), data_(std::move(data)) {
    attributes_ = rex::filesystem::kFileAttributeReadOnly;
    size_ = data_.size();
    allocation_size_ = data_.size();
  }

  X_STATUS Open(uint32_t desired_access, rex::filesystem::File** out_file) override {
    *out_file = new MemoryFile(desired_access, this, &data_);
    return X_STATUS_SUCCESS;
  }

 private:
  std::vector<uint8_t> data_;
};

class HostEntry : public rex::filesystem::Entry {
 public:
  HostEntry(rex::filesystem::Device* device, rex::filesystem::Entry* parent,
            const std::string& path, std::filesystem::path host_path, size_t size)
      : Entry(device, parent, path), host_path_(std::move(host_path)) {
    attributes_ = rex::filesystem::kFileAttributeReadOnly;
    size_ = size;
    allocation_size_ = size;
  }

  X_STATUS Open(uint32_t desired_access, rex::filesystem::File** out_file) override {
    *out_file = new HostFile(desired_access, this, host_path_);
    return X_STATUS_SUCCESS;
  }

 private:
  std::filesystem::path host_path_;
};

// The shadow MEDIA directory. Its children are the real directory's files,
// with font_ja.xpr swapped for the composed image.
//
// This has to shadow the whole directory rather than just the one file,
// because VirtualFileSystem::OpenFile resolves only the *parent* path through
// ResolvePath (which honours symlinks) and then looks the filename up with
// Entry::GetChild, which does not. A symlink on the full file path is
// therefore never consulted on the open path -- only a directory-level shadow
// puts our entry where GetChild will find it.
class MediaDirEntry : public rex::filesystem::Entry {
 public:
  MediaDirEntry(rex::filesystem::Device* device, const std::string& path)
      : Entry(device, nullptr, path) {
    attributes_ = rex::filesystem::kFileAttributeDirectory;
  }

  X_STATUS Open(uint32_t, rex::filesystem::File**) override { return X_STATUS_ACCESS_DENIED; }

  void Add(std::unique_ptr<rex::filesystem::Entry> child) {
    children_.push_back(std::move(child));
  }
};

class MediaOverlayDevice : public rex::filesystem::Device {
 public:
  explicit MediaOverlayDevice(const std::string& mount_path)
      : Device(mount_path), name_(mount_path.substr(mount_path.rfind('\\') + 1)) {
    root_ = std::make_unique<MediaDirEntry>(this, "");
  }

  bool Initialize() override { return true; }
  void Dump(rex::string::StringBuffer*) override {}

  rex::filesystem::Entry* ResolvePath(const std::string_view path) override {
    // An empty path is the documented alias for the device root.
    return path.empty() ? root_.get() : root_->ResolvePath(path);
  }

  MediaDirEntry* root() { return root_.get(); }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  std::string name_;
  std::unique_ptr<MediaDirEntry> root_;
};

// The file's real name in dir, matched without case (the folder comes from an
// extraction, and the host file system may be case sensitive).
std::filesystem::path FindFile(const std::filesystem::path& dir, std::string_view name) {
  std::error_code ec;
  for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
    if (item.is_regular_file(ec) &&
        rex::string::utf8_equal_case(item.path().filename().string(), name))
      return item.path();
  }
  return {};
}

std::vector<uint8_t> ReadHostFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return {};
  std::vector<uint8_t> data(static_cast<size_t>(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return in ? data : std::vector<uint8_t>{};
}

// Shadows the guest directory at dir_device_path, whose files are in
// host_dir, with a device mounted at `mount`: every file is mirrored except
// `files`, which are served from memory. False when nothing was installed.
bool ShadowDirectory(rex::filesystem::VirtualFileSystem* vfs, const std::string& dir_device_path,
                     const std::filesystem::path& host_dir, const char* mount,
                     std::vector<std::pair<std::string, std::vector<uint8_t>>> files) {
  auto device = std::make_unique<MediaOverlayDevice>(mount);
  MediaOverlayDevice* device_raw = device.get();
  MediaDirEntry* root = device_raw->root();

  std::error_code ec;
  for (const auto& item : std::filesystem::directory_iterator(host_dir, ec)) {
    if (ec) {
      break;
    }
    if (!item.is_regular_file()) {
      // MEDIA and DATA\fonts are flat in every layout we have seen; a
      // subdirectory would need recursive mirroring, so bail rather than
      // silently hide it.
      REXLOG_WARN("font overlay: {} contains a subdirectory, not shadowing", host_dir.string());
      return false;
    }
    const std::string name = item.path().filename().string();
    if (std::any_of(files.begin(), files.end(), [&](const auto& f) {
          return rex::string::utf8_equal_case(name, f.first);
        })) {
      continue;
    }
    root->Add(std::make_unique<HostEntry>(device_raw, root, name, item.path(),
                                          static_cast<size_t>(item.file_size(ec))));
  }
  if (ec) {
    REXLOG_WARN("font overlay: could not enumerate {}, not shadowing", host_dir.string());
    return false;
  }
  for (auto& [name, file] : files) {
    root->Add(std::make_unique<MemoryEntry>(device_raw, root, name, std::move(file)));
  }

  if (!vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("font overlay: could not register {}", mount);
    return false;
  }
  // Shadow the directory, not the file -- see MediaDirEntry.
  if (!vfs->RegisterSymbolicLink(dir_device_path, mount)) {
    REXLOG_WARN("font overlay: could not link {} to {}", dir_device_path, mount);
    vfs->UnregisterDevice(mount);
    return false;
  }
  return true;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t at) {
  return static_cast<uint32_t>(d[at]) << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];
}

void PutBe32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
  d[at] = static_cast<uint8_t>(v >> 24);
  d[at + 1] = static_cast<uint8_t>(v >> 16);
  d[at + 2] = static_cast<uint8_t>(v >> 8);
  d[at + 3] = static_cast<uint8_t>(v);
}

// strings_<code>.bin: an entry count, then 12-byte entries {text, font name,
// scale in thousandths}, offsets from the file start; the text is
// NUL-terminated UTF-16BE.
constexpr size_t kStringEntrySize = 12;

struct StringEntry {
  std::vector<uint8_t> text;  // without the terminator
  uint32_t scale;
  bool operator==(const StringEntry&) const = default;
};

bool ParseStrings(const std::vector<uint8_t>& d, std::vector<StringEntry>& out) {
  if (d.size() < 4)
    return false;
  const uint32_t count = Be32(d, 0);
  if (count == 0 || count > (d.size() - 4) / kStringEntrySize)
    return false;
  out.clear();
  for (uint32_t i = 0; i < count; ++i) {
    const size_t entry = 4 + i * kStringEntrySize;
    size_t at = Be32(d, entry);
    const size_t start = at;
    while (at + 1 < d.size() && (d[at] | d[at + 1]))
      at += 2;
    if (at + 1 >= d.size())
      return false;
    out.push_back({{d.begin() + start, d.begin() + at}, Be32(d, entry + 8)});
  }
  return true;
}

// World's strings_ja.bin with JP's wording. The code is World's, so the ids
// must stay World's: JP's file splits one entry in two and adds others, which
// shifts every later id. The two lists are aligned (longest common
// subsequence); an entry in a run that differs on both sides takes JP's text
// and scale only when the runs are the same length, i.e. it is the same entry
// reworded. Everything else keeps World's. Empty on failure.
std::vector<uint8_t> MergeJapaneseStrings(const std::vector<uint8_t>& world_file,
                                          const std::vector<uint8_t>& jp_file,
                                          size_t& reworded) {
  std::vector<StringEntry> w, j;
  if (!ParseStrings(world_file, w) || !ParseStrings(jp_file, j))
    return {};

  // lcs[a][b]: the common subsequence length of w[a:] and j[b:].
  const size_t n = w.size(), m = j.size();
  std::vector<std::vector<uint16_t>> lcs(n + 1, std::vector<uint16_t>(m + 1, 0));
  for (size_t a = n; a-- > 0;)
    for (size_t b = m; b-- > 0;)
      lcs[a][b] = w[a] == j[b] ? lcs[a + 1][b + 1] + 1
                               : std::max(lcs[a + 1][b], lcs[a][b + 1]);

  std::vector<uint8_t> out = world_file;
  reworded = 0;
  auto take_run = [&](size_t a0, size_t a1, size_t b0, size_t b1) {
    if (a1 - a0 != b1 - b0)
      return;
    for (size_t k = 0; k < a1 - a0; ++k) {
      const StringEntry& e = j[b0 + k];
      const size_t entry = 4 + (a0 + k) * kStringEntrySize;
      if (out.size() & 1)
        out.push_back(0);
      PutBe32(out, entry, static_cast<uint32_t>(out.size()));
      PutBe32(out, entry + 8, e.scale);
      out.insert(out.end(), e.text.begin(), e.text.end());
      out.insert(out.end(), {0, 0});
      ++reworded;
    }
  };
  size_t a = 0, b = 0, run_a = 0, run_b = 0;
  while (a < n && b < m) {
    if (w[a] == j[b]) {
      take_run(run_a, a, run_b, b);
      run_a = ++a;
      run_b = ++b;
    } else if (lcs[a + 1][b] >= lcs[a][b + 1]) {
      ++a;
    } else {
      ++b;
    }
  }
  take_run(run_a, n, run_b, m);
  return out;
}

// The button that used to say "Return to Arcade" (id 31, description id 48)
// quits the game, so every strings_<code>.bin gets new text for both.
constexpr size_t kQuitStringId = 31;
constexpr size_t kQuitDescriptionId = 48;
struct QuitLabel {
  const char* code;
  const char16_t* text;
  const char16_t* description;
};
constexpr QuitLabel kQuitLabels[] = {
    {"en", u"Quit game", u"Quit the game"},
    {"de", u"Spiel beenden", u"Das Spiel beenden"},
    {"es", u"Salir del juego", u"Salir del juego"},
    {"fr", u"Quitter le jeu", u"Quitter le jeu"},
    {"it", u"Esci dal gioco", u"Esci dal gioco"},
    {"ja", u"ゲームを終了", u"ゲームを終了"},
};

// Points entry `id` of `file` at `text`, appended at the end, as in
// take_run. Unchanged if the file has no such entry.
void SetString(std::vector<uint8_t>& file, size_t id, const char16_t* text) {
  const size_t entry = 4 + id * kStringEntrySize;
  if (file.size() & 1)
    file.push_back(0);
  PutBe32(file, entry, static_cast<uint32_t>(file.size()));
  for (const char16_t* c = text; *c; ++c) {
    file.push_back(static_cast<uint8_t>(*c >> 8));
    file.push_back(static_cast<uint8_t>(*c));
  }
  file.insert(file.end(), {0, 0});
}

std::vector<uint8_t> RelabelQuit(std::vector<uint8_t> file, const QuitLabel& label) {
  std::vector<StringEntry> entries;
  if (!ParseStrings(file, entries) || entries.size() <= kQuitDescriptionId)
    return file;
  SetString(file, kQuitStringId, label.text);
  SetString(file, kQuitDescriptionId, label.description);
  return file;
}

}  // namespace

unsigned InstallJapaneseFontOverlay(rex::Runtime* runtime) {
  if (!runtime) {
    return 0;
  }
  auto* vfs = runtime->file_system();
  if (!vfs) {
    return 0;
  }

  rex::filesystem::Entry* font_entry = vfs->ResolvePath(kGuestFontPath);
  if (!font_entry) {
    REXLOG_DEBUG("font overlay: no {} to patch", kGuestFontPath);
    return 0;
  }

  // We need the real host directory to mirror. Only a host-backed game mount
  // can be shadowed this way; a disc image or STFS container has no directory
  // on disk to enumerate, so leave those alone.
  const auto* host_entry = dynamic_cast<const rex::filesystem::HostPathEntry*>(font_entry);
  if (!host_entry) {
    REXLOG_DEBUG("font overlay: game data is not a host directory, skipping");
    return 0;
  }
  const std::filesystem::path font_host_path = host_entry->host_path();
  const std::filesystem::path media_dir = font_host_path.parent_path();

  rex::filesystem::Entry* media_entry = font_entry->parent();
  if (!media_entry) {
    REXLOG_DEBUG("font overlay: font has no parent directory entry, skipping");
    return 0;
  }
  const std::string media_device_path = media_entry->absolute_path();

  // With JP installed, Japanese is JP's: its strings_ja.bin (merged onto
  // World's ids) and the font built for it. The two are a pair, since each
  // release's font is a subset made for its own strings; JP's covers all of
  // World's characters too.
  std::vector<uint8_t> jp_strings;
  std::vector<uint8_t> original;
  if (JpOverlaysAvailable()) {
    const std::filesystem::path jp_media = JpDir() / "MEDIA";
    const std::filesystem::path jp_font_path = FindFile(jp_media, kFontFileName);
    const std::filesystem::path jp_strings_path = FindFile(jp_media, kStringsFileName);
    const std::filesystem::path world_strings_path = FindFile(media_dir, kStringsFileName);
    if (!jp_font_path.empty() && !jp_strings_path.empty() && !world_strings_path.empty()) {
      size_t reworded = 0;
      jp_strings = MergeJapaneseStrings(ReadHostFile(world_strings_path),
                                        ReadHostFile(jp_strings_path), reworded);
      if (!jp_strings.empty())
        original = ReadHostFile(jp_font_path);
      if (jp_strings.empty() || original.empty()) {
        jp_strings.clear();
        REXLOG_WARN("font overlay: could not read JP's {} and {}; Japanese stays World's",
                    kStringsFileName, kFontFileName);
      } else {
        REXLOG_INFO("font overlay: Japanese uses JP's font and {} reworded string(s)", reworded);
      }
    }
  }
  if (original.empty()) {
    original = ReadHostFile(font_host_path);
    if (original.empty()) {
      REXLOG_DEBUG("font overlay: cannot read {}", font_host_path.string());
      return 0;
    }
  }

  FontPatchResult patched = PatchFont(original, kSupplementalGlyphs);
  if (patched.out_of_space) {
    REXLOG_WARN("font overlay: ran out of atlas space; some glyphs were dropped");
  }
  if (patched.image.empty()) {
    REXLOG_DEBUG("font overlay: not patching font_ja.xpr ({})",
                 patched.error ? patched.error : "unknown");
    patched.image = std::move(original);  // the strings are still relabelled
  }
  const size_t patched_size = patched.image.size();

  // Every language's strings with the quit button relabelled. Japanese is
  // JP's when it is merged in, World's otherwise.
  std::vector<std::pair<std::string, std::vector<uint8_t>>> strings;
  for (const QuitLabel& label : kQuitLabels) {
    const std::string name = std::string("strings_") + label.code + ".bin";
    std::vector<uint8_t> file;
    if (!jp_strings.empty() && name == kStringsFileName) {
      file = std::move(jp_strings);
    } else {
      const std::filesystem::path path = FindFile(media_dir, name);
      if (!path.empty())
        file = ReadHostFile(path);
    }
    if (!file.empty())
      strings.emplace_back(name, RelabelQuit(std::move(file), label));
  }

  strings.emplace_back(kFontFileName, std::move(patched.image));
  if (!ShadowDirectory(vfs, media_device_path, media_dir, kOverlayMount, std::move(strings)))
    return 0;

  // Verify through the same two-step the open path actually uses: resolve the
  // parent, then GetChild the filename. Resolving the full path would only
  // prove the symlink exists, which is what the previous attempt got wrong.
  rex::filesystem::Entry* parent = vfs->ResolvePath("d:\\media");
  rex::filesystem::Entry* child = parent ? parent->GetChild(kFontFileName) : nullptr;
  if (!child) {
    REXLOG_WARN("font overlay: 'd:\\media' + GetChild({}) does not resolve", kFontFileName);
  } else if (child->size() != patched_size) {
    REXLOG_WARN("font overlay: open path still yields the unpatched font ({} bytes, "
                "expected {})", child->size(), patched_size);
  } else {
    REXLOG_INFO("font overlay: open path yields the composed font ({} bytes)", child->size());
  }

  REXLOG_INFO("font overlay: added {} supplemental glyph(s) to font_ja.xpr, aliased {} fullwidth",
              patched.glyphs_added, patched.glyphs_aliased);
  return patched.glyphs_added;
}

bool InstallLatin1FontOverlay(rex::Runtime* runtime) {
  auto* vfs = runtime ? runtime->file_system() : nullptr;
  if (!vfs) {
    return false;
  }
  const auto* zip_entry =
      dynamic_cast<const rex::filesystem::HostPathEntry*>(vfs->ResolvePath(kGuestFontsZipPath));
  if (!zip_entry || !zip_entry->parent()) {
    REXLOG_DEBUG("latin1 font: {} is not a host file, skipping", kGuestFontsZipPath);
    return false;
  }
  const std::filesystem::path zip_path = zip_entry->host_path();
  const std::vector<uint8_t> zip = ReadHostFile(zip_path);
  const std::vector<ZipEntry> entries = ParseZip(zip);
  auto find = [&](std::string_view name) {
    return std::find_if(entries.begin(), entries.end(), [&](const ZipEntry& e) {
      return rex::string::utf8_equal_case(e.name, name);
    });
  };
  if (entries.empty() || find(kLatin1FontName) != entries.end()) {
    return false;  // World's, which has the table already
  }
  const auto game_font = find(kGameFontName);
  std::vector<uint8_t> font;
  if (game_font != entries.end())
    font = Extract(zip, *game_font);
  std::vector<uint8_t> served = zip;
  for (const bool upres : {false, true}) {
    const std::vector<uint8_t> table = BuildLatin1Font(font, upres);
    if (table.empty() || served.empty())
      break;
    served = AddStoredEntry(served, upres ? kLatin1UpresFontName : kLatin1FontName, table);
  }
  if (ParseZip(served).size() != entries.size() + 2) {
    REXLOG_WARN("latin1 font: could not draw the table from {}, accents stay blank",
                zip_path.string());
    return false;
  }

  std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
  files.emplace_back(zip_path.filename().string(), std::move(served));
  if (!ShadowDirectory(vfs, zip_entry->parent()->absolute_path(), zip_path.parent_path(),
                       kFontsOverlayMount, std::move(files)))
    return false;
  REXLOG_INFO("latin1 font: added {} and {} to {}", kLatin1FontName, kLatin1UpresFontName,
              kGuestFontsZipPath);
  return true;
}

}  // namespace nocturne
