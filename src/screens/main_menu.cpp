// nocturnerecomp: see main_menu.h.
#include "screens/main_menu.h"

#include <string>

#include <rex/ppc/context.h>

#include "core/function_hooks.h"
#include "core/game_state.h"
#include "screens/native_options.h"
#include "ui/native_ui.h"
#include "rando/rando.h"
#include "rando/rando_runtime.h"
#include "ui/ui_text.h"

namespace nocturne {

namespace {

// --- The main menu (front-end page 1) ------------------------------------------
//
// Its builder (sub_825C5400) sets up a six-item menu list
// (sub_825D3AE0(list, 0, 180, 6)) and texts the items by id with
// sub_825D3DB8. Its Activate (sub_825C5880) re-texts item 4 and shows or
// hides item 5 for the full game or the trial. Its event handler
// (sub_825C4FA8) acts on an "item chosen" message by the item's index, and
// its update (sub_825C5A08) greys Leaderboards (item 1) out in a two-player
// mode and sets the description panel from the selected item. All three lay
// the list out with sub_825CAA30(list, 180).
constexpr uint32_t kBuildFnAddr = 0x825C5400u;     // (screen)
constexpr uint32_t kActivateFnAddr = 0x825C5880u;  // (screen, ...)
constexpr uint32_t kEventFnAddr = 0x825C4FA8u;     // (screen, message)
constexpr uint32_t kUpdateFnAddr = 0x825C5A08u;    // (screen, delta)
// The return address of the builder's call to the shared list setup.
constexpr uint32_t kBuildListSetupLr = 0x825C5508u;

constexpr uint32_t kMenuItemTextByIdFnAddr = 0x825D3DB8u;  // (list, index, string_id)
constexpr uint32_t kSetTextByIdFnAddr = 0x825CFC68u;       // (text_widget, string_id)
// The list's per-item enable and disable, its vtable slots +60 and +56. An
// enabled item shows and can be selected; a disabled one is greyed out, or
// hidden altogether with `hide`.
constexpr uint32_t kListEnableItemFnAddr = 0x825D2BB0u;   // (list, index)
constexpr uint32_t kListDisableItemFnAddr = 0x825D4550u;  // (list, index, hide)

constexpr uint32_t kScreenList = 0x224;
constexpr uint32_t kScreenDescription = 0x22C;
constexpr uint32_t kListSelected = 0x218;
constexpr uint32_t kListItemEnabled = 0x248;  // one byte per item

constexpr uint32_t kStockItemCount = 6;
constexpr uint32_t kRandomizerItem = 1;
// Every stock item from Leaderboards on is one further down.
constexpr uint32_t kFirstMovedItem = 2;
// The two slots the Activate picks between, in our numbering.
constexpr uint32_t kUnlockItem = 5;
constexpr uint32_t kLastItem = 6;

// The stock items' text and description ids, in our numbering.
constexpr uint32_t kLeaderboardsStringId = 72;
constexpr uint32_t kAchievementsStringId = 33;
constexpr uint32_t kHelpOptionsStringId = 13;
constexpr uint32_t kUnlockStringId = 20;
constexpr uint32_t kQuitStringId = 31;
constexpr uint32_t kLeaderboardsDescriptionId = 46;
constexpr uint32_t kAchievementsDescriptionId = 47;
constexpr uint32_t kHelpOptionsDescriptionId = 45;
constexpr uint32_t kQuitDescriptionId = 48;

// A screen message: word 0 the class, word 2 the item, word 3 the controller.
// Class 1 is "list item chosen".
constexpr uint32_t kMsgClass = 0;
constexpr uint32_t kMsgValue = 8;
constexpr uint32_t kMsgController = 12;
constexpr uint32_t kMsgClassItemChosen = 1;
// An item the stock handler has no case for.
constexpr uint32_t kNoStockItem = 0xFF;

// Opening the settings screen, as Help & Options' Settings item does: page
// 11 with a profile signed in (the app object's vtable slot 108, asked about
// the controller), else the sign-in screen (page 8) with 3, which goes on to
// it.
constexpr uint32_t kPostEventFnAddr = 0x825CE8E8u;  // (class, arg1, page, controller)
constexpr uint32_t kEventClassPageSwitch = 8;
constexpr uint32_t kSettingsPage = 11;
constexpr uint32_t kSignInPage = 8;
constexpr uint32_t kSignInThenSettings = 3;
constexpr uint32_t kAppSignedInVtableSlot = 108;

// --- Text -------------------------------------------------------------------------

constexpr NativeText kItemText{"rando_item"};
constexpr NativeText kDescription{"rando_description"};
constexpr NativeText kActiveSeed{"rando_active_seed"};
constexpr NativeText kGenerating{"rando_generating"};
constexpr NativeText kFailed{"rando_failed"};
constexpr NativeText kUnavailable{"rando_unavailable"};

// The list once the builder has made it.
uint32_t g_list = 0;

// The description panel's text with the Randomizer item selected.
const char16_t* DescriptionText() {
  const auto status = rando::Generator::Get().status();
  const auto runtime = GetRandoRuntime().state();
  if (!runtime.supported) {
    return kUnavailable.Get();
  }
  if (status == rando::Status::kRunning || runtime.request_pending) {
    return kGenerating.Get();
  }
  if (status == rando::Status::kFailed) {
    return kFailed.Get();
  }
  if (!runtime.new_game) {
    return kDescription.Get();
  }
  static std::u16string text;
  const char16_t* format = kActiveSeed.Get();
  text = format ? format : u"";
  const size_t at = text.find(u"{}");
  if (at != std::u16string::npos) {
    text.replace(at, 2, Utf8ToUtf16(runtime.new_game->options.seed));
  }
  return text.c_str();
}

uint32_t ReadWord(const uint8_t* base, uint32_t address) { return Read32BE(base, address); }

bool ItemEnabled(const uint8_t* base, uint32_t index) {
  return base[g_list + kListItemEnabled + index] != 0;
}

}  // namespace

void AdjustMainMenuListSetup(PPCContext& ctx) {
  if (static_cast<uint32_t>(ctx.lr) == kBuildListSetupLr && ctx.r6.u32 == kStockItemCount) {
    g_list = ctx.r3.u32;
    ctx.r6.u32 = kStockItemCount + 1;
  }
}

// --- Hooks ----------------------------------------------------------------------

// Every stock item from Leaderboards on moves down one, keeping its text, and
// ours takes Leaderboards' place. The by-id calls go through the language
// switch's hook like the builder's own, so a switch re-texts them.
extern "C" void MainMenu_Build(PPCContext& ctx, uint8_t* base) {
  fnhook::CallOriginal(kBuildFnAddr, ctx, base);
  if (!g_list) {
    return;
  }
  CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, 2, kLeaderboardsStringId);
  CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, 3, kAchievementsStringId);
  CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, 4, kHelpOptionsStringId);
  CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, kUnlockItem, kUnlockStringId);
  CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, kLastItem, kQuitStringId);
  SetListItemText(ctx, base, g_list, kRandomizerItem, kItemText.Get());
}

