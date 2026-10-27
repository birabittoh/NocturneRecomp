#include "player_status.h"

#include <atomic>
#include <cstring>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>

#include "api/nocturne_player_api.h"
#include "game_state.h"
#include "player_status_names.h"

namespace nocturne::player {

namespace {

// Guest address of g_Status, the PS1 PlayerStatus struct, in the .data zero-fill
// area. Found by scanning for lis/addi pairs resolving to it (119 uses).
constexpr uint32_t kStatusAddress = 0x83174940u;

// Layout, offsets from kStatusAddress. "confirmed" = seen in code or live;
// relics, item counts, order lists and equipment were verified live in the item_inspector mod.
constexpr uint32_t kRelicsOffset = 0x000;       // u8[30], confirmed (wolf dash gates on relics[6] & 2)
constexpr uint32_t kUnknown1EOffset = 0x01E;    // u8[8], XB-only, unknown
constexpr uint32_t kHandCountOffset = 0x026;    // u8[169], confirmed live
constexpr uint32_t kBodyCountOffset = 0x0CF;    // u8[90], confirmed live
constexpr uint32_t kHandOrderOffset = 0x129;    // u8[169], confirmed live
constexpr uint32_t kBodyOrderOffset = 0x1D2;    // u8[90], confirmed live
constexpr uint32_t kStatsOffset = 0x23C;        // BE s32 fields, confirmed live (game_symbols)
constexpr uint32_t kEquipRawOffset = 0x294;     // BE s32[10]; the 7 slots start at +0x29C (confirmed live)
constexpr uint32_t kReadSpan = 0x2E0;           // through the play-time words (+0x2CC..+0x2DB)

// Offsets inside the stats block.
constexpr uint32_t kStatHp = 0x00, kStatHpMax = 0x04, kStatHearts = 0x08, kStatHeartsMax = 0x0C;
constexpr uint32_t kStatMp = 0x10, kStatMpMax = 0x14;
// STR/CON/INT/LCK twice, confirmed live with a +14 STR item equipped: the
// status screen's "38+14" read 38 at +0x18 and 52 at +0x38.
constexpr uint32_t kStatStrBase = 0x18;  // base
constexpr uint32_t kStatStr = 0x38;      // base + equipment bonuses
constexpr uint32_t kStatLevel = 0x48, kStatExp = 0x4C, kStatGold = 0x50, kStatKills = 0x54;
constexpr uint32_t kStatPlay = 0x90;

// Rooms visited (the status screen's ROOMS), a lone BE u32 outside g_Status. Found by a
// rescan across room transitions (see NocturneRecomp-Mods' game_symbols); room_presence.cpp
// reads it too.
constexpr uint32_t kRoomsAddress = 0x83164F10u;

// Game-clear flag, BE u32: the OR of every save slot's clear flag (slot offset 0x124),
// reset by sub_8238FC38 and rebuilt by sub_8238FAC8 when the save list loads. Non-zero plus
// the name "richter " unlocks Richter mode (sub_82395868).
constexpr uint32_t kGameClearFlagAddress = 0x83133AFCu;

// NOCTURNE_STAT_* -> offset inside the stats block, in enum order. kStatRooms marks the one
// field that lives at kRoomsAddress instead.
constexpr uint32_t kStatRooms = ~0u;
constexpr uint32_t kStatFieldOffsets[] = {
    0x00, 0x04, 0x08, 0x0C, 0x10, 0x14,  // hp, hp_max, hearts, hearts_max, mp, mp_max
    0x18, 0x1C, 0x20, 0x24,              // str/con/int/lck, base
    0x38, 0x3C, 0x40, 0x44,              // str/con/int/lck, with equipment
    0x48, 0x4C, 0x50, 0x54,              // level, exp, gold, kills
    kStatRooms,
    0x90, 0x94, 0x98, 0x9C,  // play time h/m/s/frames
};
static_assert(sizeof(kStatFieldOffsets) / sizeof(kStatFieldOffsets[0]) == NOCTURNE_STAT_COUNT);

// Level up, driven by the "player.request_level_up" event (see ApplyRequestedLevelUp).
// The game's own level-up tick (sub_82256CC0) rolls HP/MP max and a random subset of the
// four stats, like an exp-triggered level-up. The table holds the exp needed per level (BE u32,
// indexed by current level).
constexpr uint32_t kLevelUpTickFnAddress = 0x82256CC0u;
constexpr uint32_t kExpToLevelTableAddress = 0x82E440E4u;
constexpr int32_t kMaxLevel = 99;
std::atomic<bool> g_level_up_requested{false};

constexpr uint32_t kBodyFirstHead = 26, kBodyFirstCape = 48, kBodyFirstAccessory = 57;

int32_t BE32(const uint8_t* p) { return static_cast<int32_t>(rex::memory::load_and_swap<uint32_t>(p)); }

// Returns the host pointer for `size` bytes at `address`, or null if they aren't mapped.
uint8_t* GuestHostPtr(uint32_t address, uint32_t size) {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) {
    return nullptr;
  }
  auto* memory = ks->memory();
  auto* heap = memory->LookupHeap(address);
  if (!heap || heap->QueryRangeAccess(address, address + size - 1) ==
                   rex::memory::PageAccess::kNoAccess) {
    return nullptr;
  }
  return memory->TranslateVirtual<uint8_t*>(address);
}

// Returns the host pointer for the whole struct, or null if it isn't mapped.
uint8_t* StatusHostPtr(uint32_t size) { return GuestHostPtr(kStatusAddress, size); }

}  // namespace

