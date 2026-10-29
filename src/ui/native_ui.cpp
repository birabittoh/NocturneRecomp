// nocturnerecomp: see native_ui.h.
#include "ui/native_ui.h"

#include <bit>

#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

#include "ui/ui_text.h"

namespace nocturne {

namespace {

constexpr uint32_t kAllocFnAddr = 0x82576950u;           // (size) -> pointer
constexpr uint32_t kSetWidgetTextFnAddr = 0x825D1EE0u;   // (widget, utf16_text)
constexpr uint32_t kSetTextFnAddr = 0x825CEE40u;         // (text_widget, utf16_text)
constexpr uint32_t kSetWidgetScaleFnAddr = 0x825D20C0u;  // (widget, scale), scale in f1
constexpr uint32_t kTextWidthFnAddr = 0x825CF008u;       // (text_widget) -> pixels
constexpr uint32_t kSetWidgetXFnAddr = 0x825D2050u;      // (widget, x)

constexpr uint32_t kWidgetVtableShow = 16;
constexpr uint32_t kWidgetVtableHide = 20;

// Option-list layout: the selected item, the item count, the normal and
// selected item scales (floats), and the item widgets, each a prompt-style
// widget whose inner text widget sits at +548.
constexpr uint32_t kListSelected = 0x218;
constexpr uint32_t kListCount = 0x220;
constexpr uint32_t kListScale = 0x230;
constexpr uint32_t kListSelectedScale = 0x234;
constexpr uint32_t kListItems = 0x288;
constexpr uint32_t kItemInnerText = 0x224;

// sub_825D29A8's centring: x = (0x354 - width) / 2 - 0x6a.
constexpr int32_t kListItemCentreSpan = 0x354;
constexpr int32_t kListItemCentreBias = 0x6a;

uint32_t g_staged_text = 0;

}  // namespace

const NativeText kOffOnTexts[2] = {NativeText{"fullscreen_off"}, NativeText{"fullscreen_on"}};

const char16_t* NativeText::Get() const {
  if (!key) {
    return nullptr;
  }
  return current_only ? TrNativeCurrent(key) : TrNative(key);
}

PPCFunc* GuestFunction(uint32_t address) {
  auto* kernel_state = REX_KERNEL_STATE();
  return kernel_state ? kernel_state->function_dispatcher()->GetFunction(address) : nullptr;
}

uint32_t CallGuest(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3, uint32_t r4,
                   uint32_t r5, uint32_t r6, uint32_t r7) {
  if (!fn) {
    return 0;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = r3;
  ctx.r4.u32 = r4;
  ctx.r5.u32 = r5;
  ctx.r6.u32 = r6;
  ctx.r7.u32 = r7;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

uint32_t StageText(PPCContext& ctx, uint8_t* base, const char16_t* text) {
  if (!text) {
    return 0;
  }
  if (!g_staged_text) {
    g_staged_text = CallGuest(ctx, base, kAllocFnAddr, kStagedTextChars * 2);
    if (!g_staged_text) {
      return 0;
    }
  }
  uint32_t length = 0;
  while (text[length] != u'\0' && length < kStagedTextChars - 1) {
    Write16BE(base, g_staged_text + length * 2, static_cast<uint16_t>(text[length]));
    ++length;
  }
  Write16BE(base, g_staged_text + length * 2, 0);
  return g_staged_text;
}

void SetWidgetText(PPCContext& ctx, uint8_t* base, uint32_t widget, const char16_t* text) {
  const uint32_t staged = widget ? StageText(ctx, base, text) : 0;
  if (staged) {
    CallGuest(ctx, base, kSetWidgetTextFnAddr, widget, staged);
  }
}

void SetTextWidgetText(PPCContext& ctx, uint8_t* base, uint32_t text_widget,
                       const char16_t* text) {
  const uint32_t staged = text_widget ? StageText(ctx, base, text) : 0;
  if (staged) {
    CallGuest(ctx, base, kSetTextFnAddr, text_widget, staged);
  }
}

void SetWidgetScale(PPCContext& ctx, uint8_t* base, uint32_t widget, float scale) {
  PPCFunc* fn = GuestFunction(kSetWidgetScaleFnAddr);
  if (!widget || !fn) {
    return;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = widget;
  ctx.f1.f64 = static_cast<double>(scale);
  fn(ctx, base);
  ctx = saved;
}

void SetWidgetVisible(PPCContext& ctx, uint8_t* base, uint32_t widget, bool visible) {
  const uint32_t vtable = widget ? Read32BE(base, widget) : 0;
  if (!vtable) {
    return;
  }
  const uint32_t fn = Read32BE(base, vtable + (visible ? kWidgetVtableShow : kWidgetVtableHide));
  if (fn) {
    CallGuest(ctx, base, fn, widget);
  }
}

void SetListItemText(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t index,
                     const char16_t* text) {
  if (!list || index >= Read32BE(base, list + kListCount)) {
    return;
  }
  const uint32_t item = Read32BE(base, list + kListItems + index * 4);
  if (!item) {
    return;
  }
  const uint32_t scale_offset =
      index == Read32BE(base, list + kListSelected) ? kListSelectedScale : kListScale;
  SetWidgetText(ctx, base, item, text);
  SetWidgetScale(ctx, base, item, std::bit_cast<float>(Read32BE(base, list + scale_offset)));
  const auto width = static_cast<int32_t>(
      CallGuest(ctx, base, kTextWidthFnAddr, Read32BE(base, item + kItemInnerText)));
  const int32_t x = (kListItemCentreSpan - width) / 2 - kListItemCentreBias;
  CallGuest(ctx, base, kSetWidgetXFnAddr, item, static_cast<uint32_t>(x));
}

}  // namespace nocturne
