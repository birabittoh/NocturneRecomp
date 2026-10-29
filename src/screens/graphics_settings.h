// nocturnerecomp - cycles the in-game "Change Screen Size..." screen's
// stretch through a few preset percentages via the X button (repurposed --
// see graphics_settings.cpp for why), and relabels that screen's "Default"
// prompt to a localized "Presets".
#pragma once

#include <cstdint>
#include <memory>

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

class GraphicsSettings {
 public:
  GraphicsSettings();
  ~GraphicsSettings();

  // Installs the guest function overrides. Call once runtime() is live
  // (OnPostSetup) -- unlike a mod's OnModuleLaunched, all addresses here are
  // hardcoded rather than looked up via mod_registry, since this is the
  // game's own pinned build, not a third-party mod.
  void Bind(rex::Runtime* runtime);

 private:
  rex::Runtime* runtime_ = nullptr;
};

// Process-wide instance shared between the app hooks.
GraphicsSettings& GetGraphicsSettings();

// ---------------------------------------------------------------------------
// Preset catalog, exposed for the native "Preset" options row
// ---------------------------------------------------------------------------
//
// The stretch presets this file cycles are also driven from the game's own
// settings screen (native_options.cpp's Preset row, which replaced the stock
// "Change Screen Size..." row). That row needs the catalog at compile time --
// hence the constant here rather than an accessor -- and the .cpp
// static_asserts it against its own kPresetCount so the two can't drift.
// (7 stretch presets, plus "Custom" -- see kGraphicsCustomPresetIndex.)
inline constexpr uint32_t kGraphicsPresetCount = 8;

// The last entry: whatever the player last dialled in by hand on the stretch
// screen, when that isn't one of the presets. It only becomes selectable once
// such a stretch exists -- see GraphicsPresetChoiceCount.
inline constexpr uint32_t kGraphicsCustomPresetIndex = kGraphicsPresetCount - 1;

// Display text per preset, index-aligned with the .cpp's kPresetNames. These
// are ASCII-only on purpose: they're painted with the literal-text widget
// setter, which resolves glyphs from the game's own baked atlas (see
// native_options.cpp's ResolutionLabel comment on tofu).
extern const char16_t* const kGraphicsPresetTexts[kGraphicsPresetCount];

// How many of those the player can actually cycle through right now:
// kGraphicsPresetCount once a custom stretch exists, one less until then.
uint32_t GraphicsPresetChoiceCount();

// The preset last applied (or restored from disk at boot), which may be
// kGraphicsCustomPresetIndex.
int32_t GetGraphicsPresetIndex();

// Queues a preset to be applied on the next guest frame. Safe to call from
// any thread and from a context that has no live PPCContext of its own --
// applying touches guest memory and calls a guest function, so the work is
// deferred to GraphicsSettings_PerFrame rather than done here.
void RequestGraphicsPreset(int32_t index);

// Requests the Original/Enhanced graphics style. Safe to call from any
// thread -- deferred to the next GraphicsSettings_PerFrame tick, same as
// RequestGraphicsPreset above. Persists to settings.toml once applied, and
// becomes the value reasserted into guest memory every frame from then on
// (see graphics_settings.cpp's g_pending_style).
void RequestGraphicsStyle(bool enhanced);

}  // namespace nocturne
