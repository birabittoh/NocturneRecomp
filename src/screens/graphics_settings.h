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
// "Change Screen Size..." row) and the F4 overlay's Screen size combo. The
// catalog is the built-in presets, then the ones mods ship in their
// presets/ folder (see docs/making-mods.md), then "Custom" last. It is built
// once, lazily, so a newly enabled mod's presets show up after a restart.
//
// The most the native row reserves room for (built-ins, mods and Custom).
inline constexpr uint32_t kMaxGraphicsPresets = 32;

// Index of the "Custom" entry: whatever the player last dialled in by hand on
// the stretch screen, when that isn't one of the presets. It is the last entry
// of the catalog, and only becomes selectable once such a stretch exists --
// see GraphicsPresetChoiceCount.
uint32_t GraphicsCustomPresetIndex();

// Display text per preset. ASCII-only on purpose: it is painted with the
// literal-text widget setter, which resolves glyphs from the game's own baked
// atlas (see native_options.cpp's ResolutionLabel comment on tofu). Null for an
// index past the catalog.
const char16_t* GraphicsPresetText(uint32_t index);

// How many entries the player can actually cycle through right now: the whole
// catalog once a custom stretch exists, one less until then.
uint32_t GraphicsPresetChoiceCount();

// The preset last applied (or restored from disk at boot), which may be
// GraphicsCustomPresetIndex().
int32_t GetGraphicsPresetIndex();

// Queues a preset to be applied on the next guest frame. Safe to call from
// any thread and from a context that has no live PPCContext of its own --
// applying touches guest memory and calls a guest function, so the work is
// deferred to GraphicsSettings_PerFrame rather than done here.
void RequestGraphicsPreset(int32_t index);

// How far the stretch screen's width/height percentages reach (see the .cpp's
// comment on why they go past the vanilla [1,50]/[1,30]).
inline constexpr int32_t kGraphicsStretchPercentMin = 0;
inline constexpr int32_t kGraphicsStretchPercentMax = 150;

// The live stretch as the stretch screen's width and height percentages,
// worked back from the rect every guest frame. False until the game has set
// up the rect's bounds.
bool GetGraphicsStretchPercent(int32_t& width, int32_t& height);

// Queues a stretch by percentages, applied on the next guest frame like
// RequestGraphicsPreset. A pair matching a preset selects that preset;
// anything else becomes Custom.
void RequestGraphicsStretchPercent(int32_t width, int32_t height);

// Requests the Original/Enhanced graphics style. Safe to call from any
// thread -- deferred to the next GraphicsSettings_PerFrame tick, same as
// RequestGraphicsPreset above. Persists to settings.toml once applied, and
// becomes the value reasserted into guest memory every frame from then on
// (see graphics_settings.cpp's g_pending_style).
void RequestGraphicsStyle(bool enhanced);

}  // namespace nocturne