ItemCategory ItemId::Category() const {
  if (IsHand()) {
    return ItemCategory::Hand;
  }
  if (!IsBody()) {
    return ItemCategory::Special;
  }
  int i = Index();
  if (i < static_cast<int>(kBodyFirstHead)) return ItemCategory::Armor;
  if (i < static_cast<int>(kBodyFirstCape)) return ItemCategory::Head;
  if (i < static_cast<int>(kBodyFirstAccessory)) return ItemCategory::Cape;
  return ItemCategory::Accessory;
}

std::string_view ItemId::Name() const {
  return value < kItemIdCount ? kItemNames[value] : std::string_view("?");
}

std::string_view RelicName(Relic relic) {
  int i = static_cast<int>(relic);
  return i >= 0 && i < kRelicCount ? kRelicNames[i] : std::string_view("?");
}

int Snapshot::ItemCount(ItemId id) const {
  if (id.IsHand()) return hand_count[id.Index()];
  if (id.IsBody()) return body_count[id.Index()];
  return 0;
}

void PlayerStatus::Bind(rex::Runtime* runtime) {
  runtime_ = runtime;
  runtime->mod_registry()->RegisterAddress("player.status", kStatusAddress);
  runtime->mod_registry()->RegisterAddress("app.state", kAppObjectPtrAddr + 4);
  runtime->mod_registry()->RegisterTick([this] { Tick(); });
  runtime->mod_registry()->Subscribe(
      "player.request_level_up",
      [](const rex::system::ModRegistry::EventPayload&) { RequestLevelUp(); });
}

bool PlayerStatus::Read(Snapshot& out) const {
  out = Snapshot{};
  const uint8_t* p = StatusHostPtr(kReadSpan);
  if (!p) {
    return false;
  }
  // The struct is all-zero until a save (or new game) loads, and hp_max is
  // never a real 0 in play. It isn't cleared on Quit to Title, though, only
  // when the game starts again, so the game state has to be checked too.
  const uint8_t* stats = p + kStatsOffset;
  if (BE32(stats + kStatHpMax) <= 0 ||
      !GameRunning(rex::system::kernel_state()->memory()->virtual_membase())) {
    return false;
  }
  out.loaded = true;
  std::memcpy(out.relics.data(), p + kRelicsOffset, kRelicCount);
  std::memcpy(out.unknown_1e.data(), p + kUnknown1EOffset, 8);
  std::memcpy(out.hand_count.data(), p + kHandCountOffset, kHandItemCount);
  std::memcpy(out.body_count.data(), p + kBodyCountOffset, kBodyItemCount);
  std::memcpy(out.hand_order.data(), p + kHandOrderOffset, kHandItemCount);
  std::memcpy(out.body_order.data(), p + kBodyOrderOffset, kBodyItemCount);
  for (int i = 0; i < 10; ++i) {
    out.equip_raw[i] = BE32(p + kEquipRawOffset + i * 4);
  }
  Stats& s = out.stats;
  s.hp = BE32(stats + kStatHp);
  s.hp_max = BE32(stats + kStatHpMax);
  s.hearts = BE32(stats + kStatHearts);
  s.hearts_max = BE32(stats + kStatHeartsMax);
  s.mp = BE32(stats + kStatMp);
  s.mp_max = BE32(stats + kStatMpMax);
  s.str_base = BE32(stats + kStatStrBase);
  s.con_base = BE32(stats + kStatStrBase + 4);
  s.int_base = BE32(stats + kStatStrBase + 8);
  s.lck_base = BE32(stats + kStatStrBase + 12);
  s.str = BE32(stats + kStatStr);
  s.con = BE32(stats + kStatStr + 4);
  s.intl = BE32(stats + kStatStr + 8);
  s.lck = BE32(stats + kStatStr + 12);
  s.level = BE32(stats + kStatLevel);
  s.exp = BE32(stats + kStatExp);
  s.gold = BE32(stats + kStatGold);
  s.kills = BE32(stats + kStatKills);
  s.play_hours = BE32(stats + kStatPlay);
  s.play_minutes = BE32(stats + kStatPlay + 4);
  s.play_seconds = BE32(stats + kStatPlay + 8);
  s.play_frames = BE32(stats + kStatPlay + 12);
  if (const uint8_t* rooms = GuestHostPtr(kRoomsAddress, 4)) {
    s.rooms = BE32(rooms);
  }
  return true;
}

