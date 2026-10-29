// nocturnerecomp - typed access to the game's player-status struct (the PS1
// PlayerStatus equivalent, `g_Status`): relics, item counts, equipment, stats.
//
// Everything reads live guest memory, so it is only meaningful while a save is
// loaded (Snapshot::loaded). The layout is documented next to the offsets in
// player_status.cpp, including which rows are confirmed and which are still
// provisional. Mods reach it through the C ABI in api/nocturne_player_api.h
// (exported from the bottom of player_status.cpp), which mirrors this file's
// constants -- keep the two in sync -- or the raw "player.status" registry
// address.
//
// Events (ModRegistry::Publish, fired from a per-guest-frame tick):
//   player.save_loaded    Snapshot::loaded edge 0 -> 1; no relic/item events follow
//   player.save_unloaded  Snapshot::loaded edge 1 -> 0 (Quit to Title)
//   player.relic_changed  u64 = relic index, bytes = {old flags, new flags}
//   player.item_changed   u64 = item id,     bytes = {old count, new count}
// Requests (mods publish, the game applies on the guest thread):
//   player.request_level_up  one real level up (sets exp to the threshold, runs the game's tick)
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

struct PPCContext;

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne::player {

inline constexpr int kRelicCount = 30;
inline constexpr int kHandItemCount = 169;
inline constexpr int kBodyItemCount = 90;
// 0-168 hand, 169-258 body, 259-260 randomizer pseudo-items (not inventory).
inline constexpr int kItemIdCount = 261;

enum class Relic : uint8_t {
  SoulOfBat, FireOfBat, EchoOfBat, ForceOfEcho, SoulOfWolf, PowerOfWolf, SkillOfWolf,
  FormOfMist, PowerOfMist, GasCloud, CubeOfZoe, SpiritOrb, GravityBoots, LeapStone,
  HolySymbol, FaerieScroll, JewelOfOpen, MermanStatue, BatCard, GhostCard, FaerieCard,
  DemonCard, SwordCard, SpriteCard, NosedevilCard, HeartOfVlad, ToothOfVlad, RibOfVlad,
  RingOfVlad, EyeOfVlad,
};

// Relic byte: bit 0 = found, bit 1 = active (toggled in the relic menu).
inline constexpr uint8_t kRelicFound = 1;
inline constexpr uint8_t kRelicActive = 2;

enum class ItemCategory : uint8_t { Hand, Armor, Head, Cape, Accessory, Special };

struct ItemId {
  uint16_t value = 0;
  static constexpr ItemId Hand(int i) { return {static_cast<uint16_t>(i)}; }
  static constexpr ItemId Body(int i) { return {static_cast<uint16_t>(kHandItemCount + i)}; }
  constexpr bool IsHand() const { return value < kHandItemCount; }
  constexpr bool IsBody() const {
    return value >= kHandItemCount && value < kHandItemCount + kBodyItemCount;
  }
  constexpr bool IsInventory() const { return IsHand() || IsBody(); }
  // Index into the hand or body arrays; only valid when IsHand() / IsBody().
  constexpr int Index() const { return IsHand() ? value : value - kHandItemCount; }
  ItemCategory Category() const;
  std::string_view Name() const;
};

std::string_view RelicName(Relic relic);

// Equipment slots, in the order the game stores them (dwords from +0x29C).
// Confirmed live: hands hold hand-item ids, the other slots hold body-array
// indices (0-89, i.e. item id - 169). Left/right naming follows the menu.
enum class Slot : uint8_t { RightHand, LeftHand, Head, Body, Cape, Accessory1, Accessory2 };
inline constexpr int kSlotCount = 7;

struct Stats {
  int32_t hp = 0, hp_max = 0;
  int32_t hearts = 0, hearts_max = 0;
  int32_t mp = 0, mp_max = 0;
  int32_t str_base = 0, con_base = 0, int_base = 0, lck_base = 0;  // without equipment
  int32_t str = 0, con = 0, intl = 0, lck = 0;  // with equipment bonuses
  int32_t level = 0, exp = 0, gold = 0, kills = 0;
  int32_t rooms = 0;  // outside g_Status, see kRoomsAddress
  int32_t play_hours = 0, play_minutes = 0, play_seconds = 0, play_frames = 0;
};

// One consistent copy of the struct, read in a single pass.
struct Snapshot {
  // True from the moment a save or new game has populated the struct until
  // Quit to Title. False in the front end and at file select.
  bool loaded = false;
  std::array<uint8_t, kRelicCount> relics{};
  std::array<uint8_t, 8> unknown_1e{};  // +0x1E, XB-only, purpose unknown
  std::array<uint8_t, kHandItemCount> hand_count{};
  std::array<uint8_t, kBodyItemCount> body_count{};
  std::array<uint8_t, kHandItemCount> hand_order{};
  std::array<uint8_t, kBodyItemCount> body_order{};
  std::array<int32_t, 10> equip_raw{};  // dwords from +0x294; slots start at [2], see Slot
  Stats stats;

  uint8_t RelicFlags(Relic r) const { return relics[static_cast<int>(r)]; }
  int ItemCount(ItemId id) const;
  ItemId Equipped(Slot slot) const {
    int s = static_cast<int>(slot);
    int v = equip_raw[2 + s];
    return s < 2 ? ItemId::Hand(v) : ItemId::Body(v);
  }
};

class PlayerStatus {
 public:
  // Registers "player.status" and the per-frame tick that publishes events.
  void Bind(rex::Runtime* runtime);

  // Reads the struct into `out`. Safe from any thread (reads can tear against
  // the guest, which is fine for display). Returns out.loaded.
  bool Read(Snapshot& out) const;

  // Raw writes, for tools and cheats. They do not go through the game's own
  // give/obtain routines (not found yet, see the handoff's A.2), so SetItemCount
  // leaves the inventory order list alone and a new item may not appear in menus.
  bool SetRelic(Relic relic, uint8_t flags);
  bool SetItemCount(ItemId id, uint8_t count);
  // One Stats field, by NOCTURNE_STAT_* index (api/nocturne_player_api.h).
  bool SetStat(int stat, int32_t value);

  // Diffs against the previous snapshot and publishes player.* events.
  // Registered as a guest-frame tick by Bind().
  void Tick();

 private:
  rex::Runtime* runtime_ = nullptr;
  Snapshot last_;
};

// Queues a level up, as the "player.request_level_up" event does.
void RequestLevelUp();

// Performs a level up queued by RequestLevelUp() or the "player.request_level_up" event (any
// payload). Needs a live guest context, so it is called from the per-frame hook
// (GraphicsSettings_PerFrame).
void ApplyRequestedLevelUp(PPCContext& ctx, uint8_t* base);

// The game-clear flag (OR of every save slot's): 1, 0, or -1 while unmapped.
int GameCleared();
bool SetGameCleared(bool cleared);

PlayerStatus& GetPlayerStatus();

}  // namespace nocturne::player
