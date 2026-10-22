#include "rando_menu.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include "rando/rando.h"
#include "rando_xex.h"

namespace nocturne {

namespace {

constexpr const char* kBaseXexName = "default.xex";
constexpr const char* kOutputXexName = "Rando.xex";
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
        "Randomizer by MottZilla (github.com/MottZilla/SOTN_XB_RANDO). Generates Rando.xex "
        "from your default.xex; the game boots it on the next launch.");
    ImGui::Separator();

    ImGui::BeginDisabled(running);

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
    DrawGenerateControls(running);

    ImGui::Separator();
    DrawActiveRando();

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
  void DrawGenerateControls(bool running) {
    auto& gen = rando::Generator::Get();
    std::error_code ec;
    const bool have_base = std::filesystem::exists(game_root_ / kBaseXexName, ec);
    const bool title_update = std::filesystem::exists(game_root_ / (std::string(kBaseXexName) + "p"), ec);

    if (title_update) {
      ImGui::TextWrapped(
          "A title update is staged next to default.xex. The randomizer only supports the "
          "unpatched game.");
    }
    if (!have_base) {
      ImGui::TextWrapped("default.xex wasn't found in the game folder.");
    }

    ImGui::BeginDisabled(running || title_update || !have_base);
    if (ImGui::Button("Randomize!")) {
      CopySeedToOptions();
      SaveOptions();
      gen.Start(game_root_ / kBaseXexName, game_root_ / kOutputXexName,
                user_root_ / "spoilers.txt", options_);
    }
    ImGui::EndDisabled();

    if (running) {
      ImGui::SameLine();
      if (ImGui::Button("Abort")) {
        gen.Abort();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted("Processing...");
    } else {
      switch (gen.status()) {
        case rando::Status::kDone:
          if (!applied_) {
            // Select the fresh image; the boot module is chosen before the
            // game starts, so it takes effect on the next launch.
            rex::cvar::SetFlagByName("rando_xex_name", kOutputXexName, /*persist=*/true);
            applied_ = true;
          }
          ImGui::TextWrapped("Done. Restart the game to play the randomized seed. The spoiler log is %s.",
                             (user_root_ / "spoilers.txt").string().c_str());
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
    }
    if (running) {
      applied_ = false;
    }
  }

  void DrawActiveRando() {
    const std::string active = REXCVAR_GET(rando_xex_name);
    if (active.empty()) {
      ImGui::TextUnformatted("Next launch boots: the vanilla game.");
      return;
    }
    ImGui::TextWrapped("Next launch boots: %s", active.c_str());
    if (ImGui::Button("Boot the vanilla game instead")) {
      rex::cvar::SetFlagByName("rando_xex_name", "", /*persist=*/true);
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
    std::ifstream f(OptionsPath());
    std::map<std::string, std::string> kv;
    for (std::string line; std::getline(f, line);) {
      if (auto eq = line.find('='); eq != std::string::npos) {
        kv[line.substr(0, eq)] = line.substr(eq + 1);
      }
    }
    auto b = [&](const char* k, bool& v) {
      if (kv.count(k)) {
        v = kv[k] == "1";
      }
    };
    auto& o = options_;
    if (kv.count("preset")) {
      int p = std::atoi(kv["preset"].c_str());
      if (p >= 0 && p < static_cast<int>(rando::Presets().size())) {
        o.preset = p;
      }
    }
    if (kv.count("complexity")) {
      o.complexity = std::clamp(std::atoi(kv["complexity"].c_str()), 1, 8);
    }
    if (kv.count("seed")) {
      std::snprintf(seed_buf_, sizeof(seed_buf_), "%s", kv["seed"].c_str());
      o.seed = seed_buf_;
    }
    b("relics", o.relics);
    b("starting_gear", o.starting_gear);
    b("equipment", o.equipment);
    b("drops", o.drops);
    b("drop_rates", o.drop_rates);
    b("shop", o.shop);
    b("clock_statue", o.clock_statue);
    b("fast_warps", o.fast_warps);
    b("anti_freeze", o.anti_freeze);
    b("candles", o.candles);
    b("map_tracker", o.map_tracker);
    b("force_no_complexity", o.force_no_complexity);
  }

  void SaveOptions() {
    std::ofstream f(OptionsPath());
    const auto& o = options_;
    f << "preset=" << o.preset << "\ncomplexity=" << o.complexity << "\nseed=" << o.seed << "\n";
    auto b = [&](const char* k, bool v) { f << k << '=' << (v ? 1 : 0) << '\n'; };
    b("relics", o.relics);
    b("starting_gear", o.starting_gear);
    b("equipment", o.equipment);
    b("drops", o.drops);
    b("drop_rates", o.drop_rates);
    b("shop", o.shop);
    b("clock_statue", o.clock_statue);
    b("fast_warps", o.fast_warps);
    b("anti_freeze", o.anti_freeze);
    b("candles", o.candles);
    b("map_tracker", o.map_tracker);
    b("force_no_complexity", o.force_no_complexity);
  }

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
