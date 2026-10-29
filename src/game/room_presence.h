// nocturnerecomp - Discord Rich Presence: show the castle location the player
// is currently in, plus HP, level and rooms visited.
//
// Reads the guest's current-room index and the player-stats struct straight
// out of live guest memory once per guest frame and translates the room index
// through the game's own room-code table (still in guest memory) and a
// static code->friendly-name map (see room_presence.cpp for how the
// addresses and code table were found, and where the friendly names come
// from). Pushes changes to rex::discord_rpc's SetDetails/SetState so the
// SDK's own worker thread does the actual Discord IPC.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
namespace system {
class KernelState;
}  // namespace system
}  // namespace rex

namespace nocturne {

class RoomPresence {
 public:
  RoomPresence() = default;

  // Starts Discord RPC (rex::discord_rpc::Start) and registers a per-guest-
  // frame tick that keeps the presence's details/state lines in sync with
  // the player's current room, HP, level and rooms visited. Call once
  // KernelState and the runtime are both live (OnPostSetup).
  void Bind(rex::system::KernelState* kernel_state, rex::Runtime* runtime);

  // Re-reads the current room index and player stats and, if either
  // changed, updates the Discord presence. Registered as a guest-frame tick
  // by Bind(); safe to call before Bind() (no-op until bound).
  void Tick();

 private:
  rex::system::KernelState* kernel_state_ = nullptr;

  uint32_t last_room_id_ = 0xFFFFFFFFu;
  bool has_read_room_once_ = false;

  uint32_t last_hp_current_ = 0xFFFFFFFFu;
  uint32_t last_hp_max_ = 0xFFFFFFFFu;
  uint32_t last_level_ = 0xFFFFFFFFu;
  uint32_t last_rooms_ = 0xFFFFFFFFu;
  bool has_read_stats_once_ = false;

  // Tracks whether the last update showed "Main Menu" (no save loaded yet
  // this session) so that state is only pushed once per transition instead
  // of every tick.
  bool showed_main_menu_ = false;
};

// Process-wide instance shared between the app hooks.
RoomPresence& GetRoomPresence();

}  // namespace nocturne
