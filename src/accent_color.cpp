#include "accent_color.h"

#include <cstdio>
#include <fstream>
#include <system_error>

#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xcontent.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/imgui_theme.h>

namespace nocturne {

namespace {

// Guest virtual address of the live Settings -> Accent Color struct: three
// consecutive big-endian uint32 fields (value fits in the low byte, each
// channel 0-15), found by:
//   1. Breakpointing rex::system::XFile::Write and saving with a distinct
//      R/G/B combo -- that gave a guest buffer address holding the same
//      bytes as the save file, but it turned out to be a transient
//      write-staging copy (read back as zero once idle).
//   2. Scanning the game's live memory for the exact byte pattern of a
//      distinctive, *unsaved* slider combo (e.g. R=1 G=9 B=14) turned up a
//      single non-mirrored hit at this address, which tracked further
//      unsaved slider changes 1:1. See scripts/scan_guest_memory.py.
//   3. Confirmed stable across process restarts (it sits in the XEX's
//      static data, not a runtime heap allocation), by loading a save in a
//      fresh instance and reading the same address back.
// The title update relocates the whole image (see NOCTURNE_TU in
// achievements_menu.cpp), so this address shifts too; re-derived for the TU
// build the same way (address confirmed live-tracking R/G/B changes with
// scripts/scan_guest_memory.py against a --tu build).
#ifdef NOCTURNE_TU
constexpr uint32_t kAccentColorGuestAddress = 0x83173A88;
#else
constexpr uint32_t kAccentColorGuestAddress = 0x83173CC8;
#endif
constexpr uint32_t kRedOffset = 0;
constexpr uint32_t kGreenOffset = 4;
constexpr uint32_t kBlueOffset = 8;

// Save layout, reverse-engineered by diffing save files against known R/G/B
// settings changes: three consecutive big-endian int32 fields (value fits in
// the low byte since each channel is 0-15), immediately following the
// original PS1-era save block. Used only as a fallback while the live struct
// above hasn't been populated yet this session (no save loaded, so it still
// reads as all-zero -- not a real value, since the shipped default is
// (0, 0, 8)).
constexpr std::streamoff kSaveRedOffset = 0x580;
constexpr std::streamoff kSaveGreenOffset = 0x584;
constexpr std::streamoff kSaveBlueOffset = 0x588;

// Used when there is no save data at all yet (brand new install, nothing
// saved this session or ever) -- deliberately not the game's own shipped
// default (0, 0, 8) so the overlay looks distinct from "player picked blue".
constexpr ImVec4 kNoSaveFallbackColor = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);  // dark red

uint32_t ReadGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address) {
  const uint8_t* host_address = memory->TranslateVirtual<const uint8_t*>(guest_address);
  return rex::memory::load_and_swap<uint32_t>(host_address);
}

uint32_t ReadBigEndianU32(std::ifstream& file, std::streamoff offset) {
  file.seekg(offset);
  unsigned char bytes[4] = {0, 0, 0, 0};
  file.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
         uint32_t(bytes[3]);
}

ImVec4 ChannelsToColor(uint32_t r, uint32_t g, uint32_t b) {
  return ImVec4(float(r) / 15.0f, float(g) / 15.0f, float(b) / 15.0f, 1.0f);
}

std::string ToHex(uint64_t value, int width) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%0*llX", width, static_cast<unsigned long long>(value));
  return buf;
}

}  // namespace

// No-window dialog whose only job is to periodically poll guest memory for
// accent color changes. ImGuiDialog auto-registers with the drawer in its
// constructor.
class AccentColorWatcher : public rex::ui::ImGuiDialog {
 public:
  explicit AccentColorWatcher(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override { GetAccentColor().RefreshIfChanged(); }
};

AccentColor::AccentColor() = default;
AccentColor::~AccentColor() = default;

void AccentColor::Bind(rex::system::KernelState* kernel_state,
                       std::filesystem::path user_data_root) {
  kernel_state_ = kernel_state;
  user_data_root_ = std::move(user_data_root);
}

void AccentColor::AttachWatcher(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !watcher_) {
    watcher_ = std::make_unique<AccentColorWatcher>(drawer);
  }
}