bool PlayerStatus::SetRelic(Relic relic, uint8_t flags) {
  uint8_t* p = StatusHostPtr(kReadSpan);
  if (!p || static_cast<int>(relic) >= kRelicCount) {
    return false;
  }
  p[kRelicsOffset + static_cast<int>(relic)] = flags & 3;
  return true;
}

bool PlayerStatus::SetItemCount(ItemId id, uint8_t count) {
  uint8_t* p = StatusHostPtr(kReadSpan);
  if (!p || !id.IsInventory()) {
    return false;
  }
  p[(id.IsHand() ? kHandCountOffset : kBodyCountOffset) + id.Index()] = count;
  return true;
}

bool PlayerStatus::SetStat(int stat, int32_t value) {
  if (stat < 0 || stat >= NOCTURNE_STAT_COUNT) {
    return false;
  }
  uint32_t offset = kStatFieldOffsets[stat];
  uint8_t* p = nullptr;
  if (offset == kStatRooms) {
    p = GuestHostPtr(kRoomsAddress, 4);
  } else if (uint8_t* status = StatusHostPtr(kReadSpan)) {
    p = status + kStatsOffset + offset;
  }
  if (!p) {
    return false;
  }
  rex::memory::store_and_swap<uint32_t>(p, static_cast<uint32_t>(value));
  return true;
}

void PlayerStatus::Tick() {
  if (!runtime_) {
    return;
  }
  auto* registry = runtime_->mod_registry();
  using Payload = rex::system::ModRegistry::EventPayload;

  Snapshot now;
  Read(now);

  if (now.loaded != last_.loaded) {
    // Load/unload swaps the whole struct: publish the edge, not per-item diffs.
    registry->Publish(now.loaded ? "player.save_loaded" : "player.save_unloaded", Payload{});
    last_ = now;
    return;
  }
  if (!now.loaded) {
    return;
  }

  for (int i = 0; i < kRelicCount; ++i) {
    if (now.relics[i] != last_.relics[i]) {
      const uint8_t change[2] = {last_.relics[i], now.relics[i]};
      Payload payload;
      payload.u64 = static_cast<uint64_t>(i);
      payload.bytes = change;
      registry->Publish("player.relic_changed", payload);
    }
  }
  auto diff_items = [&](const uint8_t* a, const uint8_t* b, int n, int base) {
    for (int i = 0; i < n; ++i) {
      if (a[i] != b[i]) {
        const uint8_t change[2] = {b[i], a[i]};
        Payload payload;
        payload.u64 = static_cast<uint64_t>(base + i);
        payload.bytes = change;
        registry->Publish("player.item_changed", payload);
      }
    }
  };
  diff_items(now.hand_count.data(), last_.hand_count.data(), kHandItemCount, 0);
  diff_items(now.body_count.data(), last_.body_count.data(), kBodyItemCount, kHandItemCount);
  last_ = now;
}

void RequestLevelUp() { g_level_up_requested.store(true, std::memory_order_release); }

int GameCleared() {
  const uint8_t* p = GuestHostPtr(kGameClearFlagAddress, 4);
  return p ? (BE32(p) != 0 ? 1 : 0) : -1;
}

