// graphics_settings - cycles the "Change Screen Size..." screen's stretch
// through a few preset percentages via X (repurposed), and relabels
// that screen's "Default" prompt to a localized "Presets".
#include "graphics_settings.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/mod_registry.h>
#include <rex/system/mod_storage.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace nocturne {

namespace {

constexpr uint32_t kStretchXOffset = 560;
constexpr uint32_t kStretchYOffset = 564;

// LTRB offsets from graphics.stretch_rect.
constexpr uint32_t kRectLeftOffset = 0;
constexpr uint32_t kRectTopOffset = 4;
constexpr uint32_t kRectRightOffset = 8;
constexpr uint32_t kRectBottomOffset = 12;

constexpr uint32_t kStretchRectAddr = 0x82882C68u;
constexpr uint32_t kStretchRectMaxAddr = 0x82882C98u;
constexpr uint32_t kStretchRectMinAddr = 0x82882CC8u;
constexpr uint32_t kUiTransitionManagerAddr = 0x82E7A02Cu;
constexpr uint32_t kChangeScreenSizeFnAddr = 0x825BA678u;
constexpr uint32_t kStretchPercentToRectFnAddr = 0x825BB2B0u;
constexpr uint32_t kStretchWidgetRepositionFnAddr = 0x825AB2B0u;
constexpr uint32_t kFixedTimestepTickFnAddr = 0x8258B3B8u;
constexpr uint32_t kSetWidgetTextByIdFnAddr = 0x825CFC68u;

// Named after mods_src/graphics_settings' (the separate ImGui-overlay mod in
// NocturneRecomp-Mods) full preset catalog: PSX Default/Big, 16:10 Default/
// Big/Huge/Extreme, Other Stretched. Solved from that mod's own baseline
// pixel constants (kPsxDefaultWidth/Height etc.), which are really RIGHT/
// BOTTOM rect edges (not rendered width/height; see that mod's Apply()
// comment), inverted through this screen's confirmed converter formula:
// right(x) = 232x/50 + 1048, bottom(y) = 54y/30 + 666 (at 720p; the
// converter itself is resolution-independent, driven by the live
// kStretchRectMax/MinAddr bounds below, not these constants).
constexpr uint32_t kPresetCount = 7;
constexpr uint32_t kPresetX[kPresetCount] = {1, 11, 1, 19, 38, 50, 50};
constexpr uint32_t kPresetY[kPresetCount] = {30, 55, 1, 30, 55, 71, 56};
constexpr const char* kPresetNames[kPresetCount] = {
    "PSX Default", "PSX Big", "16:10 Default", "16:10 Big",
    "16:10 Huge",  "16:10 Extreme", "Stretched"};

constexpr int64_t kToastDurationMs = 2000;

// The localized-string-id menu.set_widget_text_by_id_fn uses for "DEFAULT"
// -- confirmed live by dumping the whole shared string table (base/count
// globals at 0x82E61854/0x82E61858) and matching on content.
constexpr uint32_t kDefaultStringId = 1;
// Offset of a widget's own copied-text buffer from its own base pointer --
// confirmed via kSetWidgetTextByIdFnAddr's decompile (it copies into
// a1+572, up to 2048 bytes, rather than keeping a pointer into the shared
// string table). This is why editing the shared table's entry at runtime
// has no visible effect on any widget that already called this for that id.
constexpr uint32_t kWidgetTextBufferOffset = 572;
// The stretch screen's own widget-construction function (sub_825BADD0,
// confirmed by its direct reference to the literal string "ScreenSize", its
// background asset name) calls menu.set_widget_text_by_id_fn with id==1
// exactly once, from this return address. Each options-submenu screen
// (Graphics/Volume Level/Change Screen Size/one more) has its own dedicated
// widget-construction call rather than sharing one, so filtering on this
// call site scopes the relabel to just this screen -- every other screen
// showing "Default" keeps saying so.
constexpr uint32_t kStretchScreenPromptCallSite = 0x825BB124u;

// Mirrors rex::system::XLanguage's numeric values (see xcontent.h), but only
// the 6 ids settings.cpp's own kLanguageOptions actually offers in the
// in-game Language dropdown (English/Japanese/German/French/Spanish/
// Italian, ids 1-6. Any language id outside the 6 offered by the dropdown
// falls back to English.
enum class XLanguageId : uint32_t {
  kEnglish = 1,
  kJapanese = 2,
  kGerman = 3,
  kFrench = 4,
  kSpanish = 5,
  kItalian = 6,
};

const char16_t* PresetsLabelForCurrentLanguage() {
  switch (static_cast<XLanguageId>(REXCVAR_QUERY(uint32_t, user_language))) {
    case XLanguageId::kItalian:
      return u"PRESET";
    case XLanguageId::kJapanese:
      return u"プリセット";
    case XLanguageId::kEnglish:
    case XLanguageId::kGerman:
    case XLanguageId::kFrench:
    case XLanguageId::kSpanish:
    default:
      return u"PRESETS";
  }
}

PPCFunc* g_original_stretch_fn = nullptr;
PPCFunc* g_original_perframe_fn = nullptr;
PPCFunc* g_original_percent_to_rect_fn = nullptr;
PPCFunc* g_original_set_widget_text_fn = nullptr;
PPCFunc* g_widget_reposition_fn = nullptr;
rex::system::ModRegistry* g_mod_registry = nullptr;

std::atomic<bool> g_cycle_requested{false};
std::atomic<bool> g_screen_open{false};
std::atomic<uint32_t> g_last_a1{0};
uint32_t g_preset_index = 0;

// "Open" is detected as "the stretch screen's handler has fired at least
// once and hasn't seen a close message yet" (word0==0/word2==5) -- this
// screen's real open message didn't match the (0,0,4,0) pattern assumed
// from static RE, but close (0,0,5,0) is confirmed live and reliable.

// Published to mods_src/graphics_settings (the separate ImGui custom-
// resolution overlay mod in NocturneRecomp-Mods, which also writes
// graphics.stretch_rect -- unconditionally, every frame, to keep a custom
// override alive across menus/launches) whenever a preset is applied here,
// carrying the resulting RIGHT/BOTTOM rect edges (packed into the event's
// u64: high 32 bits = right, low 32 bits = bottom -- the exact same units
// that mod's own Apply()/SetOverride() already use, see its header
// comment on the "(2 * edge - max) " edge convention, so no unit
// conversion is needed on either side).
//
// Earlier versions of this file tried to resolve the conflict by telling
// that mod when to stand aside (an open/close event, then an idle
// watchdog on top of it when the open/close latch proved unreliable
// on its own) -- wrong approach: arbitrating *who* writes still leaves two
// independent pieces of state that can disagree. This publishes the
// actual value instead, so the mod can adopt it as its own override; both
// sides then keep reasserting the same number, so there's nothing left to
// race over. Nothing about this event is specific to that one mod; any mod
// wanting to track the live stretch preset can subscribe the same way.
constexpr const char* kPresetAppliedEvent = "graphics_settings.preset_applied";

std::atomic<int> g_toast_preset_index{-1};
std::atomic<int64_t> g_toast_deadline_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t Read32BE(const uint8_t* base, uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(base + guest_address);
}

void Write32BE(uint8_t* base, uint32_t guest_address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(base + guest_address, value);
}

void Write16BE(uint8_t* base, uint32_t guest_address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(base + guest_address, value);
}

void PublishPresetApplied(const uint8_t* base) {
  if (!g_mod_registry) {
    return;
  }
  uint32_t right = Read32BE(base, kStretchRectAddr + kRectRightOffset);
  uint32_t bottom = Read32BE(base, kStretchRectAddr + kRectBottomOffset);
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = (static_cast<uint64_t>(right) << 32) | static_cast<uint64_t>(bottom);
  g_mod_registry->Publish(kPresetAppliedEvent, payload);
}

// Persists the stretch as a fraction of the configured render resolution,
// not raw pixels (the resolution can differ launch to launch, e.g. 720p one
// session, 1080p the next) -- mirrors mods_src/graphics_settings' own
// former persistence exactly (same file, same keys: user_data_root()/
// "mods"/"graphics_settings.cfg", "width_ratio"/"height_ratio"), which this
// now replaces; that mod already read/wrote this same file, so an existing
// player's saved preference carries over with no migration step needed.
std::optional<rex::system::ModStorage> g_storage;

// Set once in Bind() from a persisted ratio, if any; reasserted from every
// GraphicsSettings_PerFrame tick for as long as this stays true. A one-shot
// restore isn't enough -- confirmed live, it wrote successfully (logged) but
// still didn't show up -- and neither was a bounded ~10s reassert window
// (mirroring mods_src/graphics_settings' TryRestoreStyle, which uses the
// same pattern for the graphics-style byte): reasserted correctly and
// continuously for the full 600 ticks with no gaps, right up to the last
// one, and it *still* didn't take. That rules out "something overwrites it
// a few seconds in" -- more likely the boot sequence (company logos, etc.)
// just takes longer than 10s to reach the point where rendering actually
// reads stretch_rect, so the window had already closed by the time it
// mattered. No fixed boot-duration assumption is reliable here, so this
// reasserts unconditionally, indefinitely, exactly like that removed mod's
// own ReassertOverride used to -- until a real X press explicitly takes
// over (see ApplyPendingCycle, which clears this).
std::atomic<bool> g_restoring{false};
bool g_restore_logged = false;
double g_pending_restore_width_ratio = 0.0;
double g_pending_restore_height_ratio = 0.0;

void SavePresetRatios(const uint8_t* base) {
  if (!g_storage) {
    return;
  }
  uint32_t right = Read32BE(base, kStretchRectAddr + kRectRightOffset);
  uint32_t bottom = Read32BE(base, kStretchRectAddr + kRectBottomOffset);
  uint32_t max_w = Read32BE(base, kStretchRectMaxAddr + kRectRightOffset);
  uint32_t max_h = Read32BE(base, kStretchRectMaxAddr + kRectBottomOffset);
  if (max_w == 0 || max_h == 0) {
    return;
  }
  g_storage->SetDouble("width_ratio", static_cast<double>(right) / max_w);
  g_storage->SetDouble("height_ratio", static_cast<double>(bottom) / max_h);
  g_storage->Save();
}

// Applies a persisted ratio directly to the live rect (LEFT/TOP derived as
// max-edge, same convention Apply()/the vanilla converter both use) --
// doesn't go through a screen instance's percent fields like
// ApplyPendingCycle does, since none exists yet this early (no player has
// opened the stretch screen), only the always-live stretch_rect/_max
// globals. Called every frame for as long as g_restoring stays true (see
// GraphicsSettings_PerFrame) rather than just once or a few times -- a no-op
// (returns false) if stretch_rect_max reads as 0/0 is harmless and just
// means "retry next frame."
bool ApplyPendingRestore(uint8_t* base) {
  uint32_t max_w = Read32BE(base, kStretchRectMaxAddr + kRectRightOffset);
  uint32_t max_h = Read32BE(base, kStretchRectMaxAddr + kRectBottomOffset);
  if (max_w == 0 || max_h == 0) {
    return false;
  }
  uint32_t right = static_cast<uint32_t>(std::lround(g_pending_restore_width_ratio * max_w));
  uint32_t bottom = static_cast<uint32_t>(std::lround(g_pending_restore_height_ratio * max_h));
  Write32BE(base, kStretchRectAddr + kRectLeftOffset, max_w - right);
  Write32BE(base, kStretchRectAddr + kRectTopOffset, max_h - bottom);
  Write32BE(base, kStretchRectAddr + kRectRightOffset, right);
  Write32BE(base, kStretchRectAddr + kRectBottomOffset, bottom);
  PublishPresetApplied(base);
  if (!g_restore_logged) {
    g_restore_logged = true;
    REXLOG_INFO("[graphics_settings] restoring persisted stretch (right={}, bottom={})", right,
                bottom);
  }
  return true;
}

// Full replacement for the vanilla percent-to-rect converter -- same
// formula, confirmed via its decompile, minus its two [1,50]/[1,30] clamp
// checks, so writing an out-of-range percent (e.g. >50) correctly
// extrapolates the rect past the native bounds. Real player d-pad input is
// unaffected: the stretch screen's own d-pad handler has a *separate* clamp
// on the percent fields that this doesn't touch.
extern "C" void GraphicsSettings_PercentToRect(PPCContext& ctx, uint8_t* base) {
  uint32_t a1 = static_cast<uint32_t>(ctx.r3.u32);
  int32_t x = static_cast<int32_t>(Read32BE(base, a1 + kStretchXOffset));
  int32_t y = static_cast<int32_t>(Read32BE(base, a1 + kStretchYOffset));

  int32_t max_left = static_cast<int32_t>(Read32BE(base, kStretchRectMaxAddr + kRectLeftOffset));
  int32_t max_top = static_cast<int32_t>(Read32BE(base, kStretchRectMaxAddr + kRectTopOffset));
  int32_t max_right = static_cast<int32_t>(Read32BE(base, kStretchRectMaxAddr + kRectRightOffset));
  int32_t max_bottom =
      static_cast<int32_t>(Read32BE(base, kStretchRectMaxAddr + kRectBottomOffset));
  int32_t min_left = static_cast<int32_t>(Read32BE(base, kStretchRectMinAddr + kRectLeftOffset));
  int32_t min_top = static_cast<int32_t>(Read32BE(base, kStretchRectMinAddr + kRectTopOffset));
  int32_t min_right = static_cast<int32_t>(Read32BE(base, kStretchRectMinAddr + kRectRightOffset));
  int32_t min_bottom =
      static_cast<int32_t>(Read32BE(base, kStretchRectMinAddr + kRectBottomOffset));

  int32_t left = (max_left - min_left) * x / 50 + min_left;
  int32_t right = (max_right - min_right) * x / 50 + min_right;
  int32_t top = (max_top - min_top) * y / 30 + min_top;
  int32_t bottom = (max_bottom - min_bottom) * y / 30 + min_bottom;

  Write32BE(base, kStretchRectAddr + kRectLeftOffset, static_cast<uint32_t>(left));
  Write32BE(base, kStretchRectAddr + kRectTopOffset, static_cast<uint32_t>(top));
  Write32BE(base, kStretchRectAddr + kRectRightOffset, static_cast<uint32_t>(right));
  Write32BE(base, kStretchRectAddr + kRectBottomOffset, static_cast<uint32_t>(bottom));

  if (g_widget_reposition_fn) {
    ctx.r3.u32 = Read32BE(base, kUiTransitionManagerAddr);
    ctx.r4.u32 = static_cast<uint32_t>(x);
    ctx.r5.u32 = static_cast<uint32_t>(y);
    g_widget_reposition_fn(ctx, base);
  }
}

// Applies the next preset using whatever live ctx/base this call was given
// -- shared by both hooks below, since either might notice a pending cycle
// request first.
void ApplyPendingCycle(PPCContext& ctx, uint8_t* base) {
  uint32_t a1 = g_last_a1.load(std::memory_order_acquire);
  if (a1 == 0) {
    return;
  }

  // An explicit player-driven cycle always wins outright, even mid-restore
  // -- otherwise this press could get silently overwritten by the very
  // next reassert tick.
  g_restoring.store(false, std::memory_order_release);

  g_preset_index = (g_preset_index + 1) % kPresetCount;
  uint32_t target_x = kPresetX[g_preset_index];
  uint32_t target_y = kPresetY[g_preset_index];

  Write32BE(base, a1 + kStretchXOffset, target_x);
  Write32BE(base, a1 + kStretchYOffset, target_y);

  ctx.r3.u32 = a1;
  GraphicsSettings_PercentToRect(ctx, base);
  PublishPresetApplied(base);
  SavePresetRatios(base);

  g_toast_preset_index.store(static_cast<int>(g_preset_index), std::memory_order_release);
  g_toast_deadline_ms.store(NowMs() + kToastDurationMs, std::memory_order_release);
}

// Hooks the stretch screen itself: tracks the live a1 and open/close state,
// and repurposes X (msg2==6) to cycle instead of resetting to the graphics
// style's default.
extern "C" void GraphicsSettings_StretchScreen(PPCContext& ctx, uint8_t* base) {
  uint32_t a1 = static_cast<uint32_t>(ctx.r3.u32);
  uint32_t a2 = static_cast<uint32_t>(ctx.r4.u32);

  if (a1 != 0) {
    g_last_a1.store(a1, std::memory_order_release);
    g_screen_open.store(true, std::memory_order_release);
  }

  uint32_t msg0 = Read32BE(base, a2 + 0);
  uint32_t msg2 = Read32BE(base, a2 + 8);
  if (msg0 == 0 && msg2 == 5) {
    g_screen_open.store(false, std::memory_order_release);
  }

  bool suppress_default_reset = false;
  if (msg0 == 0 && msg2 == 6) {
    g_cycle_requested.store(true, std::memory_order_release);
    Write32BE(base, a2 + 8, 0xFFFFFFFFu);
    suppress_default_reset = true;
  }

  if (a1 != 0 && g_cycle_requested.exchange(false)) {
    PPCContext saved = ctx;
    ApplyPendingCycle(ctx, base);
    ctx = saved;
  }

  if (g_original_stretch_fn) {
    g_original_stretch_fn(ctx, base);
  }

  if (suppress_default_reset) {
    Write32BE(base, a2 + 8, 6);
  }
}

// Hooks a guaranteed-every-frame guest function purely to get a live, valid
// PPCContext without waiting for the player to press a button.
extern "C" void GraphicsSettings_PerFrame(PPCContext& ctx, uint8_t* base) {
  if (g_restoring.load(std::memory_order_acquire)) {
    ApplyPendingRestore(base);
  }

  if (g_screen_open.load(std::memory_order_acquire) && g_cycle_requested.exchange(false)) {
    PPCContext saved = ctx;
    ApplyPendingCycle(ctx, base);
    ctx = saved;
  }

  if (g_original_perframe_fn) {
    g_original_perframe_fn(ctx, base);
  }
}

// Wraps menu.set_widget_text_by_id_fn: calls through to the vanilla
// copy-string-by-id-into-widget-buffer behavior first, then, if the id was
// kDefaultStringId ("DEFAULT") AND the call came from the stretch screen's
// own constructor, overwrites the widget's own just-copied buffer with a
// language-appropriate "PRESETS"/"PRESET"/プリセット label. All are shorter
// than or equal to "DEFAULT"'s own length, so this can never overrun the
// widget's 2048-byte buffer or need to touch any length field -- just
// writes its own NUL terminator.
extern "C" void GraphicsSettings_SetWidgetText(PPCContext& ctx, uint8_t* base) {
  uint32_t a1 = static_cast<uint32_t>(ctx.r3.u32);
  uint32_t a2 = static_cast<uint32_t>(ctx.r4.u32);
  uint32_t lr = static_cast<uint32_t>(ctx.lr);

  if (g_original_set_widget_text_fn) {
    g_original_set_widget_text_fn(ctx, base);
  }

  if (a1 != 0 && a2 == kDefaultStringId && lr == kStretchScreenPromptCallSite) {
    uint32_t text_addr = a1 + kWidgetTextBufferOffset;
    const char16_t* label = PresetsLabelForCurrentLanguage();
    size_t len = std::char_traits<char16_t>::length(label) + 1;  // include NUL
    for (size_t i = 0; i < len; ++i) {
      Write16BE(base, text_addr + static_cast<uint32_t>(i * 2), static_cast<uint16_t>(label[i]));
    }
  }
}

}  // namespace

