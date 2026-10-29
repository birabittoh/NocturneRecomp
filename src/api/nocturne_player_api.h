// nocturnerecomp - ReXGlue Recompiled Project
//
// Public C ABI for the player-status struct (the PS1 PlayerStatus equivalent,
// `g_Status`): relics, item counts and order lists, equipment and stats.
//
// This exists so a mod never has to know a single guest address. The layout
// and how each row was confirmed are written up next to the offsets in
// src/game/player_status.cpp.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable:
//
//     auto get = reinterpret_cast<NocturneGetPlayerStatusFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "NocturneGetPlayerStatus"));
//     NocturnePlayerStatus status;
//     if (get && get(&status) == NOCTURNE_PLAYER_OK) { ... }
//
// (dlsym(RTLD_DEFAULT, ...) on Linux and Android.) Always null-check: a mod
// built against a newer host must still load on an older one. Check
// NocturnePlayerAbiVersion() before using anything added after version 1.
//
// Events. Changes are published on the shared mod registry bus
// (rex::system::ModRegistry, reached via runtime->mod_registry()) from a
// per-guest-frame tick, so a mod subscribes by name and needs neither this
// header nor a linked symbol:
//
//     NOCTURNE_PLAYER_EVENT_SAVE_LOADED    "player.save_loaded"
//     NOCTURNE_PLAYER_EVENT_SAVE_UNLOADED  "player.save_unloaded"
//     NOCTURNE_PLAYER_EVENT_RELIC_CHANGED  "player.relic_changed"
//     NOCTURNE_PLAYER_EVENT_ITEM_CHANGED   "player.item_changed"
//
// save_loaded / save_unloaded fire on the edges of
// NocturneIsPlayerStatusAvailable, with an empty payload; a load swaps the
// whole struct, so no relic/item events follow it. relic_changed carries
// `u64` = relic index and `bytes` = {old flags, new flags}; item_changed
// carries `u64` = item id and `bytes` = {old count, new count}. They fire for
// the game's own changes as well as for a mod's, on the frame after the
// change. Subscribers run on the command-processor thread, not the ImGui draw
// thread.
//
// Threading. Every entry point here is safe to call from any thread, including
// the ImGui draw thread. Reads come straight from guest memory and may tear
// against the guest, which is fine for display. Writes are raw stores that
// take effect immediately; they do not go through the game's own give/obtain
// routines (not found yet), so NocturneSetItemCount leaves the inventory order
// list alone and a newly given item may not show up in the menus. The one
// exception is NocturneRequestLevelUp, which has to run guest code: it is
// queued onto the guest thread and applied on its next frame.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define NOCTURNE_PLAYER_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define NOCTURNE_PLAYER_EVENT_SAVE_LOADED "player.save_loaded"
#define NOCTURNE_PLAYER_EVENT_SAVE_UNLOADED "player.save_unloaded"
#define NOCTURNE_PLAYER_EVENT_RELIC_CHANGED "player.relic_changed"
#define NOCTURNE_PLAYER_EVENT_ITEM_CHANGED "player.item_changed"

// Relics. Index order matches the PS1 game (Soul of Bat = 0 .. Eye of Vlad =
// 29). Each relic is one byte of flags.
#define NOCTURNE_RELIC_COUNT 30
#define NOCTURNE_RELIC_FOUND 1u   // the player has it
#define NOCTURNE_RELIC_ACTIVE 2u  // switched on in the relic menu

// Item ids. 0..168 are hand items, 169..258 body items (armor, helmets, capes,
// accessories, in that order). The randomizer's two pseudo-items, 259 and 260,
// have names but are never in the inventory.
#define NOCTURNE_HAND_ITEM_COUNT 169
#define NOCTURNE_BODY_ITEM_COUNT 90
#define NOCTURNE_INVENTORY_ITEM_COUNT 259
#define NOCTURNE_ITEM_ID_COUNT 261
#define NOCTURNE_ITEM_STACK_MAX 99

// NocturneGetItemCategory results.
enum {
  NOCTURNE_ITEM_CATEGORY_HAND = 0,
  NOCTURNE_ITEM_CATEGORY_ARMOR = 1,
  NOCTURNE_ITEM_CATEGORY_HEAD = 2,
  NOCTURNE_ITEM_CATEGORY_CAPE = 3,
  NOCTURNE_ITEM_CATEGORY_ACCESSORY = 4,
  NOCTURNE_ITEM_CATEGORY_SPECIAL = 5  // a pseudo-item, not inventory
};