bool SetGameCleared(bool cleared) {
  uint8_t* p = GuestHostPtr(kGameClearFlagAddress, 4);
  if (!p) {
    return false;
  }
  rex::memory::store_and_swap<uint32_t>(p, cleared ? 1u : 0u);
  return true;
}

void ApplyRequestedLevelUp(PPCContext& ctx, uint8_t* base) {
  if (!g_level_up_requested.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (!GameRunning(base)) {
    return;
  }
  uint8_t* stats = base + kStatusAddress + kStatsOffset;
  int32_t level = BE32(stats + kStatLevel);
  if (level < 1 || level >= kMaxLevel) {
    REXLOG_INFO("Level Up requested at level {}; ignoring", level);
    return;
  }
  uint32_t needed = rex::memory::load_and_swap<uint32_t>(base + kExpToLevelTableAddress + level * 4);
  if (static_cast<uint32_t>(BE32(stats + kStatExp)) < needed) {
    rex::memory::store_and_swap<uint32_t>(stats + kStatExp, needed);
  }
  auto* fn = rex::system::kernel_state()->function_dispatcher()->GetFunction(kLevelUpTickFnAddress);
  if (!fn) {
    return;
  }
  // An unplanned nested call the caller's code didn't account for: keep its registers.
  PPCContext saved = ctx;
  fn(ctx, base);
  ctx = saved;
  REXLOG_INFO("Level Up: {} -> {}", level, BE32(stats + kStatLevel));
}

PlayerStatus& GetPlayerStatus() {
  static PlayerStatus instance;
  return instance;
}

}  // namespace nocturne::player

// ---------------------------------------------------------------------------
// Public C ABI (api/nocturne_player_api.h)
// ---------------------------------------------------------------------------

