/**
 * @file        touch_controls_overlay.h
 * @brief       Draws the on-screen touch pad fed by TouchInputDriver.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include "touch_input_driver.h"
#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>

namespace nocturne::touch {

/**
 * Paints the virtual pad over the game.
 *
 * Purely a view: hit-testing and the resulting controller state live in
 * TouchInputDriver, which owns them because it has to answer GetState from
 * guest threads whether or not a frame is being drawn.
 */
class TouchControlsOverlay final : public rex::ui::ImGuiDialog {
 public:
  TouchControlsOverlay(rex::ui::ImGuiDrawer* drawer, TouchInputDriver* driver,
                       InputSystem* input_system);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  TouchInputDriver* driver_ = nullptr;
  InputSystem* input_system_ = nullptr;
};

}  // namespace nocturne::touch
