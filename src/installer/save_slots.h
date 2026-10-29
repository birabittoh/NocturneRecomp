#pragma once

// The game's saves on disk, for the start screen's Saves page, which runs
// before a Runtime exists.
//
// Each save is a raw PS1 memory card block (8 KB: "SC" magic, a Shift-JIS
// title such as "CASTLEVANIA-01 ALUCARD 020%", a 16x16 4bpp icon, the game
// data) stored as the content file BASLUS-00067DRAX<n>, with an XContent
// .header beside it. The game writes a new save to the first free <n> below
// 30 (sub_822538E0); the page shows all kSaveSlotCount of them. The
// block's info (sub_82253BF0) follows the PS1's SaveInfo from +0x100.
//
// Only Xbox 360 saves are imported: our export zip, or a bare block with its
// .header next to it (one is made when it is missing).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nocturne {

inline constexpr int kSaveSlotCount = 30;
// The PS1 memory card icon.
inline constexpr int kSaveThumbWidth = 16;
inline constexpr int kSaveThumbHeight = 16;

struct SaveSlotInfo {
  bool used = false;
  // The player's name from the block's title, and the map completion ("200.6%").
  std::string name;
  std::string completion;
  // Local time of the last write, "YYYY/MM/DD HH:MM".
  std::string modified;
  // Played in a randomizer seed (the game's rando_slots record), and its seed name.
  bool randomized = false;
  std::string seed;
  // The icon's frames, RGBA kSaveThumbWidth x kSaveThumbHeight each; empty when
  // unreadable. More than one when the game's icon set is cached.
  std::vector<std::vector<uint8_t>> frames;
  // How long each frame shows.
  float frame_seconds = 0.0f;
};

// The block keeps only the last frame of its icon; the others are in the game's
// image. The game calls this once it is mapped, with a reader of its memory, so
// the start screen can animate the icons on the next launch.
void CacheSaveIcons(const std::filesystem::path& user_data_root,
                    const std::function<const uint8_t*(uint32_t)>& guest);

// A save read for a slot, held until it is written.
struct PreparedSave {
  std::vector<uint8_t> block;
  // The XContent header that came with it; empty to make one.
  std::vector<uint8_t> header;
};

std::vector<SaveSlotInfo> ReadSaveSlots(const std::filesystem::path& user_data_root);

// Each returns an error message, empty on success.
std::string ExportSaveSlot(const std::filesystem::path& user_data_root, int slot,
                           const std::filesystem::path& archive);
std::string PrepareSaveImport(const std::filesystem::path& user_data_root,
                              const std::filesystem::path& source, PreparedSave& out);
std::string WriteSaveSlot(const std::filesystem::path& user_data_root, int slot,
                          const PreparedSave& save);
std::string DeleteSaveSlot(const std::filesystem::path& user_data_root, int slot);

// "nocturnerecomp-save-01.zip" for slot 0.
std::string SaveSlotExportName(int slot);

}  // namespace nocturne
