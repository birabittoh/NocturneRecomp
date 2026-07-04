// event_ping mod - event-bus producer, no UI.
//
// Registers a per-guest-frame tick via rex::system::ModRegistry and publishes
// a "sample.ping" event roughly once a second. Pairs with event_pong (see
// mods_src/event_pong/mod_main.cpp), which subscribes to this event. Together
// they demonstrate the Subscribe/Publish and RegisterTick/DispatchTick sides
// of the shared mod registry that mods_src/game_symbols/mods_src/ui_color
// don't exercise (those two only use RegisterAddress/FindAddress).
//
// RegisterTick callbacks run on the command-processor thread (see
// DispatchTick's doc comment in mod_registry.h), not the render/UI thread --
// this mod has no UI, so that's a non-issue here, but see event_pong for how
// a *consumer* has to handle it (never touch ImGui from inside a Subscribe
// callback).

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <string>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>

namespace {

// ~60 guest frames per publish; a fixed frame count (rather than wall-clock
// time) keeps this mod's tick callback trivial -- no clock reads needed.
constexpr uint64_t kFramesPerPing = 60;

class EventPingMod : public rex::system::IModPlugin {
 public:
  explicit EventPingMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* /*drawer*/) override {
    if (runtime_ && runtime_->mod_registry()) {
      runtime_->mod_registry()->RegisterTick([this] { OnTick(); });
    }
  }

 private:
  void OnTick() {
    if (++frame_count_ < kFramesPerPing) {
      return;
    }
    frame_count_ = 0;

    // message_ is a member (not a local) so the EventPayload::bytes span
    // stays valid for the whole Publish() call -- Publish invokes every
    // subscriber synchronously and bytes must outlive all of them.
    message_ = "ping #" + std::to_string(seq_);

    rex::system::ModRegistry::EventPayload payload;
    payload.u64 = seq_;
    payload.f64 = kFramesPerPing / 60.0;  // approximate seconds since last ping
    payload.bytes = {reinterpret_cast<const uint8_t*>(message_.data()), message_.size()};

    runtime_->mod_registry()->Publish("sample.ping", payload);
    ++seq_;
  }

  rex::Runtime* runtime_ = nullptr;
  uint64_t frame_count_ = 0;
  uint64_t seq_ = 0;
  std::string message_;
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
  return new EventPingMod(ctx->runtime);
}
