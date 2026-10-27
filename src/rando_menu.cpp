#include "rando_menu.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <string_view>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include "player_status.h"
#include "rando/rando.h"
#include "rando_runtime.h"
#include "ui_font.h"
#include "ui_text.h"

namespace nocturne {

namespace {

// The title shown is translated; ImGui identifies the window by what follows ##.
constexpr const char* kWindowTitle = "Randomizer##rando";

// Items the randomizer can place that gate progression, shown next to the
// relics: Holy Glasses (final boss), Gold and Silver Ring (Clock Tower
// door), Spike Breaker (Catacombs spikes).
constexpr const char* kKeyItemNames[] = {"Holy Glasses", "Gold Ring", "Silver Ring",
                                         "Spike Breaker"};

player::ItemId FindItem(std::string_view name) {
  for (int id = 0; id < player::kItemIdCount; ++id) {
    player::ItemId item{static_cast<uint16_t>(id)};
    if (item.Name() == name) {
      return item;
    }
  }
  return {};
}

// Owned: in the inventory or worn.
bool Owns(const player::Snapshot& snap, player::ItemId item) {
  if (snap.ItemCount(item) > 0) {
    return true;
  }
  for (int s = 0; s < player::kSlotCount; ++s) {
    if (snap.Equipped(static_cast<player::Slot>(s)).value == item.value) {
      return true;
    }
  }
  return false;
}

// Lit when owned, dimmed otherwise.
void OwnedLabel(std::string_view name, bool owned) {
  if (owned) {
    ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
  } else {
    ImGui::TextDisabled("%.*s", static_cast<int>(name.size()), name.data());
  }
}

}  // namespace

class RandoMenuDialog : public rex::ui::ImGuiDialog {
 public:
  RandoMenuDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path user_root)
      : ImGuiDialog(drawer), user_root_(std::move(user_root)) {
    LoadOptions();
    SetSeedIfEmpty();
    rex::ui::RegisterBind(
        "bind_randomizer", "F5", "Toggle randomizer overlay", [this] { visible_ = !visible_; },
        [this] { return visible_; }, kWindowTitle);
  }

  ~RandoMenuDialog() override { rex::ui::UnregisterBind("bind_randomizer"); }

 protected:
  void OnDraw(ImGuiIO&) override {
    if (!visible_) {
      return;
    }
    const UiFontScope font;
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin((std::string(Tr("randomizer", "title")) + "##rando").c_str(), &visible_)) {
      ImGui::End();
      return;
    }

    auto& gen = rando::Generator::Get();
    const bool running = gen.status() == rando::Status::kRunning;
    const auto& presets = rando::Presets();
    auto& o = options_;
    bool changed = false;

    ImGui::TextWrapped("%s", Tr("randomizer", "intro"));
    ImGui::Separator();

    const auto runtime = GetRandoRuntime().state();
    if (!GetRandoRuntime().InFrontEnd()) {
      // The world a running game plays in can't change under it.
      DrawTracker(runtime);
      ImGui::End();
      return;
    }

    DrawActiveRando(running, runtime);
    ImGui::Separator();

    ImGui::BeginDisabled(running || runtime.request_pending);

    ImGui::TextUnformatted(Tr("randomizer", "seed"));
    ImGui::SetNextItemWidth(-90);
    changed |= ImGui::InputText("##seed", seed_buf_, sizeof(seed_buf_));
    ImGui::SameLine();
    if (ImGui::Button(Tr("randomizer", "random"))) {
      std::snprintf(seed_buf_, sizeof(seed_buf_), "%s", rando::RandomSeedName().c_str());
      changed = true;
    }

