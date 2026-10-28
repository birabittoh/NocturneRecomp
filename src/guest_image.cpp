// nocturnerecomp - ReXGlue Recompiled Project

#include "guest_image.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>
#include <rex/system/xex_module.h>

// The XEX header and the recipe (scripts/guest_recipe.py `pack`), linked in by
// guest-image.S.
extern "C" {
extern const uint8_t kGuestRecipeData[];
extern const uint8_t kGuestRecipeDataEnd[];
}

namespace nocturne {
namespace {

using namespace rex::filesystem;
using rex::X_STATUS;

constexpr const char* kMountPath = "\\Device\\GuestImage";
constexpr const char* kFileName = "default.xex";

// The header, then the image: the file the module is loaded from.
std::vector<uint8_t> g_file;

std::span<const uint8_t> Image() {
  return g_file;
}

uint32_t ReadLE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

uint32_t ReadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

// The blob's layout, all little-endian; see scripts/guest_recipe.py `pack`.
struct RecipeHeader {
  uint32_t version, image_size, load_address, world_timestamp, jp_timestamp, header_size,
      copy_count, patch_count, pool_size;
};
constexpr uint32_t kRecipeVersion = 1;

uint32_t PeTimestamp(const std::vector<uint8_t>& image) {
  if (image.size() < 0x40)
    return 0;
  const uint32_t pe = ReadLE32(image.data() + 0x3C);
  return pe + 12 <= image.size() ? ReadBE32(image.data() + pe + 8) : 0;
}

// The game folder's default.xex, World's or JP's, turned into World's image.
bool BuildFile(const std::filesystem::path& xex_path) {
  const uint8_t* blob = kGuestRecipeData;
  const size_t blob_size = static_cast<size_t>(kGuestRecipeDataEnd - kGuestRecipeData);
  RecipeHeader h;
  if (blob_size < 4 + sizeof(h) || std::memcmp(blob, "NRGR", 4) != 0) {
    REXLOG_ERROR("Guest image: the linked recipe is malformed");
    return false;
  }
  std::memcpy(&h, blob + 4, sizeof(h));
  const uint8_t* header = blob + 4 + sizeof(h);
  const uint8_t* copies = header + h.header_size;
  const uint8_t* patches = copies + size_t(h.copy_count) * 12;
  const uint8_t* pool = patches + size_t(h.patch_count) * 8;
  if (h.version != kRecipeVersion || pool + h.pool_size != blob + blob_size) {
    REXLOG_ERROR("Guest image: the linked recipe is malformed");
    return false;
  }

  std::ifstream in(xex_path, std::ios::binary);
  std::vector<uint8_t> xex((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<uint8_t> source;
  if (xex.empty() ||
      !rex::runtime::XexModule::ExtractBaseImage(xex.data(), xex.size(), source)) {
    REXLOG_ERROR("Guest image: can't read {}", xex_path.string());
    return false;
  }
  xex = {};
  const uint32_t timestamp = PeTimestamp(source);
  const bool jp = timestamp == h.jp_timestamp;
  if (!jp && timestamp != h.world_timestamp) {
    REXLOG_ERROR("Guest image: {} is neither the World nor the JP release (0x{:08X})",
                 xex_path.string(), timestamp);
    return false;
  }

  g_file.assign(h.header_size + size_t(h.image_size), 0);
  std::memcpy(g_file.data(), header, h.header_size);
  uint8_t* image = g_file.data() + h.header_size;
  for (uint32_t i = 0; i < h.copy_count; ++i) {
    const uint32_t dst = ReadLE32(copies + 12 * i);
    const uint32_t src = jp ? ReadLE32(copies + 12 * i + 4) : dst;
    const uint32_t size = ReadLE32(copies + 12 * i + 8);
    if (size_t(dst) + size > h.image_size || size_t(src) + size > source.size()) {
      REXLOG_ERROR("Guest image: copy {} is out of range", i);
      g_file = {};
      return false;
    }
    std::memcpy(image + dst, source.data() + src, size);
  }
  size_t pool_offset = 0;
  for (uint32_t i = 0; i < h.patch_count; ++i) {
    const uint32_t dst = ReadLE32(patches + 8 * i);
    const uint32_t size = ReadLE32(patches + 8 * i + 4);
    if (size_t(dst) + size > h.image_size || pool_offset + size > h.pool_size) {
      REXLOG_ERROR("Guest image: patch {} is out of range", i);
      g_file = {};
      return false;
    }
    std::memcpy(image + dst, pool + pool_offset, size);
    pool_offset += size;
  }
  REXLOG_INFO("Guest image: built from {}'s {} ({} copies, {} patches)", jp ? "JP" : "World",
              xex_path.string(), h.copy_count, h.patch_count);
  return true;
}

class ImageFile : public File {
 public:
  ImageFile(uint32_t file_access, Entry* entry) : File(file_access, entry) {}

  void Destroy() override { delete this; }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override {
    auto image = Image();
    size_t count = byte_offset < image.size()
                       ? std::min(buffer.size(), image.size() - byte_offset)
                       : 0;
    std::memcpy(buffer.data(), image.data() + byte_offset, count);
    *out_bytes_read = count;
    return X_STATUS_SUCCESS;
  }

  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t*) override {
    return X_STATUS_ACCESS_DENIED;
  }
};

class ImageEntry : public Entry {
 public:
  ImageEntry(Device* device, Entry* parent, const std::string_view path, bool directory)
      : Entry(device, parent, path) {
    attributes_ = directory ? kFileAttributeDirectory : kFileAttributeReadOnly;
    size_ = directory ? 0 : Image().size();
    allocation_size_ = size_;
  }

  X_STATUS Open(uint32_t desired_access, File** out_file) override {
    if (desired_access & (FileAccess::kFileWriteData | FileAccess::kFileAppendData)) {
      return X_STATUS_ACCESS_DENIED;
    }
    *out_file = new ImageFile(desired_access, this);
    return X_STATUS_SUCCESS;
  }

  void AddChild(std::unique_ptr<Entry> child) { children_.push_back(std::move(child)); }
};

class ImageDevice : public Device {
 public:
  ImageDevice() : Device(kMountPath) {}

  bool Initialize() override {
    root_ = std::make_unique<ImageEntry>(this, nullptr, "", true);
    auto file = std::make_unique<ImageEntry>(this, root_.get(), kFileName, false);
    file_ = file.get();
    root_->AddChild(std::move(file));
    return true;
  }

  void Dump(rex::string::StringBuffer* string_buffer) override { root_->Dump(string_buffer, 0); }

  Entry* ResolvePath(const std::string_view path) override {
    if (path.empty()) {
      return root_.get();
    }
    std::string_view name = path;
    if (name.starts_with('\\')) {
      name.remove_prefix(1);
    }
    return rex::string::utf8_equal_case(name, kFileName) ? file_ : nullptr;
  }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 40; }
  uint32_t total_allocation_units() const override { return 0; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  std::string name_ = "GuestImage";
  std::unique_ptr<ImageEntry> root_;
  Entry* file_ = nullptr;
};

}  // namespace

std::string MountGuestImage(VirtualFileSystem* file_system,
                            const std::filesystem::path& xex_path) {
  if (!BuildFile(xex_path))
    return {};
  auto device = std::make_unique<ImageDevice>();
  device->Initialize();
  file_system->RegisterDevice(std::move(device));
  REXLOG_INFO("Guest image: {} bytes, mounted at {}", Image().size(), kMountPath);
  return std::string(kMountPath) + "\\" + kFileName;
}

}  // namespace nocturne
