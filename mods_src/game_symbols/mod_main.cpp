// game_symbols mod - a "library mod": no UI, no code of its own to react to
// anything, just published guest addresses other mods can depend on instead
// of re-deriving (or copy-pasting) the same reverse-engineered constants.
//
// Registers every known address in OnCreateDialogs (dispatched before
// OnModuleLaunched, and before any consumer's own OnCreateDialogs runs a
// lazy lookup) via rex::system::ModRegistry, reached through
// ctx->runtime->mod_registry(). Consumers add `requires = "game_symbols"` to
// their own mod.toml (see mods_src/ui_color/mod.toml) so the SDK guarantees
// this mod is enabled and ordered first, instead of relying on convention.
//
// See docs/making-mods.md's "Library mods and the shared registry" section.

#include <rex/system/mod_plugin.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>

namespace {

// Guest addresses of the live Settings -> Accent Color struct (three
// consecutive big-endian uint32 fields, R/G/B, each 0-15)
constexpr uint32_t kAccentAddrVanilla = 0x83173CC8u;
constexpr uint32_t kAccentAddrTU = 0x83173A88u;

// Guest address of the live screen-stretch viewport rect: four consecutive
// big-endian uint32 fields (x offset, y offset, width, height). Found via
// the min/max boundary-matrix memory scan documented in the project's
// "live-memory-reverse-engineering-technique" note. Width ranges
// 1052-1280, height 667-720 (this game renders at 1280x720); x/y offset
// are derived by the game as (1280 - width) and (720 - height) -- the rect
// is anchored to the bottom-right corner, not centered. This setting is
// NOT save-file-persisted -- it resets to some default every launch.
constexpr uint32_t kScreenStretchRectAddrVanilla = 0x82882C68u;

// Guest address of the graphics-style *menu selection*, in the same
// Settings screen as the screen-stretch rect above: a single big-endian
// uint32, 0 = "Original", 1 = "Enhanced".
constexpr uint32_t kGraphicsStyleMenuAddrVanilla = 0x828B04A4u;

// Guest address of the *applied* graphics-style flag, 4040 bytes after the
// menu-selection address above: 0 = "Original", 2 = "Enhanced" (not a plain
// bool). Read-only from a mod's perspective -- writing it does not render
// correctly. See mods_src/graphics_settings for the read-only display.
constexpr uint32_t kGraphicsStyleAddrVanilla = 0x828B146Cu;

// Guest address near the graphics-style fields above; idle value 0xFFFFFFFF,
// pulses briefly to 8 when the Settings menu selection changes.
constexpr uint32_t kGraphicsStyleTriggerAddrVanilla = 0x82883038u;

// Guest address of the live player-stats struct (character screen: HP/Heart/
// MP, STR/CON/INT/LCK, level, exp, gold, kills, rooms, playtime).
// Note: STR/CON/INT/LCK are shown consecutively on the character screen, so
// scanning for that exact 4-value big-endian uint32 sequence turned up
// exactly one hit (plus its usual +0x10000000 heap-alias mirror).
//
// All fields are consecutive big-endian uint32_t, offsets from this address:
//   +0x00 hp_current        73
//   +0x04 hp_max            73
//   +0x08 heart_current     56   (consumed by item use)
//   +0x0c heart_max         56
//   +0x10 mp_current        34
//   +0x14 mp_max            34
//   +0x18 str (copy #1)      8
//   +0x1c con (copy #1)      9
//   +0x20 int (copy #1)      7
//   +0x24 lck (copy #1)      9
//   +0x28..0x34 unidentified (all 0 this session)
//   +0x38 str (copy #2)      8
//   +0x3c con (copy #2)      9
//   +0x40 int (copy #2)      7
//   +0x44 lck (copy #2)      9
//   +0x48 level              4
//   +0x4c exp               686
//   +0x50 gold              429
//   +0x54 kills              65
//   +0x58..0x6c unidentified (2, 5, 19, 26, 0, 48 this session -- plausibly
//                             equipped item/slot indices, not confirmed)
//   +0x74 NOT rooms (disproven -- see below), read as 57 this session
//   +0x78 unidentified, also 57 this session (duplicate or coincidence)
//   +0x7c..0x8c unidentified (0, 2, 0, 0, 0 this session)
//   +0x90 playtime_hours      0
//   +0x94 playtime_minutes   22
//   +0x98 playtime_seconds   44
//   +0x9c playtime_frames    20  (sub-second; not shown by the HUD's 00:22:44)
//
// The two STR/CON/INT/LCK copies were identical this session (no equipped
// stat bonus active), so which one is "base" vs. "equipped/effective" isn't
// disambiguated yet -- re-test by equipping a stat-boosting item and seeing
// which copy moves before a mod depends on telling them apart.
//
// NOT found in this struct: "NEXT" (exp remaining to next level, shown on
// the same screen) is presumably computed from a level/exp table rather
// than stored; "STATUS" (ailment indicator, e.g. "GOOD") is presumably
// tracked on a separate actor-state struct. Both need a follow-up
// investigation before a mod can expose them.
//
// +0x74 was originally labeled "rooms" because it happened to read 57 (the
// on-screen ROOMS value) in the first session. Disproven live in a later
// session via mods_src/player_stats's edit mode: writing to +0x74 did NOT
// move the on-screen ROOMS counter at all, but instead caused STR/CON/INT/
// LCK to jump around (+5 at one value, reset to 0 two values later, +1 two
// values after that) -- behavior consistent with indexing into some
// stat-growth/level table (plausibly the *real* level value that drives
// stat computation, as opposed to +0x48 above which may just be a display
// cache), not a simple incrementing room counter. Do NOT treat +0x74 as
// "rooms" or write to it from a mod until it's properly re-identified --
// the real rooms-visited counter is registered separately below, at a
// completely different address (not part of this struct at all).
constexpr uint32_t kPlayerStatsAddrVanilla = 0x83174B7Cu;

// TU address for the same struct: found the same way (scanned the TU
// process for the STR/CON/INT/LCK sequence), confirmed by re-reading the
// full struct at the candidate address and matching every field above
// exactly (playtime differed slightly, as expected for a separately-running
// session -- everything else, including the unidentified/duplicate fields,
// matched). The delta from the vanilla address (0x240) is exactly the same
// vanilla/TU offset as kAccentAddrVanilla/kAccentAddrTU above, which is a
// good independent sanity check that this is the right address rather than
// a coincidental match.
constexpr uint32_t kPlayerStatsAddrTU = 0x8317493Cu;

// Guest address of the real rooms-visited counter, a single big-endian
// uint32_t living well outside the kPlayerStatsAddrVanilla struct above --
// found via the min/max boundary-matrix technique's sibling, an exact value-
// transition scan: snapshotted every guest-range (0x80000000-0x94000000)
// address holding the on-screen ROOMS value (57), asked the user to walk
// into one new room (ROOMS -> 58), rescanned, and intersected the two sets.
// 4210 candidates for "57 somewhere in the guest range" collapsed to exactly
// 2 (this address plus its usual +0x10000000 heap-alias mirror). Confirmed
// live: held steady at 59 across a 5-second gap with no room change (ruling
// out a free-running frame/tick counter that would coincidentally pass the
// same +1 transition test), and matched the user's on-screen ROOMS exactly
// at 57, 58, and 59 in sequence.
//
// Vanilla only so far -- not yet found in the TU build.
constexpr uint32_t kRoomsAddrVanilla = 0x83164CD0u;

// Guest global holding a POINTER to the Application/framework singleton
// object (the `a1` passed to the game's fwmain mode loop / fixed-timestep
// tick, sub_8258B8A0 / sub_8258B3B8). On the pointed-to object: +2236 =
// game_time, +2232 = target_time (the fixed-timestep catch-up loop's own
// clock, units "300ths of a second" -- one 60Hz frame = 5 units). See
// docs/native-renderer-pacing-investigation.md, "full frame-pacing chain
// reverse-engineered", for the full derivation -- verified against
// assets/default.xex (imagebase 0x82000000) and live-probed.
//
// Consumers must not trust this blindly: dereference it, then sanity-check
// the pointed-to struct (see src/fast_forward.cpp's plausibility guard)
// before reading/writing game_time/target_time through it, in case this
// ever runs against a build where the address doesn't hold what's expected.
constexpr uint32_t kAppSingletonPtrAddrVanilla = 0x82E4F808u;

// TU address: the TU relocates the image's .data statics by -0x240 (same
// delta as kAccentAddrVanilla/TU and kPlayerStatsAddrVanilla/TU above).
// Derived from TU codegen output, not guessed from the delta: the TU build
// of the Live-logon screen's Update (TU sub_825B6CB0, vanilla sub_825B6AB0)
// loads this global as lis 0x82E50000 + offset -2616 = 0x82E4F5C8, where
// vanilla uses offset -2040 = 0x82E4F808. See scripts/match_tu_functions.py
// for the vanilla->TU function-matching workflow.
constexpr uint32_t kAppSingletonPtrAddrTU = 0x82E4F5C8u;

class GameSymbolsMod : public rex::system::IModPlugin {
 public:
  explicit GameSymbolsMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* /*drawer*/) override {
    if (runtime_ && runtime_->mod_registry()) {
      runtime_->mod_registry()->RegisterAddress("ui.accent_color", kAccentAddrVanilla,
                                                kAccentAddrTU);
      runtime_->mod_registry()->RegisterAddress("graphics.stretch_rect",
                                                kScreenStretchRectAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("graphics.style",
                                                kGraphicsStyleAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("graphics.style_menu",
                                                kGraphicsStyleMenuAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("graphics.style_trigger",
                                                kGraphicsStyleTriggerAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("player.stats", kPlayerStatsAddrVanilla,
                                                kPlayerStatsAddrTU);
      runtime_->mod_registry()->RegisterAddress("player.rooms", kRoomsAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("app.singleton_ptr",
                                                kAppSingletonPtrAddrVanilla,
                                                kAppSingletonPtrAddrTU);
    }
  }

 private:
  rex::Runtime* runtime_ = nullptr;
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
  return new GameSymbolsMod(ctx->runtime);
}