    ImGui::TextUnformatted(Tr("randomizer", "preset_mode"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##preset", presets[o.preset].name)) {
      for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        if (ImGui::Selectable(presets[i].name, i == o.preset) && i != o.preset) {
          SelectPreset(i);
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
    const rando::Preset& preset = presets[o.preset];

    if (preset.picks_complexity) {
      ImGui::TextUnformatted(Tr("randomizer", "min_complexity"));
      ImGui::SetNextItemWidth(-1);
      changed |= ImGui::SliderInt("##complexity", &o.complexity, 1, 8);
    }

    ImGui::Spacing();
    if (!preset.fixes_randomization) {
      changed |= ImGui::Checkbox(Tr("randomizer", "relics"), &o.relics);
      changed |= ImGui::Checkbox(Tr("randomizer", "starting_gear"), &o.starting_gear);
      changed |= ImGui::Checkbox(Tr("randomizer", "equipment"), &o.equipment);
      changed |= ImGui::Checkbox(Tr("randomizer", "drops"), &o.drops);
      changed |= ImGui::Checkbox(Tr("randomizer", "shop"), &o.shop);
      changed |= ImGui::Checkbox(Tr("randomizer", "candles"), &o.candles);
    }
    changed |= ImGui::Checkbox(Tr("randomizer", "force_no_complexity"), &o.force_no_complexity);
    changed |= ImGui::Checkbox(Tr("randomizer", "map_tracker"), &o.map_tracker);
    changed |= ImGui::Checkbox(Tr("randomizer", "clock_statue"), &o.clock_statue);
    changed |= ImGui::Checkbox(Tr("randomizer", "drop_rates"), &o.drop_rates);
    changed |= ImGui::Checkbox(Tr("randomizer", "fast_warps"), &o.fast_warps);
    changed |= ImGui::Checkbox(Tr("randomizer", "anti_freeze"), &o.anti_freeze);

    ImGui::EndDisabled();
    if (changed) {
      CopySeedToOptions();
      SaveOptions();
    }

    ImGui::Spacing();
    DrawGenerateControls(running, runtime);

    ImGui::Separator();
    ImGui::TextUnformatted(preset.name);
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const char* line : preset.description) {
      ImGui::TextWrapped("%s", line);
    }
    const auto log = gen.Log();
    if (!log.empty()) {
      ImGui::Separator();
      for (const auto& line : log) {
        ImGui::TextWrapped("%s", line.c_str());
      }
      if (running) {
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();

    ImGui::End();
  }

 private:
  void DrawGenerateControls(bool running, const RandoRuntime::State& runtime) {
    auto& gen = rando::Generator::Get();
    if (!runtime.supported) {
      ImGui::TextWrapped("%s", runtime.unsupported_reason.c_str());
    }

    // These options would give the seed that is already active: nothing to
    // do until something changes.
    const bool already_active = runtime.active && runtime.active->options == options_;
    if (already_active && !running) {
      ImGui::TextDisabled("%s", Tr("randomizer", "seed_active"));
    } else {
      ImGui::BeginDisabled(running || runtime.request_pending || !runtime.supported);
      if (ImGui::Button(Tr("randomizer", "randomize"))) {
        CopySeedToOptions();
        SaveOptions();
        gen.Start(GetRandoRuntime().base(), user_root_ / "spoilers.txt", options_);
        applied_ = false;
      }
      ImGui::EndDisabled();
    }

    if (running) {
      ImGui::SameLine();
      if (ImGui::Button(Tr("randomizer", "abort"))) {
        gen.Abort();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(Tr("randomizer", "generating"));
      return;
    }
    switch (gen.status()) {
      case rando::Status::kDone:
        if (!applied_) {
          // Applied by the guest thread between frames; see rando_runtime.h.
          GetRandoRuntime().RequestApply(gen.result());
          applied_ = true;
        }
        break;
      case rando::Status::kFailed:
        ImGui::TextWrapped("%s", Tr("randomizer", "failed"));
        break;
      case rando::Status::kAborted:
        ImGui::TextWrapped("%s", Tr("randomizer", "aborted"));
        break;
      default:
        break;
    }
    if (runtime.request_pending) {
      ImGui::TextUnformatted(Tr("randomizer", "applying"));
    }
  }

  // Status line: the active seed (with the way back to vanilla, kept away
  // from Randomize) or the vanilla game.
  void DrawActiveRando(bool running, const RandoRuntime::State& runtime) {
    if (!runtime.active) {
      ImGui::TextUnformatted(Tr("randomizer", "active_vanilla"));
    } else {
      DrawSeedHeader(*runtime.active, runtime);
      ImGui::BeginDisabled(running || runtime.request_pending);
      if (ImGui::Button(Tr("randomizer", "return_to_vanilla"))) {
        GetRandoRuntime().RequestRevert();
      }
      ImGui::EndDisabled();
      if (ImGui::CollapsingHeader(Tr("randomizer", "spoiler_log"))) {
        DrawSpoilers(*runtime.active);
      }
    }
    if (!runtime.message.empty()) {
      ImGui::TextWrapped("%s", runtime.message.c_str());
    }
  }

  void DrawSeedHeader(const rando::Result& seed, const RandoRuntime::State& runtime) {
    const auto& presets = rando::Presets();
    ImGui::TextWrapped("%s", Tr("randomizer", "active_seed", seed.options.seed).c_str());
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016" PRIx64, seed.patches.Hash());
    ImGui::TextUnformatted(
        TrArgs("randomizer", "seed_summary",
               {presets[std::min<size_t>(seed.options.preset, presets.size() - 1)].name,
                std::to_string(seed.complexity), hash})
            .c_str());
    if (runtime.inactive_code_patches) {
      ImGui::TextWrapped(
          "%s", Tr("randomizer", "inactive_patches", std::to_string(runtime.inactive_code_patches))
                    .c_str());
    }
  }

  void DrawSpoilers(const rando::Result& seed) {
    ImGui::BeginChild("##spoilers", ImVec2(0, 200), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(seed.spoilers.c_str(), seed.spoilers.c_str() + seed.spoilers.size());
    ImGui::EndChild();
  }

  // Read-only view while a game runs, for randomized and vanilla games alike.
  void DrawTracker(const RandoRuntime::State& runtime) {
    if (runtime.active) {
      DrawSeedHeader(*runtime.active, runtime);
    } else {
      ImGui::TextUnformatted(Tr("randomizer", "playing_vanilla"));
    }
    if (!runtime.message.empty()) {
      ImGui::TextWrapped("%s", runtime.message.c_str());
    }
    ImGui::TextDisabled("%s", Tr("randomizer", "quit_to_change"));
    ImGui::Separator();

    player::Snapshot snap;
    if (!player::GetPlayerStatus().Read(snap)) {
      ImGui::TextUnformatted(Tr("randomizer", "waiting_for_save"));
      return;
    }

    ImGui::SeparatorText(Tr("randomizer", "relics_header"));
    const int vlad_first = static_cast<int>(player::Relic::HeartOfVlad);
    DrawRelicGrid(snap, 0, vlad_first);
    ImGui::SeparatorText(Tr("randomizer", "vlad_relics_header"));
    DrawRelicGrid(snap, vlad_first, player::kRelicCount);

    ImGui::SeparatorText(Tr("randomizer", "key_items_header"));
    if (ImGui::BeginTable("##key_items", 2)) {
      for (const char* name : kKeyItemNames) {
        ImGui::TableNextColumn();
        OwnedLabel(name, Owns(snap, FindItem(name)));
      }
      ImGui::EndTable();
    }

    if (runtime.active) {
      ImGui::Spacing();
      ImGui::Checkbox(Tr("randomizer", "reveal_spoilers"), &reveal_spoilers_);
      if (reveal_spoilers_) {
        DrawSpoilers(*runtime.active);
      }
    }

    int owned = 0;
    for (int id = 0; id < player::kHandItemCount + player::kBodyItemCount; ++id) {
      owned += snap.ItemCount(player::ItemId{static_cast<uint16_t>(id)}) > 0;
    }
    const std::string label =
        Tr("randomizer", "inventory", std::to_string(owned)) + "###inventory";
    if (ImGui::CollapsingHeader(label.c_str())) {
      ImGui::BeginChild("##inventory", ImVec2(0, 0), ImGuiChildFlags_Borders);
      for (int id = 0; id < player::kHandItemCount + player::kBodyItemCount; ++id) {
        player::ItemId item{static_cast<uint16_t>(id)};
        if (const int count = snap.ItemCount(item)) {
          ImGui::Text("%-20.*s x%d", static_cast<int>(item.Name().size()), item.Name().data(),
                      count);
        }
      }
      ImGui::EndChild();
    }
  }

  void DrawRelicGrid(const player::Snapshot& snap, int first, int last) {
    ImGui::PushID(first);
    if (!ImGui::BeginTable("##relics", 3)) {
      ImGui::PopID();
      return;
    }
    for (int i = first; i < last; ++i) {
      auto relic = static_cast<player::Relic>(i);
      ImGui::TableNextColumn();
      OwnedLabel(player::RelicName(relic), snap.RelicFlags(relic) & player::kRelicFound);
    }
    ImGui::EndTable();
    ImGui::PopID();
  }

  void SelectPreset(int index) {
    options_.preset = index;
    const rando::Preset& p = rando::Presets()[index];
    if (p.picks_complexity) {
      options_.complexity = p.default_complexity;
    }
    if (p.sets_normal_options) {
      options_.relics = options_.starting_gear = options_.equipment = options_.drops =
          options_.shop = options_.candles = true;
    }
  }

  void SetSeedIfEmpty() {
    if (!seed_buf_[0]) {
      std::snprintf(seed_buf_, sizeof(seed_buf_), "%s", rando::RandomSeedName().c_str());
      CopySeedToOptions();
    }
  }

  void CopySeedToOptions() { options_.seed = seed_buf_; }

  std::filesystem::path OptionsPath() const { return user_root_ / "randomizer.txt"; }

  void LoadOptions() {
    rando::LoadOptionsFile(OptionsPath(), &options_);
    std::snprintf(seed_buf_, sizeof(seed_buf_), "%s", options_.seed.c_str());
  }

  void SaveOptions() { rando::SaveOptionsFile(OptionsPath(), options_); }

  std::filesystem::path user_root_;
  rando::Options options_;
  char seed_buf_[256] = {};
  bool visible_ = false;
  bool applied_ = false;
  bool reveal_spoilers_ = false;  // tracker; off every session
};

RandoMenu::RandoMenu() = default;
RandoMenu::~RandoMenu() = default;

void RandoMenu::Attach(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& user_data_root) {
  if (drawer && !dialog_) {
    dialog_ = std::make_unique<RandoMenuDialog>(drawer, user_data_root);
  }
}

RandoMenu& GetRandoMenu() {
  static RandoMenu instance;
  return instance;
}

}  // namespace nocturne
