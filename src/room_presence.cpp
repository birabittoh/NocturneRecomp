#include "room_presence.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include <rex/discord_rpc.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>

namespace nocturne {

namespace {

// Discord Application ID (discord.com/developers/applications, "NocturneRecomp").
constexpr char kDiscordClientId[] = "1528401288918728744";

// Guest virtual address of the live "current room index" (dword_83133AEC in
// assets/default.xex): the index the game's own room-load state machine
// (sub_82250D10) uses to look up everything about the active room, including
// the 3/4-letter room code below. Found by locating that state machine via
// its "load:c:\bin\%s.bin\n" debug format string and tracing back to the
// index variable driving its switch statement. Vanilla only -- unlike the
// player-stats struct below, this hasn't been re-derived for a TU build, so
// the room half of the presence is simply skipped under NOCTURNE_TU.
constexpr uint32_t kRoomIndexGuestAddress = 0x83133AEC;

// Base of the game's own room table: an array of fixed-size (44-byte)
// records, one per room index, with a pointer to the room's 3/4-letter code
// string (e.g. "NO0", "ARE") at offset 0. Same table the state machine above
// reads via `off_82E00D08 + 11 * v6` (11 dwords == 44 bytes) to build the
// "c:\bin\<code>.bin" load path.
constexpr uint32_t kRoomTableGuestAddress = 0x82E00D08;
constexpr uint32_t kRoomTableStride = 44;
// Highest known-populated index in the table (id 56, "RBO0"); anything past
// this is out of bounds for the vanilla image and skipped rather than read.
constexpr uint32_t kRoomTableMaxIndex = 56;

// Guest address of the live player-stats struct (character screen: HP/Heart/
// MP, STR/CON/INT/LCK, level, exp, gold, kills, playtime), and its offsets --
// both taken from ../NocturneRecomp-Mods/src/game_symbols/mod_main.cpp
// ("player.stats"), which is where these were originally reverse-engineered
// (exact-pattern scan for the STR/CON/INT/LCK sequence shown consecutively
// on the character screen). The TU address was independently re-derived
// there the same way and cross-checked against the vanilla/TU delta shared
// with kAccentAddrVanilla/TU in that file (0x240) -- not just guessed.
#ifdef NOCTURNE_TU
constexpr uint32_t kPlayerStatsGuestAddress = 0x8317493Cu;
#else
constexpr uint32_t kPlayerStatsGuestAddress = 0x83174B7Cu;
#endif
constexpr uint32_t kHpCurrentOffset = 0x00;
constexpr uint32_t kHpMaxOffset = 0x04;
constexpr uint32_t kLevelOffset = 0x48;
// Covers every player-stats field read here, for the heap-readability check
// below.
constexpr uint32_t kPlayerStatsSpanSize = kLevelOffset + 4;

// Guest address of the real rooms-visited counter -- a separate, standalone
// address, not part of the player-stats struct above.
//
// Both addresses were confirmed live via scripts/re/scan_guest_memory.py's
// value-transition technique: scanned the whole process for every host
// address holding 57 (the on-screen ROOMS value), asked the user to walk
// into one new room (ROOMS -> 58), rescanned, and intersected the two sets.
// Each build collapsed to exactly 2 candidates -- the address below plus its
// usual +0x10000000 heap-alias mirror -- both confirmed reading 58
// afterward.
//
// The vanilla address is 0x83164F10, NOT the 0x83164CD0 documented as
// "player.rooms" in ../NocturneRecomp-Mods/src/game_symbols/mod_main.cpp --
// that address reads as a stale 0 against the current vanilla build/save.
// Re-running the same scan against a NOCTURNE_TU build found the ROOMS
// counter at exactly 0x83164CD0 -- i.e. game_symbols' "vanilla" address was
// actually the TU one, and the delta between it and the real vanilla address
// above (0x240) matches the vanilla/TU delta used everywhere else in this
// file, confirming the pairing.
#ifdef NOCTURNE_TU
constexpr uint32_t kRoomsGuestAddress = 0x83164CD0u;
#else
constexpr uint32_t kRoomsGuestAddress = 0x83164F10u;
#endif

uint32_t ReadGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address) {
  const uint8_t* host_address = memory->TranslateVirtual<const uint8_t*>(guest_address);
  return rex::memory::load_and_swap<uint32_t>(host_address);
}

// Room codes are short, null-terminated ASCII literals baked into the XEX's
// static data (see kRoomTableGuestAddress above) -- never longer than 8
// bytes ("RBO0" + reverse-castle "R" prefix variants top out around there).
std::string ReadGuestCString(rex::memory::Memory* memory, uint32_t guest_address, size_t max_len) {
  const char* host_address = memory->TranslateVirtual<const char*>(guest_address);
  size_t len = strnlen(host_address, max_len);
  return std::string(host_address, len);
}

// Friendly names for every room code, from sotn_room_ids.md (sourced from the
// sotn-decomp project's file listing, cross-checked with Castlevania Wiki).
// Reverse Castle rooms have their own distinct names here (e.g. RCHI ==
// "Cave", not "Abandoned Mine (Reverse Castle)") rather than being derived
// from their normal-castle counterpart.
const std::unordered_map<std::string, const char*>& RoomNameTable() {
  static const std::unordered_map<std::string, const char*> table = {
      {"ARE", "Colosseum"},
      {"CAT", "Catacombs"},
      {"CEN", "Center Cube"},
      {"CHI", "Abandoned Mine"},
      {"DAI", "Royal Chapel"},
      {"DRE", "Nightmare"},
      {"LIB", "Long Library"},
      {"MAD", "Debug Room"},
      {"NO0", "Marble Gallery"},
      {"NO1", "Outer Wall"},
      {"NO2", "Olrox's Quarters"},
      {"NO3", "Castle Entrance"},
      {"NO4", "Underground Caverns"},
      {"NP3", "Castle Entrance"},
      {"NZ0", "Alchemy Laboratory"},
      {"NZ1", "Clock Tower"},
      {"TOP", "Castle Keep"},
      {"WRP", "Warp Room"},
      {"TE1", "Test Room 1"},
      {"TE2", "Test Room 2"},
      {"TE3", "Test Room 3"},
      {"TE4", "Test Room 4"},
      {"TE5", "Test Room 5"},
      {"RARE", "Reverse Colosseum"},
      {"RCAT", "Floating Catacombs"},
      {"RCEN", "Reverse Center Cube"},
      {"RCHI", "Cave"},
      {"RDAI", "Anti-Chapel"},
      {"RLIB", "Forbidden Library"},
      {"RNO0", "Black Marble Gallery"},
      {"RNO1", "Reverse Outer Wall"},
      {"RNO2", "Death Wing's Lair"},
      {"RNO3", "Reverse Entrance"},
      {"RNO4", "Reverse Caverns"},
      {"RNZ0", "Necromancy Laboratory"},
      {"RNZ1", "Reverse Clock Tower"},
      {"RTOP", "Reverse Keep"},
      {"RWRP", "Warp Room"},
      {"BO0", "Boss: Olrox"},
      {"BO1", "Boss: Granfaloon"},
      {"BO2", "Boss: Minotaur & Werewolf"},
      {"BO3", "Boss: Scylla"},
      {"BO4", "Boss: Doppleganger 10"},
      {"BO5", "Boss: Hippogryph"},
      {"MAR", "Maria's Room"},
      {"SEL", "Title Screen"},
      {"ST0", "Intro (Bloodlines)"},
  };
  return table;
}

std::string RoomCodeToName(const std::string& code) {
  const auto& table = RoomNameTable();
  auto it = table.find(code);
  // Unrecognized/unused code (padding slot, etc.) -- fall back to the raw
  // code rather than showing nothing.
  return it != table.end() ? it->second : code;
}

}  // namespace

