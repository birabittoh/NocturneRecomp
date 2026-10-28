// nocturnerecomp - ReXGlue Recompiled Project

#include "guest_image.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>

// The stripped default.xex (scripts/gen-guest-image.py), linked in by
// guest-image.S.
extern "C" {
extern const uint8_t kGuestImageData[];
extern const uint8_t kGuestImageDataEnd[];
}

namespace nocturne {
namespace {

using namespace rex::filesystem;
using rex::X_STATUS;

constexpr const char* kMountPath = "\\Device\\GuestImage";
constexpr const char* kFileName = "default.xex";

std::span<const uint8_t> Image() {
  return {kGuestImageData, static_cast<size_t>(kGuestImageDataEnd - kGuestImageData)};
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

std::string MountGuestImage(VirtualFileSystem* file_system) {
  auto device = std::make_unique<ImageDevice>();
  device->Initialize();
  file_system->RegisterDevice(std::move(device));
  REXLOG_INFO("Guest image: {} bytes embedded, mounted at {}", Image().size(), kMountPath);
  return std::string(kMountPath) + "\\" + kFileName;
}

}  // namespace nocturne