// The stock Activate keeps one of its items 4 and 5: Quit on item 4 with item
// 5 hidden in the full game, Unlock and Quit in the trial. It is told which by
// item 5 being shown, and the same choice is made one item further down.
extern "C" void MainMenu_Activate(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  fnhook::CallOriginal(kActivateFnAddr, ctx, base);
  if (!g_list || !screen || ReadWord(base, screen + kScreenList) != g_list) {
    return;
  }
  const bool trial = ItemEnabled(base, kUnlockItem);
  CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, 4, kHelpOptionsStringId);
  if (trial) {
    CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, kUnlockItem, kUnlockStringId);
    CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, kLastItem, kQuitStringId);
    CallGuest(ctx, base, kListEnableItemFnAddr, g_list, kLastItem);
  } else {
    CallGuest(ctx, base, kMenuItemTextByIdFnAddr, g_list, kUnlockItem, kQuitStringId);
    CallGuest(ctx, base, kListEnableItemFnAddr, g_list, kUnlockItem);
    CallGuest(ctx, base, kListDisableItemFnAddr, g_list, kLastItem, 1);
  }
}

// Ours opens the randomizer pages; the stock items get their own index back.
extern "C" void MainMenu_Event(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t message = ctx.r4.u32;
  const bool chosen = message && g_list && screen &&
                      ReadWord(base, screen + kScreenList) == g_list &&
                      ReadWord(base, message + kMsgClass) == kMsgClassItemChosen;
  const uint32_t item = chosen ? ReadWord(base, message + kMsgValue) : 0;

  uint32_t stock_item = item;
  if (chosen && item == kRandomizerItem) {
    stock_item = kNoStockItem;
    const uint32_t controller = ReadWord(base, message + kMsgController);
    const uint32_t app = ReadWord(base, kAppObjectPtrAddr);
    const uint32_t signed_in_fn =
        app ? ReadWord(base, ReadWord(base, app) + kAppSignedInVtableSlot) : 0;
    const bool signed_in =
        signed_in_fn && (CallGuest(ctx, base, signed_in_fn, app, controller) & 0xFF) != 0;
    SelectSettingsPage(SettingsPage::kRandomizer);
    if (signed_in) {
      CallGuest(ctx, base, kPostEventFnAddr, kEventClassPageSwitch, 0, kSettingsPage, controller);
    } else {
      CallGuest(ctx, base, kPostEventFnAddr, kEventClassPageSwitch, kSignInThenSettings,
                kSignInPage, controller);
    }
  } else if (chosen && item >= kFirstMovedItem) {
    stock_item = item - 1;
  }

  if (stock_item != item) {
    Write32BE(base, message + kMsgValue, stock_item);
  }
  fnhook::CallOriginal(kEventFnAddr, ctx, base);
  if (stock_item != item) {
    Write32BE(base, message + kMsgValue, item);
  }
}

