// ui_color mod - overlay (F7) with a color picker for the game's UI accent
// color, synced live with guest memory.
//
// The SOTN "Settings -> Accent Color" setting is three consecutive big-endian
// uint32 fields (R, G, B; each 0-15) in the XEX's static data -- see
// src/accent_color.cpp for the base game's read-only watcher that themes the
// overlay from this same struct. This mod adds the write side: a picker that
// pokes new values into that struct. Because the base game (and its watcher)
// read the struct every frame, writes here propagate automatically -- no IPC
// needed.
//
// The struct's guest address differs between the vanilla and title-update
// (TU) builds, because a TU relocates the whole image. Rather than
// hardcoding either address itself, this mod looks it up by name
// ("ui.accent_color") from the shared registry that mods_src/game_symbols
// publishes into (rex::system::ModRegistry), which resolves the vanilla/TU
// split internally. mod.toml declares `requires = "game_symbols"` so the SDK
// guarantees that mod is enabled and loaded first -- see
// docs/making-mods.md's "Library mods and the shared registry" section.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <imgui.h>

#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

constexpr uint32_t kRedOffset = 0;
constexpr uint32_t kGreenOffset = 4;
constexpr uint32_t kBlueOffset = 8;
constexpr uint32_t kSpanSize = kBlueOffset + 4;  // 12 bytes covering all three fields

uint32_t ReadGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address) {
  const uint8_t* host_address = memory->TranslateVirtual<const uint8_t*>(guest_address);
  return rex::memory::load_and_swap<uint32_t>(host_address);
}

void WriteGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address, uint32_t value) {
  uint8_t* host_address = memory->TranslateVirtual<uint8_t*>(guest_address);
  rex::memory::store_and_swap<uint32_t>(host_address, value);
}

int QuantizeChannel(float value) {
  return std::clamp(static_cast<int>(std::lround(value * 15.0f)), 0, 15);
}

class UiColorDialog : public rex::ui::ImGuiDialog {
 public:
  UiColorDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind("bind_ui_color", "F7", "Toggle UI color picker",
                          [this] { visible_ = !visible_; });
  }

  ~UiColorDialog() override { rex::ui::UnregisterBind("bind_ui_color"); }

  // Called once KernelState/the executable module are live (see
  // UiColorMod::OnModuleLaunched). Looks up the accent address published by
  // game_symbols (see mods_src/game_symbols/mod_main.cpp), which resolves
  // vanilla vs TU for us.
  void ResolveAddress() {
    if (runtime_ && runtime_->mod_registry()) {
      if (auto addr = runtime_->mod_registry()->FindAddress("ui.accent_color")) {
        addr_ = *addr;
        addr_resolved_ = true;
      }
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UI Color Picker", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (!addr_resolved_) {
      ImGui::TextDisabled("Waiting for the game to start...");
      ImGui::End();
      return;
    }

    auto* memory = runtime_ ? runtime_->memory() : nullptr;
    auto* heap = memory ? memory->LookupHeap(addr_) : nullptr;
    bool readable = heap && heap->QueryRangeAccess(addr_, addr_ + kSpanSize - 1) !=
                                rex::memory::PageAccess::kNoAccess;

    if (!memory || !readable) {
      // A fresh game process may reuse this address for an unrelated struct
      // before the accent color one is live, so don't trust a stale latch
      // from a previous session.
      ever_populated_ = false;
      ImGui::TextDisabled("Start or load a game to edit the accent color.");
      ImGui::End();
      return;
    }

    uint32_t r = ReadGuestU32BE(memory, addr_ + kRedOffset);
    uint32_t g = ReadGuestU32BE(memory, addr_ + kGreenOffset);
    uint32_t b = ReadGuestU32BE(memory, addr_ + kBlueOffset);
    bool valid_range = r <= 15 && g <= 15 && b <= 15;

    // Latched rather than re-checked every frame: a genuine (0, 0, 0) accent
    // color (i.e. black, picked by the user) reads identically to "the game
    // hasn't populated the struct yet". Once we've seen one real value,
    // trust all subsequent reads -- including black -- instead of flipping
    // back to the placeholder text.
    if (valid_range && (r != 0 || g != 0 || b != 0)) {
      ever_populated_ = true;
    }
    bool populated = valid_range && ever_populated_;

    if (!populated) {
      ImGui::TextDisabled("Start or load a game to edit the accent color.");
      ImGui::End();
      return;
    }

    // Only overwrite our local color from memory while the user isn't
    // actively dragging a widget here -- otherwise a slow guest-side refresh
    // could fight the user's own drag. This also picks up changes made
    // through the game's own Settings -> Accent Color menu.
    if (!ImGui::IsAnyItemActive()) {
      rgb_[0] = float(r) / 15.0f;
      rgb_[1] = float(g) / 15.0f;
      rgb_[2] = float(b) / 15.0f;
    }

    ImGui::TextUnformatted("Accent color:");
    // ColorPicker3's built-in numeric readout only ever shows 0-255 (or
    // 0.00-1.00 with the Float flag) -- there's no ImGui option for a custom
    // channel range. Hide it (NoInputs) since the 0-15 sliders below already
    // give the exact game value; showing both would just be two different
    // scales for the same channels.
    ImGui::ColorPicker3("##accent", rgb_,
                       ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs);

    ImGui::Separator();
    int qr = QuantizeChannel(rgb_[0]);
    int qg = QuantizeChannel(rgb_[1]);
    int qb = QuantizeChannel(rgb_[2]);
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt("R", &qr, 0, 15);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt("G", &qg, 0, 15);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt("B", &qb, 0, 15);

    // Sliders are the source of truth for the swatch too, so dragging one
    // updates the wheel/square immediately.
    rgb_[0] = float(qr) / 15.0f;
    rgb_[1] = float(qg) / 15.0f;
    rgb_[2] = float(qb) / 15.0f;

    uint32_t wr = static_cast<uint32_t>(qr);
    uint32_t wg = static_cast<uint32_t>(qg);
    uint32_t wb = static_cast<uint32_t>(qb);
    if (!has_written_ || wr != last_written_r_ || wg != last_written_g_ ||
        wb != last_written_b_) {
      WriteGuestU32BE(memory, addr_ + kRedOffset, wr);
      WriteGuestU32BE(memory, addr_ + kGreenOffset, wg);
      WriteGuestU32BE(memory, addr_ + kBlueOffset, wb);
      last_written_r_ = wr;
      last_written_g_ = wg;
      last_written_b_ = wb;
      has_written_ = true;
    }

    ImGui::End();
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;

  bool addr_resolved_ = false;
  uint32_t addr_ = 0;
  bool ever_populated_ = false;

  float rgb_[3] = {0.0f, 0.0f, 0.0f};

  bool has_written_ = false;
  uint32_t last_written_r_ = 0;
  uint32_t last_written_g_ = 0;
  uint32_t last_written_b_ = 0;
};

class UiColorMod : public rex::system::IModPlugin {
 public:
  explicit UiColorMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<UiColorDialog>(drawer, runtime_);
  }

  void OnModuleLaunched() override {
    // KernelState is live now, but the lookup itself is just a registry read
    // (game_symbols already published the address in its own
    // OnCreateDialogs, dispatched before this).
    if (dialog_) {
      dialog_->ResolveAddress();
    }
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<UiColorDialog> dialog_;
};

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new UiColorMod(ctx->runtime);
}
