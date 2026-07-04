// event_pong mod - event-bus consumer + overlay (F11).
//
// Subscribes to event_ping's "sample.ping" event (see
// mods_src/event_ping/mod_main.cpp) via rex::system::ModRegistry and shows
// the last received ping in an ImGui overlay. `requires = "event_ping"` in
// mod.toml guarantees that mod is enabled and ordered first, though for
// Subscribe (unlike FindAddress) that's not strictly load-bearing: a
// subscriber just needs to register before the *first* Publish, and every
// mod's OnCreateDialogs runs before any guest frame ticks. It's declared
// anyway because this mod has nothing to show without its producer.
//
// Also demonstrates loose coupling: every received ping republishes a couple
// of counters onto the "blackboard" mod's shared key/value store (see
// mods_src/blackboard/mod_main.cpp) via plain Publish calls, with no
// `requires = "blackboard"` and no code dependency on it at all. If
// blackboard isn't enabled, those publishes just have no subscribers and are
// silent no-ops (see ModRegistry::Publish's doc comment).
//
// Threading: Subscribe callbacks fire synchronously on whatever thread calls
// Publish, which for "sample.ping" is event_ping's tick callback, i.e. the
// command-processor thread (see mod_registry.h's DispatchTick comment), never
// the render/UI thread. So the callback here only copies the payload into
// mutex-guarded members; ImGui itself is only ever touched from OnDraw,
// which runs on the UI thread.

#include <rex/system/mod_plugin.h>

#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

class EventPongDialog : public rex::ui::ImGuiDialog {
 public:
  EventPongDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind("bind_event_pong", "F11", "Toggle event pong overlay",
                          [this] { visible_ = !visible_; });

    if (runtime_ && runtime_->mod_registry()) {
      runtime_->mod_registry()->Subscribe("sample.ping", [this](const auto& payload) {
        OnPing(payload);
      });
    }
  }

  ~EventPongDialog() override { rex::ui::UnregisterBind("bind_event_pong"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    uint64_t seq;
    double period;
    std::string message;
    uint64_t received;
    {
      std::lock_guard lock(mutex_);
      seq = last_seq_;
      period = last_period_;
      message = last_message_;
      received = received_;
    }

    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Event Pong", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (received == 0) {
        ImGui::TextDisabled("Waiting for event_ping's first \"sample.ping\"...");
      } else {
        ImGui::Text("Received: %llu", static_cast<unsigned long long>(received));
        ImGui::Text("Last sequence: %llu", static_cast<unsigned long long>(seq));
        ImGui::Text("Approx. period: %.2f s", period);
        ImGui::TextWrapped("Last message: %s", message.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("Also publishing event_pong.count/period to \"blackboard\"");
      }
      ImGui::TextDisabled("mods_src/event_pong - press F11 to close");
    }
    ImGui::End();
  }

 private:
  void OnPing(const rex::system::ModRegistry::EventPayload& payload) {
    std::string message(reinterpret_cast<const char*>(payload.bytes.data()), payload.bytes.size());
    uint64_t received;
    {
      std::lock_guard lock(mutex_);
      last_seq_ = payload.u64;
      last_period_ = payload.f64;
      last_message_ = message;
      received = ++received_;
    }

    // Loose-coupling demo: publish to whatever's listening for
    // "blackboard.set", without knowing or caring whether the blackboard mod
    // is even enabled.
    auto* registry = runtime_ ? runtime_->mod_registry() : nullptr;
    if (!registry) {
      return;
    }
    PublishSet(*registry, "event_pong.count", std::to_string(received));
    PublishSet(*registry, "event_pong.period", std::to_string(payload.f64));
  }

  static void PublishSet(rex::system::ModRegistry& registry, std::string_view key,
                        const std::string& value) {
    std::string entry = std::string(key) + "=" + value;
    rex::system::ModRegistry::EventPayload payload;
    payload.bytes = {reinterpret_cast<const uint8_t*>(entry.data()), entry.size()};
    registry.Publish("blackboard.set", payload);
  }

  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;

  std::mutex mutex_;
  uint64_t last_seq_ = 0;
  double last_period_ = 0.0;
  std::string last_message_;
  uint64_t received_ = 0;
};

class EventPongMod : public rex::system::IModPlugin {
 public:
  explicit EventPongMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<EventPongDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<EventPongDialog> dialog_;
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
  return new EventPongMod(ctx->runtime);
}