namespace {

using nocturne::player::GetPlayerStatus;
using nocturne::player::ItemId;
using nocturne::player::Relic;
using nocturne::player::Snapshot;

bool ValidRelic(int relic) { return relic >= 0 && relic < nocturne::player::kRelicCount; }

bool ValidInventoryItem(int item_id) {
  return item_id >= 0 && ItemId{static_cast<uint16_t>(item_id)}.IsInventory();
}

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t NocturnePlayerAbiVersion(void) {
  return NOCTURNE_PLAYER_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneIsPlayerStatusAvailable(void) {
  Snapshot snap;
  return GetPlayerStatus().Read(snap) ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneGetPlayerStatus(NocturnePlayerStatus* out) {
  if (!out) {
    return NOCTURNE_PLAYER_ERR_INVALID_ARGUMENT;
  }
  *out = NocturnePlayerStatus{};
  Snapshot snap;
  if (!GetPlayerStatus().Read(snap)) {
    return NOCTURNE_PLAYER_ERR_UNAVAILABLE;
  }
  std::memcpy(out->relics, snap.relics.data(), sizeof(out->relics));
  std::memcpy(out->unknown_1e, snap.unknown_1e.data(), sizeof(out->unknown_1e));
  std::memcpy(out->hand_count, snap.hand_count.data(), sizeof(out->hand_count));
  std::memcpy(out->body_count, snap.body_count.data(), sizeof(out->body_count));
  std::memcpy(out->hand_order, snap.hand_order.data(), sizeof(out->hand_order));
  std::memcpy(out->body_order, snap.body_order.data(), sizeof(out->body_order));
  for (int s = 0; s < NOCTURNE_SLOT_COUNT; ++s) {
    out->equipped[s] = snap.Equipped(static_cast<nocturne::player::Slot>(s)).value;
  }
  std::memcpy(out->equip_raw, snap.equip_raw.data(), sizeof(out->equip_raw));
  const auto& st = snap.stats;
  auto& o = out->stats;
  o.hp = st.hp, o.hp_max = st.hp_max, o.hearts = st.hearts, o.hearts_max = st.hearts_max;
  o.mp = st.mp, o.mp_max = st.mp_max;
  o.str_base = st.str_base, o.con_base = st.con_base, o.int_base = st.int_base;
  o.lck_base = st.lck_base;
  o.str = st.str, o.con = st.con, o.intl = st.intl, o.lck = st.lck;
  o.level = st.level, o.exp = st.exp, o.gold = st.gold, o.kills = st.kills;
  o.rooms = st.rooms;
  o.play_hours = st.play_hours, o.play_minutes = st.play_minutes;
  o.play_seconds = st.play_seconds, o.play_frames = st.play_frames;
  return NOCTURNE_PLAYER_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneGetRelicFlags(int relic) {
  if (!ValidRelic(relic)) {
    return NOCTURNE_PLAYER_ERR_INVALID_RELIC;
  }
  Snapshot snap;
  if (!GetPlayerStatus().Read(snap)) {
    return NOCTURNE_PLAYER_ERR_UNAVAILABLE;
  }
  return snap.relics[relic];
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneGetItemCount(int item_id) {
  if (!ValidInventoryItem(item_id)) {
    return NOCTURNE_PLAYER_ERR_INVALID_ITEM;
  }
  Snapshot snap;
  if (!GetPlayerStatus().Read(snap)) {
    return NOCTURNE_PLAYER_ERR_UNAVAILABLE;
  }
  return snap.ItemCount(ItemId{static_cast<uint16_t>(item_id)});
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* NocturneGetRelicName(int relic) {
  // The name tables are string literals, so their views are NUL-terminated.
  return ValidRelic(relic) ? nocturne::player::RelicName(static_cast<Relic>(relic)).data() : "?";
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* NocturneGetItemName(int item_id) {
  if (item_id < 0 || item_id >= nocturne::player::kItemIdCount) {
    return "?";
  }
  return ItemId{static_cast<uint16_t>(item_id)}.Name().data();
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneGetItemCategory(int item_id) {
  if (item_id < 0 || item_id >= nocturne::player::kItemIdCount) {
    return NOCTURNE_PLAYER_ERR_INVALID_ITEM;
  }
  return static_cast<int>(ItemId{static_cast<uint16_t>(item_id)}.Category());
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneSetRelicFlags(int relic, int flags) {
  if (!ValidRelic(relic)) {
    return NOCTURNE_PLAYER_ERR_INVALID_RELIC;
  }
  Snapshot snap;
  if (!GetPlayerStatus().Read(snap) ||
      !GetPlayerStatus().SetRelic(static_cast<Relic>(relic), static_cast<uint8_t>(flags & 3))) {
    return NOCTURNE_PLAYER_ERR_UNAVAILABLE;
  }
  return NOCTURNE_PLAYER_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneSetItemCount(int item_id, int count) {
  if (!ValidInventoryItem(item_id)) {
    return NOCTURNE_PLAYER_ERR_INVALID_ITEM;
  }
  if (count < 0 || count > NOCTURNE_ITEM_STACK_MAX) {
    return NOCTURNE_PLAYER_ERR_INVALID_ARGUMENT;
  }
  Snapshot snap;
  if (!GetPlayerStatus().Read(snap) ||
      !GetPlayerStatus().SetItemCount(ItemId{static_cast<uint16_t>(item_id)},
                                      static_cast<uint8_t>(count))) {
    return NOCTURNE_PLAYER_ERR_UNAVAILABLE;
  }
  return NOCTURNE_PLAYER_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneSetPlayerStat(int stat, int32_t value) {
  if (stat < 0 || stat >= NOCTURNE_STAT_COUNT || value < 0) {
    return NOCTURNE_PLAYER_ERR_INVALID_ARGUMENT;
  }
  Snapshot snap;
  if (!GetPlayerStatus().Read(snap) || !GetPlayerStatus().SetStat(stat, value)) {
    return NOCTURNE_PLAYER_ERR_UNAVAILABLE;
  }
  return NOCTURNE_PLAYER_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneRequestLevelUp(void) {
  Snapshot snap;
  if (!GetPlayerStatus().Read(snap)) {
    return NOCTURNE_PLAYER_ERR_UNAVAILABLE;
  }
  if (snap.stats.level >= NOCTURNE_LEVEL_MAX) {
    return NOCTURNE_PLAYER_ERR_MAX_LEVEL;
  }
  nocturne::player::RequestLevelUp();
  return NOCTURNE_PLAYER_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneGetGameCleared(void) {
  int cleared = nocturne::player::GameCleared();
  return cleared < 0 ? NOCTURNE_PLAYER_ERR_UNAVAILABLE : cleared;
}

extern "C" REX_MOD_PLUGIN_EXPORT int NocturneSetGameCleared(int cleared) {
  return nocturne::player::SetGameCleared(cleared != 0) ? NOCTURNE_PLAYER_OK
                                                        : NOCTURNE_PLAYER_ERR_UNAVAILABLE;
}
