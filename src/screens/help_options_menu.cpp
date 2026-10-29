// nocturnerecomp: see help_options_menu.h.
#include "screens/help_options_menu.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "core/function_hooks.h"
#include "screens/main_menu.h"
#include "screens/native_options.h"
#include "screens/native_options_page.h"
#include "ui/native_ui.h"

namespace nocturne {

namespace {

// --- Main-menu Help & Options (front-end page 4) ------------------------------
//
// Its builder (sub_825C0230) sets up a menu list of four items
// (sub_825D3AE0(list, x, y, count)) and texts them by id with sub_825D3DB8:
// How to Play, Controls, Settings, Credits. Its event handler (sub_825C00C8)
// opens a page per item on an "item chosen" message, and its update
// (sub_825C05F8) sets the description panel from the selected item.
constexpr uint32_t kMainBuildFnAddr = 0x825C0230u;    // (screen)
constexpr uint32_t kMainBuildFnSize = 0x270u;
constexpr uint32_t kMainEventFnAddr = 0x825C00C8u;    // (screen, message)
constexpr uint32_t kMainUpdateFnAddr = 0x825C05F8u;   // (screen, delta)
constexpr uint32_t kMenuListSetupFnAddr = 0x825D3AE0u;  // (list, x, y, count)
constexpr uint32_t kMenuItemTextByIdFnAddr = 0x825D3DB8u;  // (list, index, string_id)
constexpr uint32_t kSetTextByIdFnAddr = 0x825CFC68u;  // (text_widget, string_id)

constexpr uint32_t kMainScreenList = 0x218;
constexpr uint32_t kMainScreenDescription = 0x21C;
constexpr uint32_t kMainStockItemCount = 4;

// The main menu's items, before and after ours. Credits keeps its string ids
// (item text and description) and moves down one.
constexpr uint32_t kSettingsItem = 2;
constexpr uint32_t kEnhancementsItem = 3;
constexpr uint32_t kCreditsItem = 4;
constexpr uint32_t kStockCreditsItem = 3;
constexpr uint32_t kCreditsStringId = 0x4E;
constexpr uint32_t kCreditsDescriptionStringId = 0x3F;

// A screen message: word 0 the class, word 1 its source, word 2 the item or
// button. Class 1 is "list item chosen", from the list in word 1; class 0 a
// button press, 4 being A.
constexpr uint32_t kMsgClass = 0;
constexpr uint32_t kMsgSource = 4;
constexpr uint32_t kMsgValue = 8;
constexpr uint32_t kMsgClassButton = 0;
constexpr uint32_t kMsgClassItemChosen = 1;
constexpr uint32_t kButtonAccept = 4;

// --- Pause-menu Help & Options (front-end page 18) ----------------------------
//
// Its Activate (sub_825B32F8) texts the items of a five-slot list by id with
// sub_825D29A8 and fills an action table alongside, one word per item: 0 How
// to Play, 1 Controls, 2 Settings, 4 Credits (3, a graphics toggle, goes
// unused). It shows the first four items and hides the fifth. Its event
// handler (sub_825B2F18) switches on the selected item's action on A.
constexpr uint32_t kPauseActivateFnAddr = 0x825B32F8u;  // (screen, ?, user_index)
constexpr uint32_t kPauseEventFnAddr = 0x825B2F18u;     // (screen, message)
constexpr uint32_t kListItemTextByIdFnAddr = 0x825D29A8u;  // (list, index, string_id)
constexpr uint32_t kListShowItemFnAddr = 0x825D2BB0u;   // (list, index)

constexpr uint32_t kPauseScreenList = 0x220;
constexpr uint32_t kPauseScreenActions = 0x22C;
constexpr uint32_t kPauseCreditsStringId = 0x112;

constexpr uint32_t kActionSettings = 2;
constexpr uint32_t kActionCredits = 4;
// Ours: a value the handler's switch has no case for, so nothing happens if
// the handler ever sees it.
constexpr uint32_t kActionEnhancements = 5;

constexpr uint32_t kListSelected = 0x218;

// --- Text -----------------------------------------------------------------------

// Title case, like the main menu's other items.
constexpr NativeText kItemText{"enhancements_item"};

// The description panel's text for the item, in the style of the others
// ("Customize game settings").
constexpr NativeText kItemDescription{"enhancements_description"};

// The pause menu's items are in caps, like the screen's title, so they share
// the enhancements page's.
const char16_t* PauseItemText() {
  return EnhancementsMenuText();
}

// The lists, once their screens have been built, for the language refresh.
uint32_t g_main_list = 0;
uint32_t g_pause_list = 0;

uint32_t ReadWord(const uint8_t* base, uint32_t address) { return Read32BE(base, address); }

}  // namespace

// --- Hooks ----------------------------------------------------------------------

// One more item on the main menu's list. The setup is shared by every menu
// list, so only the Help & Options builder's own call is touched, and the
// main menu's (main_menu.cpp), which gets one more too.
extern "C" void HelpOptions_MenuListSetup(PPCContext& ctx, uint8_t* base) {
  const auto lr = static_cast<uint32_t>(ctx.lr);
  if (lr > kMainBuildFnAddr && lr <= kMainBuildFnAddr + kMainBuildFnSize &&
      ctx.r6.u32 == kMainStockItemCount) {
    ctx.r6.u32 = kMainStockItemCount + 1;
  }
  AdjustMainMenuListSetup(ctx);
  fnhook::CallOriginal(kMenuListSetupFnAddr, ctx, base);
}

// Credits moves down to the new last item, and ours takes its place. The by-id
// call goes through the language switch's hook like the builder's own, so a
// switch re-texts it.
extern "C" void HelpOptions_MainBuild(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  fnhook::CallOriginal(kMainBuildFnAddr, ctx, base);

  const uint32_t list = screen ? ReadWord(base, screen + kMainScreenList) : 0;
  if (!list) {
    return;
  }
  g_main_list = list;
  CallGuest(ctx, base, kMenuItemTextByIdFnAddr, list, kCreditsItem, kCreditsStringId);
  SetListItemText(ctx, base, list, kEnhancementsItem, kItemText.Get());
}

// Hands the stock handler the Settings item for ours, after picking the page,
// and the old Credits index for Credits.
extern "C" void HelpOptions_MainEvent(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t message = ctx.r4.u32;
  const uint32_t list = screen ? ReadWord(base, screen + kMainScreenList) : 0;
  const bool chosen = message && list &&
                      ReadWord(base, message + kMsgClass) == kMsgClassItemChosen &&
                      ReadWord(base, message + kMsgSource) == list;
  const uint32_t item = chosen ? ReadWord(base, message + kMsgValue) : 0;

  uint32_t stock_item = item;
  if (chosen && item == kSettingsItem) {
    SelectSettingsPage(SettingsPage::kSettings);
  } else if (chosen && item == kEnhancementsItem) {
    SelectSettingsPage(SettingsPage::kEnhancements);
    stock_item = kSettingsItem;
  } else if (chosen && item == kCreditsItem) {
    stock_item = kStockCreditsItem;
  }

  if (stock_item != item) {
    Write32BE(base, message + kMsgValue, stock_item);
  }
  fnhook::CallOriginal(kMainEventFnAddr, ctx, base);
  if (stock_item != item) {
    Write32BE(base, message + kMsgValue, item);
  }
}

// The stock update texts the panel for items 0-3 and blanks it for anything
// else, so ours (3) got Credits' description and Credits (4) none.
extern "C" void HelpOptions_MainUpdate(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  fnhook::CallOriginal(kMainUpdateFnAddr, ctx, base);

  const uint32_t list = screen ? ReadWord(base, screen + kMainScreenList) : 0;
  const uint32_t description = screen ? ReadWord(base, screen + kMainScreenDescription) : 0;
  if (!list || !description) {
    return;
  }
  const uint32_t selected = ReadWord(base, list + kListSelected);
  if (selected == kEnhancementsItem) {
    // Keeps the scale Credits' entry just gave the panel, which is the one
    // every description has.
    SetTextWidgetText(ctx, base, description, kItemDescription.Get());
  } else if (selected == kCreditsItem) {
    CallGuest(ctx, base, kSetTextByIdFnAddr, description, kCreditsDescriptionStringId);
  }
}

// Shows the fifth slot, as Credits, with ours where Credits was.
extern "C" void HelpOptions_PauseActivate(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  fnhook::CallOriginal(kPauseActivateFnAddr, ctx, base);

  const uint32_t list = screen ? ReadWord(base, screen + kPauseScreenList) : 0;
  if (!list) {
    return;
  }
  g_pause_list = list;
  Write32BE(base, screen + kPauseScreenActions + 4 * kEnhancementsItem, kActionEnhancements);
  Write32BE(base, screen + kPauseScreenActions + 4 * kCreditsItem, kActionCredits);
  CallGuest(ctx, base, kListShowItemFnAddr, list, kCreditsItem);
  CallGuest(ctx, base, kListItemTextByIdFnAddr, list, kCreditsItem, kPauseCreditsStringId);
  SetListItemText(ctx, base, list, kEnhancementsItem, PauseItemText());
}

// As HelpOptions_MainEvent, through the action table: ours runs the Settings
// action for the one press.
extern "C" void HelpOptions_PauseEvent(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t message = ctx.r4.u32;
  const uint32_t list = screen ? ReadWord(base, screen + kPauseScreenList) : 0;
  const bool accept = message && list && ReadWord(base, message + kMsgClass) == kMsgClassButton &&
                      ReadWord(base, message + kMsgValue) == kButtonAccept;
  const uint32_t action_address =
      accept ? screen + kPauseScreenActions + 4 * ReadWord(base, list + kListSelected) : 0;
  const uint32_t action = accept ? ReadWord(base, action_address) : 0;

  if (accept && action == kActionSettings) {
    SelectSettingsPage(SettingsPage::kSettings);
  } else if (accept && action == kActionEnhancements) {
    SelectSettingsPage(SettingsPage::kEnhancements);
    Write32BE(base, action_address, kActionSettings);
  }
  fnhook::CallOriginal(kPauseEventFnAddr, ctx, base);
  if (accept && action == kActionEnhancements) {
    Write32BE(base, action_address, kActionEnhancements);
  }
}

void RefreshHelpOptionsText(PPCContext& ctx, uint8_t* base) {
  SetListItemText(ctx, base, g_main_list, kEnhancementsItem, kItemText.Get());
  SetListItemText(ctx, base, g_pause_list, kEnhancementsItem, PauseItemText());
}

}  // namespace nocturne

// Entry hooks, wired up in config/nocturnerecomp_config.toml.
bool HelpOptions_MenuListSetupHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kMenuListSetupFnAddr,
                                 &nocturne::HelpOptions_MenuListSetup);
}
bool HelpOptions_MainBuildHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kMainBuildFnAddr, &nocturne::HelpOptions_MainBuild);
}
bool HelpOptions_MainEventHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kMainEventFnAddr, &nocturne::HelpOptions_MainEvent);
}
bool HelpOptions_MainUpdateHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kMainUpdateFnAddr,
                                 &nocturne::HelpOptions_MainUpdate);
}
bool HelpOptions_PauseActivateHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kPauseActivateFnAddr,
                                 &nocturne::HelpOptions_PauseActivate);
}
bool HelpOptions_PauseEventHook(PPCRegister& r3) {
  return nocturne::fnhook::Enter(r3, nocturne::kPauseEventFnAddr,
                                 &nocturne::HelpOptions_PauseEvent);
}
