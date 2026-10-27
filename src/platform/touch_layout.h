// nocturnerecomp - ReXGlue Recompiled Project
//
// The on-screen pad's design for this game.
//
// The driver owns the machinery (hit-testing, multi-touch, the resulting
// X_INPUT_STATE) and the overlay the drawing; this is the layout. Symphony of
// the Night is a 2D platformer played on the D-pad, so the pad has a D-pad
// where a stick would be. A free-placed left stick is available through
// touch_movement.

#pragma once

#include "touch_input_driver.h"

#include <algorithm>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>

REXCVAR_DECLARE(double, touch_scale);
REXCVAR_DECLARE(bool, touch_function_keys);

namespace nocturne {

// The buttons behind the SDK's ui_mode_button, which opens the overlays.
inline uint16_t UiModeButtons() {
  const uint16_t mask = rex::ui::ParseGamepadButton(rex::cvar::GetFlagByName("ui_mode_button"));
  return mask ? mask : rex::input::X_INPUT_GAMEPAD_GUIDE;
}

// What the game sees for a real pad's Start or Back. The touch driver skips
// the remap_* table, and the shipped defaults swap the two, so the pills
// resolve it themselves to behave like the controller's buttons.
inline uint16_t RemappedStartOrBack(const char* cvar, uint16_t fallback) {
  const std::string target = rex::cvar::GetFlagByName(cvar);
  if (target == "start")
    return rex::input::X_INPUT_GAMEPAD_START;
  if (target == "back")
    return rex::input::X_INPUT_GAMEPAD_BACK;
  return fallback;
}

// Builds the pad for a surface of the given size.
//
// Pure function of the size, as TouchLayoutProvider requires: the driver calls
// it in window pixels and the overlay calls it in ImGui points, and the two
// only describe the same pad because each rebuilds it in its own space.
inline touch::TouchLayout BuildTouchLayout(float width, float height) {
  using namespace touch;

  TouchLayout layout;
  if (width <= 0.0f || height <= 0.0f)
    return layout;

  // Every size is a fraction of the short side, so the pad keeps its
  // proportions and its reach from the edges on any aspect ratio instead of
  // stretching out with an ultra-wide screen.
  const float u = std::min(width, height);
  const float scale = float(std::clamp(REXCVAR_GET(touch_scale), 0.5, 2.0));

  // Everything is kept this far off the edges. A phone's gesture-nav pill and
  // display cutout live right at them, and a thumb reaching the last few
  // millimetres of glass is both awkward and easy to misread as a system
  // gesture.
  const float inset = u * 0.06f;

  if (rex::cvar::GetFlagByName("touch_movement") == "stick") {
    // Free-placed: the thumb plants it wherever it lands.
    layout.sticks.push_back(MakeStick(/*zone_left=*/0.0f, /*zone_top=*/height * 0.30f,
                                      /*zone_right=*/width * 0.45f, /*zone_bottom=*/height,
                                      /*radius=*/u * 0.16f * scale, TouchAxis::kLeft));
  } else {
    // Eight-way D-pad, bottom left: a diagonal reports both neighbours, which
    // the game needs for crouching attacks and up-diagonal jumps.
    const float dpad_r = u * 0.20f * scale;
    layout.controls.push_back(MakeDpad(inset + dpad_r, height - inset - dpad_r, dpad_r));
  }

  // A/B/X/Y diamond, bottom right, in the Xbox arrangement (A at the bottom).
  const float face_r = u * 0.085f * scale;
  const float arm = u * 0.125f * scale;
  const float fx = width - inset - face_r - arm;
  const float fy = height - inset - face_r - arm;
  layout.controls.push_back(MakeCircle(fx, fy + arm, face_r, "A", X_INPUT_GAMEPAD_A));
  layout.controls.push_back(MakeCircle(fx + arm, fy, face_r, "B", X_INPUT_GAMEPAD_B));
  layout.controls.push_back(MakeCircle(fx - arm, fy, face_r, "X", X_INPUT_GAMEPAD_X));
  layout.controls.push_back(MakeCircle(fx, fy - arm, face_r, "Y", X_INPUT_GAMEPAD_Y));

  // Fast forward sits past the diamond's top right corner, clear of Y and B.
  TouchControl ff = MakeCircle(fx + arm * 1.3f, fy - arm * 1.55f, u * 0.06f * scale, "", 0);
  ff.fast_forward = true;
  layout.controls.push_back(std::move(ff));

  // Shoulders, triggers, Back and Start share one row along the top edge,
  // where no thumb rests during normal play.
  const float pill_rx = u * 0.075f * scale;
  const float pill_ry = u * 0.030f * scale;
  const float pill_y = inset + pill_ry;
  const float margin = inset;
  const float step = pill_rx * 2.0f + u * 0.02f;
  layout.controls.push_back(
      MakePill(margin + pill_rx, pill_y, pill_rx, pill_ry, "LT", 0, TouchTrigger::kLeft));
  layout.controls.push_back(MakePill(margin + pill_rx + step, pill_y, pill_rx, pill_ry, "LB",
                                     X_INPUT_GAMEPAD_LEFT_SHOULDER));
  layout.controls.push_back(MakePill(width - margin - pill_rx, pill_y, pill_rx, pill_ry, "RT", 0,
                                     TouchTrigger::kRight));
  layout.controls.push_back(MakePill(width - margin - pill_rx - step, pill_y, pill_rx, pill_ry,
                                     "RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER));
  // The overlay toggle, Back and Start, the trio centred on the row.
  const float menu_rx = u * 0.09f * scale;
  const float menu_step = menu_rx * 2.0f + u * 0.02f;
  layout.controls.push_back(
      MakePill(width * 0.5f - menu_step, pill_y, menu_rx, pill_ry, "", UiModeButtons()));
  layout.controls.push_back(
      MakePill(width * 0.5f, pill_y, menu_rx, pill_ry, "BACK",
               RemappedStartOrBack("remap_back", X_INPUT_GAMEPAD_BACK)));
  layout.controls.push_back(
      MakePill(width * 0.5f + menu_step, pill_y, menu_rx, pill_ry, "START",
               RemappedStartOrBack("remap_start", X_INPUT_GAMEPAD_START)));

  // Optional second row of square buttons, one per F-key bind, read live.
  if (REXCVAR_GET(touch_function_keys)) {
    const auto keys = touch::TouchFunctionKeys();
    const float side = u * 0.05f * scale;
    const float gap = u * 0.015f;
    const float row_y = pill_y + pill_ry + gap + side;
    const float total = keys.size() * (side * 2.0f + gap) - gap;
    float x = width * 0.5f - total * 0.5f + side;
    for (const auto& key : keys) {
      TouchControl c;
      c.shape = TouchControl::Shape::kSquare;
      c.cx = x;
      c.cy = row_y;
      c.half_width = side;
      c.half_height = side;
      c.label = "F" + std::to_string(key.number);
      c.bind = key.bind;
      layout.controls.push_back(std::move(c));
      x += side * 2.0f + gap;
    }
  }

  return layout;
}

}  // namespace nocturne