// The stock update greys out item 1 for Leaderboards, and texts the panel by
// the selected index, so both are redone for where the items are now.
extern "C" void MainMenu_Update(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  fnhook::CallOriginal(kUpdateFnAddr, ctx, base);
  if (!g_list || !screen || ReadWord(base, screen + kScreenList) != g_list) {
    return;
  }

  // Only in a two-player mode, so ours is normally left as the stock update
  // leaves it: enabled, every frame, as it does Leaderboards.
  if (!ItemEnabled(base, kRandomizerItem)) {
    CallGuest(ctx, base, kListEnableItemFnAddr, g_list, kRandomizerItem);
    CallGuest(ctx, base, kListDisableItemFnAddr, g_list, kFirstMovedItem, 0);
  }

  const uint32_t description = ReadWord(base, screen + kScreenDescription);
  if (!description) {
    return;
  }
  const uint32_t selected = ReadWord(base, g_list + kListSelected);
  uint32_t id = 0;
  switch (selected) {
    case kRandomizerItem:
      // Keeps the scale Leaderboards' description just gave the panel, which
      // is the one every description has.
      SetTextWidgetText(ctx, base, description, DescriptionText());
      return;
    case 2:
      id = kLeaderboardsDescriptionId;
      break;
    case 3:
      id = kAchievementsDescriptionId;
      break;
    case 4:
      id = kHelpOptionsDescriptionId;
      break;
    case kUnlockItem:
      id = ItemEnabled(base, kLastItem) ? kUnlockStringId : kQuitDescriptionId;
      break;
    case kLastItem:
      id = kQuitDescriptionId;
      break;
    default:
      return;
  }
  CallGuest(ctx, base, kSetTextByIdFnAddr, description, id);
}

void RefreshMainMenuText(PPCContext& ctx, uint8_t* base) {
  SetListItemText(ctx, base, g_list, kRandomizerItem, kItemText.Get());
}

}  // namespace nocturne

// Entry hooks, wired up in config/nocturnerecomp_config.toml.
bool MainMenu_BuildHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kBuildFnAddr, &nocturne::MainMenu_Build);
}
bool MainMenu_ActivateHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kActivateFnAddr, &nocturne::MainMenu_Activate);
}
bool MainMenu_EventHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kEventFnAddr, &nocturne::MainMenu_Event);
}
bool MainMenu_UpdateHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kUpdateFnAddr, &nocturne::MainMenu_Update);
}