void RoomPresence::Bind(rex::system::KernelState* kernel_state, rex::Runtime* runtime) {
  kernel_state_ = kernel_state;

  rex::discord_rpc::Presence initial;
  initial.details_ = "Exploring Dracula's Castle";
  initial.large_image_key_ = "icon";
  initial.large_image_text_ = "NocturneRecomp";
  rex::discord_rpc::Start(kDiscordClientId, initial);

  runtime->mod_registry()->RegisterTick([this] { Tick(); });
}

void RoomPresence::Tick() {
  if (!kernel_state_ || !kernel_state_->memory()) {
    return;
  }
  auto* memory = kernel_state_->memory();

  // The player-stats struct lives on the guest heap and doesn't exist until
  // a save is loaded (main menu / no file started yet this session), so --
  // unlike the static-data room table below -- check it's actually mapped
  // before dereferencing it (same guard mods_src/player_stats uses for the
  // same struct). Even once mapped, it reads as all-zero (hp_max == 0)
  // until a save actually loads: hp_max is never a real 0 in play (you'd be
  // dead), so it doubles as the "is a session actually running" check --
  // the room-index address below reads a stale/default 0 at the same time,
  // which happens to alias a real room code ("NO0"), so without this check
  // the presence would misleadingly claim to be in Marble Gallery at the
  // main menu.
  auto* heap = memory->LookupHeap(kPlayerStatsGuestAddress);
  bool stats_mapped = heap && heap->QueryRangeAccess(kPlayerStatsGuestAddress,
                                                      kPlayerStatsGuestAddress + kPlayerStatsSpanSize - 1) !=
                                   rex::memory::PageAccess::kNoAccess;
  uint32_t hp_max =
      stats_mapped ? ReadGuestU32BE(memory, kPlayerStatsGuestAddress + kHpMaxOffset) : 0;
  bool in_game = stats_mapped && hp_max > 0;

  if (!in_game) {
    if (!showed_main_menu_) {
      showed_main_menu_ = true;
      has_read_room_once_ = false;
      has_read_stats_once_ = false;
      rex::discord_rpc::SetDetails("Main Menu");
      rex::discord_rpc::SetState("");
    }
    return;
  }
  showed_main_menu_ = false;

  // Room (top line, "details"). kRoomIndexGuestAddress/kRoomTableGuestAddress
  // are vanilla-only (not yet re-derived for a TU image, unlike the player-
  // stats struct below) -- skip rather than risk reading whatever else lives
  // at that address in a TU build's relocated .data.
#ifndef NOCTURNE_TU
  uint32_t room_id = ReadGuestU32BE(memory, kRoomIndexGuestAddress);
  if (room_id <= kRoomTableMaxIndex && (!has_read_room_once_ || room_id != last_room_id_)) {
    uint32_t code_ptr = ReadGuestU32BE(memory, kRoomTableGuestAddress + room_id * kRoomTableStride);
    if (code_ptr != 0) {
      std::string code = ReadGuestCString(memory, code_ptr, 8);
      if (!code.empty()) {
        last_room_id_ = room_id;
        has_read_room_once_ = true;
        rex::discord_rpc::SetDetails(RoomCodeToName(code));
      }
    }
  }
#endif

  // HP/level/rooms (second line, "state").
  uint32_t hp_current = ReadGuestU32BE(memory, kPlayerStatsGuestAddress + kHpCurrentOffset);
  uint32_t level = ReadGuestU32BE(memory, kPlayerStatsGuestAddress + kLevelOffset);

  // "player.rooms" is a separate address from the player-stats struct above
  // (see game_symbols/mod_main.cpp), so it gets its own mapped check.
  //
  // In practice this counter reads 0 right after loading a save (rooms==0
  // was observed here on a fresh start showing Marble Gallery) until it
  // catches up, typically after the first room transition. A real save
  // always has at least the starting room counted, so 0 is never a genuine
  // value -- treat it the same way hp_max==0 above is treated, as "not
  // populated yet", and simply omit the completion percentage from the
  // state string until then.
  bool rooms_readable = false;
  uint32_t rooms = 0;
  auto* rooms_heap = memory->LookupHeap(kRoomsGuestAddress);
  bool rooms_mapped = rooms_heap && rooms_heap->QueryRangeAccess(kRoomsGuestAddress, kRoomsGuestAddress + 3) !=
                                         rex::memory::PageAccess::kNoAccess;
  if (rooms_mapped) {
    uint32_t rooms_value = ReadGuestU32BE(memory, kRoomsGuestAddress);
    if (rooms_value > 0) {
      rooms_readable = true;
      rooms = rooms_value;
    }
  }

  if (has_read_stats_once_ && hp_current == last_hp_current_ && hp_max == last_hp_max_ &&
      level == last_level_ && rooms == last_rooms_) {
    return;
  }
  last_hp_current_ = hp_current;
  last_hp_max_ = hp_max;
  last_level_ = level;
  last_rooms_ = rooms;
  has_read_stats_once_ = true;

  char state[128];
  if (rooms_readable) {
    // Same completion formula the game's own 200.6%-style tally is built
    // from: rooms visited out of the 942 total.
    double completion_percent = rooms * 100.0 / 942.0;
    std::snprintf(state, sizeof(state), "HP %u/%u - Lv %u - %.1f%%", hp_current, hp_max, level,
                  completion_percent);
  } else {
    std::snprintf(state, sizeof(state), "HP %u/%u - Lv %u", hp_current, hp_max, level);
  }
  rex::discord_rpc::SetState(state);
}

RoomPresence& GetRoomPresence() {
  static RoomPresence instance;
  return instance;
}

}  // namespace nocturne
