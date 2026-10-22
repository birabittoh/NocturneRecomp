#include "rando_menu.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include "rando/rando.h"
#include "rando_runtime.h"

namespace nocturne {

namespace {

constexpr const char* kBaseXexName = "default.xex";
constexpr const char* kWindowTitle = "Randomizer##rando";

}  // namespace

class RandoMenuDialog : public rex::ui::ImGuiDialog {
 public:
  RandoMenuDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path game_root,
                  std::filesystem::path user_root)
      : ImGuiDialog(drawer),
        game_root_(std::move(game_root)),
        user_root_(std::move(user_root)) {
    LoadOptions();
    SetSeedIfEmpty();
    rex::ui::RegisterBind(
        "bind_randomizer", "F6", "Toggle randomizer overlay", [this] { visible_ = !visible_; },
        [this] { return visible_; }, kWindowTitle);
  }

  ~RandoMenuDialog() override { rex::ui::UnregisterBind("bind_randomizer"); }

 protected:
  void OnDraw(ImGuiIO&) override {
    if (!visible_) {
      return;
    }
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kWindowTitle, &visible_)) {
      ImGui::End();
      return;
    }

    auto& gen = rando::Generator::Get();
    const bool running = gen.status() == rando::Status::kRunning;
    const auto& presets = rando::Presets();
    auto& o = options_;
    bool changed = false;

    ImGui::TextWrapped(
        "Randomizer by MottZilla (github.com/MottZilla/SOTN_XB_RANDO). Randomize from the "
        "title screen or main menu, then start a new game.");
    ImGui::Separator();

    DrawActiveRando();
    ImGui::Separator();

    const auto runtime = GetRandoRuntime().state();
    if (GetRandoRuntime().SaveLoaded()) {
      // The world a loaded save plays in can't change under it.
      ImGui::TextWrapped(
          "A save is loaded. Return to the title screen to randomize or go back to vanilla.");
      ImGui::End();
      return;
    }

    ImGui::BeginDisabled(running || runtime.request_pending);

    ImGui::TextUnformatted("Seed");
    ImGui::SetNextItemWidth(-90);
    changed |= ImGui::InputText("##seed", seed_buf_, sizeof(seed_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Random")) {
      std::snprintf(seed_buf_, sizeof(seed_buf_), "%s", rando::RandomSeedName().c_str());
      changed = true;
    }

    ImGui::TextUnformatted("Preset / Mode");
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
      ImGui::TextUnformatted("Minimum Complexity");
      ImGui::SetNextItemWidth(-1);
      changed |= ImGui::SliderInt("##complexity", &o.complexity, 1, 8);
    }

    ImGui::Spacing();
    if (!preset.fixes_randomization) {
      changed |= ImGui::Checkbox("Randomize Relics", &o.relics);
      changed |= ImGui::Checkbox("Randomize Starting Gear", &o.starting_gear);
      changed |= ImGui::Checkbox("Randomize Equipment Pickups", &o.equipment);
      changed |= ImGui::Checkbox("Randomize Enemy Drops", &o.drops);
      changed |= ImGui::Checkbox("Randomize Shop Items", &o.shop);
      changed |= ImGui::Checkbox("Randomize Candles", &o.candles);
    }
    changed |= ImGui::Checkbox("Force No Minimum Complexity", &o.force_no_complexity);
    changed |= ImGui::Checkbox("Map Tracker (In-Game Show Loc)", &o.map_tracker);
    changed |= ImGui::Checkbox("Remove Clock Statue", &o.clock_statue);
    changed |= ImGui::Checkbox("Boost Enemy Drops", &o.drop_rates);
    changed |= ImGui::Checkbox("Fast Warps", &o.fast_warps);
    changed |= ImGui::Checkbox("Anti-Freeze", &o.anti_freeze);

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

    ImGui::BeginDisabled(running || runtime.request_pending || !runtime.supported);
    if (ImGui::Button("Randomize!")) {
      CopySeedToOptions();
      SaveOptions();
      gen.Start(game_root_ / kBaseXexName, user_root_ / "spoilers.txt", options_);
      applied_ = false;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!runtime.active);
    if (ImGui::Button("Return to vanilla")) {
      GetRandoRuntime().RequestRevert();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (running) {
      ImGui::SameLine();
      if (ImGui::Button("Abort")) {
        gen.Abort();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted("Generating...");
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
        ImGui::TextWrapped("Generation failed; see the log below.");
        break;
      case rando::Status::kAborted:
        ImGui::TextWrapped("Aborted.");
        break;
      default:
        break;
    }
    if (runtime.request_pending) {
      ImGui::TextUnformatted("Applying...");
    }
  }

  void DrawActiveRando() {
    const auto runtime = GetRandoRuntime().state();
    if (!runtime.active) {
      ImGui::TextUnformatted("Active: the vanilla game.");
    } else {
      const auto& seed = *runtime.active;
      const auto& presets = rando::Presets();
      ImGui::TextWrapped("Active seed: %s", seed.options.seed.c_str());
      ImGui::Text("Preset: %s    Complexity: %d    Hash: %016" PRIx64,
                  presets[std::min<size_t>(seed.options.preset, presets.size() - 1)].name,
                  seed.complexity, seed.patches.Hash());
      if (runtime.inactive_code_patches) {
        ImGui::TextWrapped(
            "%zu code patch(es) of this seed couldn't be activated, so parts of the preset may "
            "misbehave. See the log.",
            runtime.inactive_code_patches);
      }
      if (ImGui::CollapsingHeader("Spoiler log")) {
        ImGui::BeginChild("##spoilers", ImVec2(0, 200), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(seed.spoilers.c_str(), seed.spoilers.c_str() + seed.spoilers.size());
        ImGui::EndChild();
      }
    }
    if (!runtime.message.empty()) {
      ImGui::TextWrapped("%s", runtime.message.c_str());
    }
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

  std::filesystem::path game_root_;
  std::filesystem::path user_root_;
  rando::Options options_;
  char seed_buf_[256] = {};
  bool visible_ = false;
  bool applied_ = false;
};

RandoMenu::RandoMenu() = default;
RandoMenu::~RandoMenu() = default;

void RandoMenu::Attach(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& game_data_root,
                       const std::filesystem::path& user_data_root) {
  if (drawer && !dialog_) {
    dialog_ = std::make_unique<RandoMenuDialog>(drawer, game_data_root, user_data_root);
  }
}

RandoMenu& GetRandoMenu() {
  static RandoMenu instance;
  return instance;
}

}  // namespace nocturne