class GraphicsSettingsToastDialog : public rex::ui::ImGuiDialog {
 public:
  explicit GraphicsSettingsToastDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    int idx = g_toast_preset_index.load(std::memory_order_acquire);
    if (idx < 0 || idx >= static_cast<int>(kPresetCount)) {
      return;
    }
    if (NowMs() >= g_toast_deadline_ms.load(std::memory_order_acquire)) {
      return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20.0f, vp->WorkPos.y + 20.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##graphics_settings_toast", nullptr, flags)) {
      ImGui::Text("Resolution preset: %s", kPresetNames[idx]);
    }
    ImGui::End();
  }
};

GraphicsSettings::GraphicsSettings() = default;
GraphicsSettings::~GraphicsSettings() = default;

void GraphicsSettings::Bind(rex::Runtime* runtime) {
  runtime_ = runtime;
  if (!runtime_ || !runtime_->function_dispatcher()) {
    return;
  }
  auto* dispatcher = runtime_->function_dispatcher();
  g_mod_registry = runtime_->mod_registry();

  g_storage.emplace(runtime_->user_data_root() / "mods" / "graphics_settings.cfg");
  g_storage->Load();
  auto width_ratio = g_storage->GetDouble("width_ratio");
  auto height_ratio = g_storage->GetDouble("height_ratio");
  if (width_ratio && height_ratio && *width_ratio > 0.0 && *height_ratio > 0.0) {
    g_pending_restore_width_ratio = *width_ratio;
    g_pending_restore_height_ratio = *height_ratio;
    g_restoring.store(true, std::memory_order_release);
  }

  if (!dispatcher->OverrideFunction(kStretchPercentToRectFnAddr,
                                    &GraphicsSettings_PercentToRect,
                                    &g_original_percent_to_rect_fn)) {
    REXLOG_WARN("[graphics_settings] OverrideFunction failed for {:08X} (percent-to-rect); "
                "presets will stay clamped to the native range",
                kStretchPercentToRectFnAddr);
  }
  g_widget_reposition_fn = dispatcher->GetFunction(kStretchWidgetRepositionFnAddr);

  if (!dispatcher->OverrideFunction(kChangeScreenSizeFnAddr, &GraphicsSettings_StretchScreen,
                                    &g_original_stretch_fn)) {
    REXLOG_WARN("[graphics_settings] OverrideFunction failed for {:08X} (stretch screen)",
                kChangeScreenSizeFnAddr);
  }

  if (!dispatcher->OverrideFunction(kFixedTimestepTickFnAddr, &GraphicsSettings_PerFrame,
                                    &g_original_perframe_fn)) {
    REXLOG_WARN("[graphics_settings] OverrideFunction failed for {:08X} (per-frame); cycling "
                "will still work but only on the next real button press",
                kFixedTimestepTickFnAddr);
  }

  if (!dispatcher->OverrideFunction(kSetWidgetTextByIdFnAddr, &GraphicsSettings_SetWidgetText,
                                    &g_original_set_widget_text_fn)) {
    REXLOG_WARN("[graphics_settings] OverrideFunction failed for {:08X} (set-widget-text); the "
                "X prompt will keep saying \"Default\"",
                kSetWidgetTextByIdFnAddr);
  }

  REXLOG_INFO("[graphics_settings] hooks installed");
}

void GraphicsSettings::AttachWatcher(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !toast_dialog_) {
    toast_dialog_ = std::make_unique<GraphicsSettingsToastDialog>(drawer);
  }
}

GraphicsSettings& GetGraphicsSettings() {
  static GraphicsSettings instance;
  return instance;
}

}  // namespace nocturne
