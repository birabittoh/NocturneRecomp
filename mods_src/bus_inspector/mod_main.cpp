// bus_inspector mod - live log overlay (F5) for the shared mod event bus.
//
// Subscribes to every event name used by the other event-bus example mods
// ("sample.ping" from event_ping, "blackboard.set"/"blackboard.delete"/
// "blackboard.changed" from blackboard/event_pong) and renders a scrolling
// log of what it observed: event name, the u64/f64 fields, and the byte
// payload as text. Deliberately has no `requires` on any of them -- it just
// shows whatever traffic happens to exist, including none.
//
// The interesting bit this mod demonstrates that no other example does:
// rex::system::ModRegistry::Subscribe lets *multiple* mods subscribe to the
// same event name (see mod_registry.h: "Multiple subscribers may share the
// same event name; all are invoked, in registration order"). event_pong and
// blackboard already each subscribe to their own events; this mod piles a
// second (or third) subscriber onto "sample.ping" and "blackboard.set",
// proving fan-out rather than last-writer-wins.
//
// Threading: exactly like event_pong/blackboard, Subscribe callbacks run on
// whatever thread published the event (the command-processor thread for
// every event in this project, since they all originate from event_ping's
// tick or blackboard's own republish) -- never touch ImGui there. This mod
// only appends a formatted line to a mutex-guarded deque; OnDraw (UI thread)
// renders a snapshot under the same lock.

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

constexpr size_t kMaxLines = 200;

// Payload bytes aren't guaranteed to be printable text for every event (they
// are for all the demo events in this project), so fall back to a length
// note rather than printing raw bytes as if they were a C string.
std::string DescribeBytes(std::span<const uint8_t> bytes) {
  for (uint8_t b : bytes) {
    if (b == 0 || b > 0x7E) {
      return "<" + std::to_string(bytes.size()) + " bytes>";
    }
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class BusInspectorDialog : public rex::ui::ImGuiDialog {
 public:
  BusInspectorDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind("bind_bus_inspector", "F5", "Toggle bus inspector overlay",
                          [this] { visible_ = !visible_; });

    auto* registry = runtime_ ? runtime_->mod_registry() : nullptr;
    if (!registry) {
      return;
    }
    for (const char* event_name :
        {"sample.ping", "blackboard.set", "blackboard.delete", "blackboard.changed"}) {
      registry->Subscribe(event_name, [this, event_name](const auto& payload) {
        OnEvent(event_name, payload);
      });
    }
  }

  ~BusInspectorDialog() override { rex::ui::UnregisterBind("bind_bus_inspector"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 280), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Bus Inspector", &visible_)) {
      ImGui::TextDisabled("Live log of sample.ping / blackboard.* mod-bus events");
      if (ImGui::Button("Clear")) {
        std::lock_guard lock(mutex_);
        lines_.clear();
      }
      ImGui::Separator();

      ImGui::BeginChild("bus_inspector_log", ImVec2(0, 0), ImGuiChildFlags_Borders);
      {
        std::lock_guard lock(mutex_);
        for (const auto& line : lines_) {
          ImGui::TextUnformatted(line.c_str());
        }
      }
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
      }
      ImGui::EndChild();
    }
    ImGui::End();
  }

 private:
  void OnEvent(const char* event_name, const rex::system::ModRegistry::EventPayload& payload) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[%s] u64=%llu f64=%.3f bytes=%s", event_name,
                 static_cast<unsigned long long>(payload.u64), payload.f64,
                 DescribeBytes(payload.bytes).c_str());

    std::lock_guard lock(mutex_);
    lines_.emplace_back(buf);
    if (lines_.size() > kMaxLines) {
      lines_.pop_front();
    }
  }

  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;

  std::mutex mutex_;
  std::deque<std::string> lines_;
};

class BusInspectorMod : public rex::system::IModPlugin {
 public:
  explicit BusInspectorMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<BusInspectorDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<BusInspectorDialog> dialog_;
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
  return new BusInspectorMod(ctx->runtime);
}