// Equipment slots, in the order the game stores them. Left/right naming
// follows the equip menu.
enum {
  NOCTURNE_SLOT_RIGHT_HAND = 0,
  NOCTURNE_SLOT_LEFT_HAND = 1,
  NOCTURNE_SLOT_HEAD = 2,
  NOCTURNE_SLOT_BODY = 3,
  NOCTURNE_SLOT_CAPE = 4,
  NOCTURNE_SLOT_ACCESSORY_1 = 5,
  NOCTURNE_SLOT_ACCESSORY_2 = 6
};
#define NOCTURNE_SLOT_COUNT 7

// Results. Everything >= 0 is success.
enum {
  NOCTURNE_PLAYER_OK = 0,
  // Accepted and applied on the guest thread's next frame (level up only).
  NOCTURNE_PLAYER_QUEUED = 1,

  // No save is loaded: the front end, file select, or after Quit to Title.
  // Every read and write refuses in this state.
  NOCTURNE_PLAYER_ERR_UNAVAILABLE = -1,
  // The relic index is outside 0..29.
  NOCTURNE_PLAYER_ERR_INVALID_RELIC = -2,
  // The item id is not an inventory item (outside 0..258).
  NOCTURNE_PLAYER_ERR_INVALID_ITEM = -3,
  // The player is already at the level cap (99).
  NOCTURNE_PLAYER_ERR_MAX_LEVEL = -4,
  NOCTURNE_PLAYER_ERR_INVALID_ARGUMENT = -10
};

#define NOCTURNE_LEVEL_MAX 99
// What the status screen's ROOMS percentage is measured against.
#define NOCTURNE_ROOM_TOTAL 942

// NocturneSetPlayerStat fields, one per NocturnePlayerStats member.
enum {
  NOCTURNE_STAT_HP = 0,
  NOCTURNE_STAT_HP_MAX = 1,
  NOCTURNE_STAT_HEARTS = 2,
  NOCTURNE_STAT_HEARTS_MAX = 3,
  NOCTURNE_STAT_MP = 4,
  NOCTURNE_STAT_MP_MAX = 5,
  NOCTURNE_STAT_STR_BASE = 6,
  NOCTURNE_STAT_CON_BASE = 7,
  NOCTURNE_STAT_INT_BASE = 8,
  NOCTURNE_STAT_LCK_BASE = 9,
  NOCTURNE_STAT_STR = 10,
  NOCTURNE_STAT_CON = 11,
  NOCTURNE_STAT_INT = 12,
  NOCTURNE_STAT_LCK = 13,
  NOCTURNE_STAT_LEVEL = 14,
  NOCTURNE_STAT_EXP = 15,
  NOCTURNE_STAT_GOLD = 16,
  NOCTURNE_STAT_KILLS = 17,
  NOCTURNE_STAT_ROOMS = 18,
  NOCTURNE_STAT_PLAY_HOURS = 19,
  NOCTURNE_STAT_PLAY_MINUTES = 20,
  NOCTURNE_STAT_PLAY_SECONDS = 21,
  NOCTURNE_STAT_PLAY_FRAMES = 22
};
#define NOCTURNE_STAT_COUNT 23

typedef struct NocturnePlayerStats {
  int32_t hp, hp_max;
  int32_t hearts, hearts_max;
  int32_t mp, mp_max;
  // STR/CON/INT/LCK twice: the base value, and the total with equipment
  // bonuses included. The status screen shows them as "base+bonus", e.g.
  // "38+14" for str_base = 38, str = 52.
  int32_t str_base, con_base, int_base, lck_base;
  int32_t str, con, intl, lck;
  int32_t level, exp, gold, kills;
  // Rooms visited (the status screen's ROOMS). Kept outside g_Status, but
  // read and written here like any other stat.
  int32_t rooms;
  int32_t play_hours, play_minutes, play_seconds, play_frames;
  int32_t reserved[8];  // zero-filled; room for later additions
} NocturnePlayerStats;

