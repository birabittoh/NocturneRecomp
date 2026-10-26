#include "player_status.h"

#include <atomic>
#include <cstring>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>

#include "game_state.h"
#include "player_status_names.h"

namespace nocturne::player {

namespace {

// Guest address of g_Status, the PS1 PlayerStatus struct, in the .data zero-fill
// area. Found by scanning for lis/addi pairs resolving to it (119 uses).
constexpr uint32_t kStatusAddress = 0x83174940u;

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
  runtime->mod_registry()->RegisterAddress("player.status", kStatusAddress);
  runtime->mod_registry()->RegisterAddress("app.state", kAppObjectPtrAddr + 4);
  runtime->mod_registry()->RegisterTick([this] { Tick(); });
  runtime->mod_registry()->Subscribe(
      "player.request_level_up", [](const rex::system::ModRegistry::EventPayload&) {
        g_level_up_requested.store(true, std::memory_order_release);
      });
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
