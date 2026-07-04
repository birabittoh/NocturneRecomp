// blackboard mod - shared key/value store over the mod event bus, with a
// viewer/editor overlay (F12).
//
// Owns a single retained std::map<std::string, std::string>, guarded by a
// mutex. Other mods write to it purely through rex::system::ModRegistry
// events -- no header, no linked symbol, no `requires` -- so any mod (even a
// binary-only third-party one) can participate just by publishing to the
// right event name:
//
//   "blackboard.set"    bytes = "key=value" (split on the first '=')
//   "blackboard.delete" bytes = "key"
//   "blackboard.clear"  (payload ignored)
//
// After every mutation this mod publishes "blackboard.changed" (u64 = new
// size) for anything that wants to react without polling -- see
// mods_src/bus_inspector, which logs it. It deliberately does not subscribe
// to its own "blackboard.changed" (that would recurse forever the moment
// something else published it back).
//
// See mods_src/event_pong/mod_main.cpp for a producer that writes here with
// zero hard dependency on this mod being enabled at all.
//
// Threading: Subscribe callbacks run on whatever thread calls Publish (for
// event_pong's writes, the command-processor thread -- see
// mod_registry.h's DispatchTick comment), never the UI thread. All access to
// entries_ goes through mutex_ for that reason; OnDraw (UI thread) takes a
// full copy under lock before rendering, and any edits made in OnDraw itself
// (the built-in editor UI) also go through the same lock, not through the
// event path -- this mod owns the data outright, so it doesn't need to
// publish events to itself.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

std::string BytesToString(const std::span<const uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class BlackboardDialog : public rex::ui::ImGuiDialog {
 public:
  BlackboardDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind("bind_blackboard", "F12", "Toggle blackboard overlay",
                          [this] { visible_ = !visible_; });

    if (auto* registry = runtime_ ? runtime_->mod_registry() : nullptr) {
      registry->Subscribe("blackboard.set", [this](const auto& payload) { OnSet(payload); });
      registry->Subscribe("blackboard.delete", [this](const auto& payload) { OnDelete(payload); });
      registry->Subscribe("blackboard.clear", [this](const auto& /*payload*/) { OnClear(); });
    }
  }

  ~BlackboardDialog() override { rex::ui::UnregisterBind("bind_blackboard"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(360, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Blackboard", &visible_)) {
      ImGui::Separator();

      std::vector<std::pair<std::string, std::string>> rows;
      {
        std::lock_guard lock(mutex_);
        rows.assign(entries_.begin(), entries_.end());
      }

      if (rows.empty()) {
        ImGui::TextDisabled("(empty)");
      } else if (ImGui::BeginTable("blackboard_rows", 3,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableHeadersRow();
        for (const auto& [key, value] : rows) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(key.c_str());
          ImGui::TableSetColumnIndex(1);
          ImGui::TextWrapped("%s", value.c_str());
          ImGui::TableSetColumnIndex(2);
          ImGui::PushID(key.c_str());
          if (ImGui::SmallButton("x")) {
            std::lock_guard lock(mutex_);
            entries_.erase(key);
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }

      ImGui::Separator();
      ImGui::SetNextItemWidth(120);
      ImGui::InputText("##key", key_buf_, sizeof(key_buf_));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(150);
      ImGui::InputText("##value", value_buf_, sizeof(value_buf_));
      ImGui::SameLine();
      if (ImGui::Button("Set") && key_buf_[0] != '\0') {
        std::lock_guard lock(mutex_);
        entries_[key_buf_] = value_buf_;
      }

      if (ImGui::Button("Clear all")) {
        std::lock_guard lock(mutex_);
        entries_.clear();
      }
      ImGui::SameLine();
      ImGui::TextDisabled("mods_src/blackboard - press F12 to close");
    }
    ImGui::End();
  }

 private:
  void OnSet(const rex::system::ModRegistry::EventPayload& payload) {
    std::string entry = BytesToString(payload.bytes);
    auto pos = entry.find('=');
    if (pos == std::string::npos) {
      return;
    }
    std::string key = entry.substr(0, pos);
    std::string value = entry.substr(pos + 1);

    size_t new_size;
    {
      std::lock_guard lock(mutex_);
      entries_[key] = value;
      new_size = entries_.size();
    }
    PublishChanged(new_size);
  }

  void OnDelete(const rex::system::ModRegistry::EventPayload& payload) {
    std::string key = BytesToString(payload.bytes);
    size_t new_size;
    {
      std::lock_guard lock(mutex_);
      entries_.erase(key);
      new_size = entries_.size();
    }
    PublishChanged(new_size);
  }

  void OnClear() {
    {
      std::lock_guard lock(mutex_);
      entries_.clear();
    }
    PublishChanged(0);
  }

  void PublishChanged(size_t new_size) {
    auto* registry = runtime_ ? runtime_->mod_registry() : nullptr;
    if (!registry) {
      return;
    }
    rex::system::ModRegistry::EventPayload payload;
    payload.u64 = static_cast<uint64_t>(new_size);
    registry->Publish("blackboard.changed", payload);
  }

  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;

  std::mutex mutex_;
  std::map<std::string, std::string> entries_;

  char key_buf_[64] = {};
  char value_buf_[128] = {};
};

class BlackboardMod : public rex::system::IModPlugin {
 public:
  explicit BlackboardMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<BlackboardDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<BlackboardDialog> dialog_;
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
  return new BlackboardMod(ctx->runtime);
}