// One consistent copy of the struct, read in a single pass.
typedef struct NocturnePlayerStatus {
  // NOCTURNE_RELIC_* flags, by relic index.
  uint8_t relics[NOCTURNE_RELIC_COUNT];
  // Eight bytes at +0x1E that only the Xbox release has. Purpose unknown;
  // exposed for reverse engineering.
  uint8_t unknown_1e[8];

  // How many of each item the player holds, by hand index (item id) and body
  // index (item id - 169).
  uint8_t hand_count[NOCTURNE_HAND_ITEM_COUNT];
  uint8_t body_count[NOCTURNE_BODY_ITEM_COUNT];
  // Where each item sits in its inventory menu list, same indexing.
  uint8_t hand_order[NOCTURNE_HAND_ITEM_COUNT];
  uint8_t body_order[NOCTURNE_BODY_ITEM_COUNT];

  // The item id in each NOCTURNE_SLOT_*.
  int32_t equipped[NOCTURNE_SLOT_COUNT];
  // The ten dwords the slots are stored among, raw (from +0x294; the slots
  // are [2..8], hands as hand ids, the rest as body indices).
  int32_t equip_raw[10];

  NocturnePlayerStats stats;

  int32_t reserved[16];  // zero-filled; room for later additions
} NocturnePlayerStatus;

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*NocturnePlayerAbiVersionFn)(void);

// 1 while a save (or new game) is loaded and the struct is meaningful, 0 in the
// front end, at file select and after Quit to Title.
typedef int (*NocturneIsPlayerStatusAvailableFn)(void);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Fills `out` with the whole struct. Returns NOCTURNE_PLAYER_OK or a negative
// error (`out` is zero-filled then).
typedef int (*NocturneGetPlayerStatusFn)(NocturnePlayerStatus* out);

// A relic's flags (NOCTURNE_RELIC_*), or a negative error.
typedef int (*NocturneGetRelicFlagsFn)(int relic);

// How many of `item_id` the player holds, or a negative error.
typedef int (*NocturneGetItemCountFn)(int item_id);

// ---------------------------------------------------------------------------
// Names and categories (static, work with no save loaded)
// ---------------------------------------------------------------------------

// The English names. Never null: an index out of range answers "?". The
// returned pointer stays valid for the life of the process.
typedef const char* (*NocturneGetRelicNameFn)(int relic);
typedef const char* (*NocturneGetItemNameFn)(int item_id);

// One of the NOCTURNE_ITEM_CATEGORY_* values, or a negative error for an id
// outside 0..260.
typedef int (*NocturneGetItemCategoryFn)(int item_id);

// ---------------------------------------------------------------------------
// Writing (raw; see the threading note at the top)
// ---------------------------------------------------------------------------

// Sets a relic's flags (only NOCTURNE_RELIC_FOUND | NOCTURNE_RELIC_ACTIVE are
// kept). Returns NOCTURNE_PLAYER_OK or a negative error.
typedef int (*NocturneSetRelicFlagsFn)(int relic, int flags);

// Sets how many of `item_id` the player holds, 0..NOCTURNE_ITEM_STACK_MAX.
// Returns NOCTURNE_PLAYER_OK or a negative error.
typedef int (*NocturneSetItemCountFn)(int item_id, int count);

// Sets one NOCTURNE_STAT_* field to `value` (>= 0). Nothing is recomputed:
// raising hp_max leaves hp alone, setting exp does not level up, and so on.
// Returns NOCTURNE_PLAYER_OK or a negative error.
typedef int (*NocturneSetPlayerStatFn)(int stat, int32_t value);

// ---------------------------------------------------------------------------
// Level up
// ---------------------------------------------------------------------------

// Performs one real level up through the game's own routine, the same as
// earning the exp for it: HP/MP max and a random subset of the four stats
// grow. Exp is raised to the next level's threshold first if it is short.
// Returns NOCTURNE_PLAYER_QUEUED, or a negative error decided up front
// (UNAVAILABLE, MAX_LEVEL).
typedef int (*NocturneRequestLevelUpFn)(void);

// ---------------------------------------------------------------------------
// Game clear flag
// ---------------------------------------------------------------------------

// Whether any save slot has cleared the game, which together with the name
// "richter " unlocks Richter mode. It is the OR of every slot's own flag,
// rebuilt from the slots when the save list loads, so it is readable at the
// title screen too and a change may not survive the next rebuild. Returns 1, 0, or
// NOCTURNE_PLAYER_ERR_UNAVAILABLE before the game has mapped it.
typedef int (*NocturneGetGameClearedFn)(void);
typedef int (*NocturneSetGameClearedFn)(int cleared);

#ifdef __cplusplus
}  // extern "C"
#endif
