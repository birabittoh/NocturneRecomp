#include "player_inspector.h"

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include "player_status.h"

namespace nocturne {

namespace {

constexpr const char* kWindowTitle = "Inventory inspector##player_inspector";
constexpr size_t kMaxLogLines = 64;

const char* const kSlotNames[player::kSlotCount] = {
    "Right hand", "Left hand", "Head", "Body", "Cape", "Accessory 1", "Accessory 2"};

// Event log shared between the registry subscribers (command-processor thread)
// and the dialog (UI thread).
std::mutex g_log_mutex;
std::deque<std::string> g_log;

void LogLine(std::string line) {
  std::lock_guard lock(g_log_mutex);
  g_log.push_back(std::move(line));
  if (g_log.size() > kMaxLogLines) {
    g_log.pop_front();
  }
}

}  // namespace

class PlayerInspectorDialog : public rex::ui::ImGuiDialog {
 public:
  explicit PlayerInspectorDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    rex::ui::RegisterBind(
        "bind_player_inspector", "I", "Toggle inventory inspector overlay",
        [this] { visible_ = !visible_; }, [this] { return visible_; }, kWindowTitle);
  }
  ~PlayerInspectorDialog() override { rex::ui::UnregisterBind("bind_player_inspector"); }

 protected:
  void OnDraw(ImGuiIO&) override {
    if (!visible_) {
      return;
    }
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kWindowTitle, &visible_)) {
      ImGui::End();
      return;
    }

    auto& status = player::GetPlayerStatus();
    player::Snapshot snap;
    if (!status.Read(snap)) {
      ImGui::TextUnformatted("No save loaded.");
      DrawLog();
      ImGui::End();
      return;
    }

    if (ImGui::BeginTabBar("##tabs")) {
      if (ImGui::BeginTabItem("Relics")) {
        DrawRelics(status, snap);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Items")) {
        DrawItems(status, snap);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Equipment / stats")) {
        DrawEquipment(snap);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    DrawLog();
    ImGui::End();
  }

 private:
  void DrawRelics(player::PlayerStatus& status, const player::Snapshot& snap) {
    ImGui::TextUnformatted("Bit 0 = found, bit 1 = active. Click a box to toggle.");
    for (int i = 0; i < player::kRelicCount; ++i) {
      auto relic = static_cast<player::Relic>(i);
      uint8_t flags = snap.relics[i];
      ImGui::PushID(i);
      bool found = flags & player::kRelicFound;
      bool active = flags & player::kRelicActive;
      if (ImGui::Checkbox("##found", &found)) {
        status.SetRelic(relic, (flags & ~player::kRelicFound) | (found ? player::kRelicFound : 0));
      }
      ImGui::SameLine();
      if (ImGui::Checkbox("##active", &active)) {
        status.SetRelic(relic, (flags & ~player::kRelicActive) | (active ? player::kRelicActive : 0));
      }
      ImGui::SameLine();
      ImGui::Text("%2d  %-14.*s  0x%02X", i, static_cast<int>(player::RelicName(relic).size()),
                  player::RelicName(relic).data(), flags);
      ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::Text("+0x1E bytes:");
    for (uint8_t b : snap.unknown_1e) {
      ImGui::SameLine();
      ImGui::Text("%02X", b);
    }
  }

  void DrawItems(player::PlayerStatus& status, const player::Snapshot& snap) {
    ImGui::Checkbox("Show all", &show_all_);
    ImGui::Text("Raw writes only: the game's order list is not updated (see player_status.h).");
    if (!ImGui::BeginChild("##items", ImVec2(0, -150))) {
      ImGui::EndChild();
      return;
    }
    for (int id = 0; id < player::kHandItemCount + player::kBodyItemCount; ++id) {
      player::ItemId item{static_cast<uint16_t>(id)};
      int count = snap.ItemCount(item);
      if (count == 0 && !show_all_) {
        continue;
      }
      int order = item.IsHand() ? snap.hand_order[item.Index()] : snap.body_order[item.Index()];
      ImGui::PushID(id);
      if (ImGui::SmallButton("-") && count > 0) {
        status.SetItemCount(item, static_cast<uint8_t>(count - 1));
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("+") && count < 99) {
        status.SetItemCount(item, static_cast<uint8_t>(count + 1));
      }
      ImGui::SameLine();
      ImGui::Text("%3d  %-20.*s x%-3d order %d", id, static_cast<int>(item.Name().size()),
                  item.Name().data(), count, order);
      ImGui::PopID();
    }
    ImGui::EndChild();
  }

  void DrawEquipment(const player::Snapshot& snap) {
    ImGui::TextUnformatted("Slots (dwords from +0x29C):");
    for (int s = 0; s < player::kSlotCount; ++s) {
      player::ItemId id = snap.Equipped(static_cast<player::Slot>(s));
      ImGui::Text("  %-12s %3d  %.*s", kSlotNames[s], id.value, static_cast<int>(id.Name().size()),
                  id.Name().data());
    }
    ImGui::Text("Raw dwords from +0x294:");
    for (int i = 0; i < 10; ++i) {
      ImGui::SameLine(0, 8);
      ImGui::Text("%d", snap.equip_raw[i]);
    }
    ImGui::Separator();
    const auto& st = snap.stats;
    ImGui::Text("HP %d/%d  Hearts %d/%d  MP %d/%d", st.hp, st.hp_max, st.hearts, st.hearts_max,
                st.mp, st.mp_max);
    ImGui::Text("STR %d  CON %d  INT %d  LCK %d", st.str, st.con, st.intl, st.lck);
    ImGui::Text("Level %d  Exp %d  Gold %d  Kills %d", st.level, st.exp, st.gold, st.kills);
    ImGui::Text("Play time %d:%02d:%02d.%02d", st.play_hours, st.play_minutes, st.play_seconds,
                st.play_frames);
  }

  void DrawLog() {
    ImGui::Separator();
    ImGui::TextUnformatted("Events");
    if (ImGui::BeginChild("##log", ImVec2(0, 110), ImGuiChildFlags_Borders)) {
      std::lock_guard lock(g_log_mutex);
      for (const auto& line : g_log) {
        ImGui::TextUnformatted(line.c_str());
      }
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();
  }

  bool visible_ = false;
  bool show_all_ = false;
};

PlayerInspector::PlayerInspector() = default;
PlayerInspector::~PlayerInspector() = default;

void PlayerInspector::Bind(rex::Runtime* runtime) {
  using Payload = rex::system::ModRegistry::EventPayload;
  auto* registry = runtime->mod_registry();
  registry->Subscribe("player.save_loaded", [](const Payload&) { LogLine("save loaded"); });
  registry->Subscribe("player.save_unloaded", [](const Payload&) { LogLine("save unloaded"); });
  registry->Subscribe("player.relic_changed", [](const Payload& p) {
    if (p.bytes.size() < 2 || p.u64 >= player::kRelicCount) return;
    auto name = player::RelicName(static_cast<player::Relic>(p.u64));
    char buf[96];
    std::snprintf(buf, sizeof(buf), "relic %.*s: %u -> %u", static_cast<int>(name.size()),
                  name.data(), p.bytes[0], p.bytes[1]);
    LogLine(buf);
  });
  registry->Subscribe("player.item_changed", [](const Payload& p) {
    if (p.bytes.size() < 2) return;
    auto name = player::ItemId{static_cast<uint16_t>(p.u64)}.Name();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "item %.*s: x%u -> x%u", static_cast<int>(name.size()),
                  name.data(), p.bytes[0], p.bytes[1]);
    LogLine(buf);
  });
}

void PlayerInspector::Attach(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !dialog_) {
    dialog_ = std::make_unique<PlayerInspectorDialog>(drawer);
  }
}

PlayerInspector& GetPlayerInspector() {
  static PlayerInspector instance;
  return instance;
}

}  // namespace nocturne
