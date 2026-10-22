#include "player_status.h"

#include <cstring>

#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>

#include "player_status_names.h"

namespace nocturne::player {

namespace {

// Guest address of g_Status, the PS1 PlayerStatus struct, in the .data zero-fill
// area. Found by scanning for lis/addi pairs resolving to it (119 uses). TU
// build: -0x240, the same delta as the other .data globals.
#ifdef NOCTURNE_TU
constexpr uint32_t kStatusAddress = 0x83174700u;
#else
constexpr uint32_t kStatusAddress = 0x83174940u;
#endif
constexpr uint32_t kStatusVanilla = 0x83174940u;
constexpr uint32_t kStatusTu = 0x83174700u;

// Layout, offsets from kStatusAddress. "confirmed" = seen in code or live;
// relics, item counts, order lists and equipment were verified live in the inspector (player_inspector).
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
constexpr uint32_t kStatStr = 0x38;  // second copy (equipment-adjusted)
constexpr uint32_t kStatLevel = 0x48, kStatExp = 0x4C, kStatGold = 0x50, kStatKills = 0x54;
constexpr uint32_t kStatPlay = 0x90;

constexpr uint32_t kBodyFirstHead = 26, kBodyFirstCape = 48, kBodyFirstAccessory = 57;

int32_t BE32(const uint8_t* p) { return static_cast<int32_t>(rex::memory::load_and_swap<uint32_t>(p)); }

// Returns the host pointer for the whole struct, or null if it isn't mapped.
uint8_t* StatusHostPtr(uint32_t size) {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) {
    return nullptr;
  }
  auto* memory = ks->memory();
  auto* heap = memory->LookupHeap(kStatusAddress);
  if (!heap || heap->QueryRangeAccess(kStatusAddress, kStatusAddress + size - 1) ==
                   rex::memory::PageAccess::kNoAccess) {
    return nullptr;
  }
  return memory->TranslateVirtual<uint8_t*>(kStatusAddress);
}

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
  runtime->mod_registry()->RegisterAddress("player.status", kStatusVanilla, kStatusTu);
  runtime->mod_registry()->RegisterTick([this] { Tick(); });
}

bool PlayerStatus::Read(Snapshot& out) const {
  out = Snapshot{};
  const uint8_t* p = StatusHostPtr(kReadSpan);
  if (!p) {
    return false;
  }
  // hp_max is never a real 0 in play, so it doubles as "a save is loaded"; the
  // struct is all-zero until then (same check room_presence.cpp makes).
  const uint8_t* stats = p + kStatsOffset;
  if (BE32(stats + kStatHpMax) <= 0) {
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

PlayerStatus& GetPlayerStatus() {
  static PlayerStatus instance;
  return instance;
}

}  // namespace nocturne::player