void AccentColor::RefreshIfChanged() {
  if (!kernel_state_ || !kernel_state_->memory()) {
    return;
  }

  auto* memory = kernel_state_->memory();
  uint32_t r = ReadGuestU32BE(memory, kAccentColorGuestAddress + kRedOffset);
  uint32_t g = ReadGuestU32BE(memory, kAccentColorGuestAddress + kGreenOffset);
  uint32_t b = ReadGuestU32BE(memory, kAccentColorGuestAddress + kBlueOffset);

  bool looks_valid = r <= 15 && g <= 15 && b <= 15;
  bool looks_populated = looks_valid && (r != 0 || g != 0 || b != 0);
  if (!looks_populated) {
    // No save loaded yet this session, so the live struct hasn't been
    // written to -- fall back to the newest save file instead of applying
    // (0, 0, 0), which `rex::ui::ShadeAccent` washes out toward white
    // (zero saturation) for most overlay roles.
    RefreshFromSaveFile();
    return;
  }

  if (has_read_once_ && r == last_r_ && g == last_g_ && b == last_b_) {
    return;
  }

  last_r_ = r;
  last_g_ = g;
  last_b_ = b;
  has_read_once_ = true;
  rex::ui::ApplyAccentTheme(ImGui::GetStyle(), ChannelsToColor(r, g, b));
}

void AccentColor::RefreshFromSaveFile() {
  if (!kernel_state_->user_profile()) {
    return;
  }

  // content_root/xuid/title_id/content_type/<slot name>/<slot name>, matching
  // rex::system::xam::ContentManager's on-disk layout for SavedGame content.
  std::filesystem::path saves_dir =
      user_data_root_ / ToHex(kernel_state_->user_profile()->xuid(), 16) /
      ToHex(kernel_state_->title_id(), 8) /
      ToHex(static_cast<uint32_t>(rex::system::XContentType::kSavedGame), 8);

  std::error_code ec;
  std::filesystem::path newest_path;
  std::filesystem::file_time_type newest_time{};
  if (std::filesystem::is_directory(saves_dir, ec)) {
    // Up to 15 save slots live as sibling directories here; pick whichever
    // was written to most recently as the best guess for "the active save".
    for (const auto& entry : std::filesystem::directory_iterator(saves_dir, ec)) {
      if (!entry.is_directory(ec)) {
        continue;
      }
      std::filesystem::path slot_file = entry.path() / entry.path().filename();
      auto write_time = std::filesystem::last_write_time(slot_file, ec);
      if (ec) {
        continue;
      }
      if (newest_path.empty() || write_time > newest_time) {
        newest_path = slot_file;
        newest_time = write_time;
      }
    }
  }

  if (newest_path.empty()) {
    // No save has ever been written -- nothing to read a real value from.
    if (!has_applied_no_save_fallback_) {
      has_applied_no_save_fallback_ = true;
      rex::ui::ApplyAccentTheme(ImGui::GetStyle(), kNoSaveFallbackColor);
    }
    return;
  }

  if (has_read_save_once_ && newest_time == last_save_write_time_) {
    return;
  }

  std::ifstream file(newest_path, std::ios::binary);
  if (!file.is_open()) {
    return;
  }

  uint32_t r = ReadBigEndianU32(file, kSaveRedOffset);
  uint32_t g = ReadBigEndianU32(file, kSaveGreenOffset);
  uint32_t b = ReadBigEndianU32(file, kSaveBlueOffset);
  if (r > 15 || g > 15 || b > 15) {
    // Doesn't look like a real accent setting (e.g. a save predating this
    // field, or a differently laid-out slot); keep the current theme.
    return;
  }

  last_save_write_time_ = newest_time;
  has_read_save_once_ = true;
  rex::ui::ApplyAccentTheme(ImGui::GetStyle(), ChannelsToColor(r, g, b));
}

AccentColor& GetAccentColor() {
  static AccentColor instance;
  return instance;
}

}  // namespace nocturne
