/**
 * @file        touch_input_driver.h
 * @brief       On-screen touch controls - drives an Xbox 360 controller from a
 *              virtual pad whose layout the host application supplies.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Sensor;

namespace nocturne::touch {

using namespace rex;
using namespace rex::input;

/// Which analog trigger a control drives, if any.
enum class TouchTrigger : uint8_t {
  kNone = 0,
  kLeft,
  kRight,
};

/// Which thumbstick axis pair a stick drives.
enum class TouchAxis : uint8_t {
  kLeft,
  kRight,
};

/**
 * One tappable control of the on-screen pad, in the coordinate space of the
 * surface its layout was built for.
 *
 * `button` is a raw X_INPUT_GAMEPAD_* mask, so every digital input the pad can
 * report is expressible here: the face buttons, Start and Back, the shoulders,
 * the guide button, the D-pad directions, and the LEFT_THUMB / RIGHT_THUMB
 * stick presses (which touch has no way to fold into the sticks themselves, so
 * a layout that wants L3/R3 places them as their own controls). Several bits
 * may be combined in one control.
 */
struct TouchControl {
  enum class Shape {
    kCircle,  // half_width is the radius; half_height is unused
    kPill,    // rounded rectangle with the given half-extents
    // Eight-way D-pad occupying a circle of radius half_width. `button` is
    // ignored: which X_INPUT_GAMEPAD_DPAD_* bits are reported depends on where
    // inside the circle the finger is, and a diagonal reports both of its
    // neighbours, so a layout gets a real D-pad rather than four buttons that
    // cannot be pressed together.
    kDpad,
  };

  Shape shape = Shape::kCircle;
  float cx = 0.0f, cy = 0.0f;
  float half_width = 0.0f, half_height = 0.0f;
  std::string label;
  uint16_t button = 0;
  TouchTrigger trigger = TouchTrigger::kNone;
  // Reports nothing to the guest; holding it only turns on TouchFastForwardHeld.
  bool fast_forward = false;
};

/**
 * An analog stick.
 *
 * By default the stick is free-placed: a finger going down anywhere in its
 * zone that no control claimed plants the centre where it landed and drags
 * from there, so there is nothing to aim for and nothing to draw until a
 * finger is down. A layout that would rather show the stick where it will
 * always be sets `fixed`, which pins the centre at cx/cy.
 */
struct TouchStick {
  // Region a touch must start in to grab this stick.
  float zone_left = 0.0f, zone_top = 0.0f, zone_right = 0.0f, zone_bottom = 0.0f;
  // Deflection at which the stick reports full range.
  float radius = 0.0f;
  TouchAxis axis = TouchAxis::kLeft;
  bool fixed = false;
  float cx = 0.0f, cy = 0.0f;
};

/// How circular controls bound to a single face button are drawn. Both
/// styles report the same X_INPUT_GAMEPAD_* bits; only the art changes.
enum class TouchFaceStyle : uint8_t {
  kXbox,         // glossy coloured domes lettered A, B, X and Y
  kPlayStation,  // dark dished buttons with a coloured cross, circle, square and triangle
};

/**
 * A whole pad laid out for a surface of a particular size.
 *
 * Nothing here is game-specific: which controls and sticks exist, where they
 * sit and how big they are is entirely up to the layout the application
 * installs on the driver. See TouchInputDriver::SetLayoutProvider.
 */
struct TouchLayout {
  std::vector<TouchControl> controls;
  // Zero, one or two sticks. Two sticks on the same axis are allowed but the
  // last one to move wins, so a layout normally has at most one of each.
  std::vector<TouchStick> sticks;
  // Positional: A is cross, B circle, X square and Y triangle.
  TouchFaceStyle face_style = TouchFaceStyle::kXbox;
};

/// Builds a circular control. `button` is an X_INPUT_GAMEPAD_* mask.
TouchControl MakeCircle(float cx, float cy, float radius, std::string label, uint16_t button,
                        TouchTrigger trigger = TouchTrigger::kNone);

/// Builds a rounded-rectangle control with the given half-extents.
TouchControl MakePill(float cx, float cy, float half_width, float half_height, std::string label,
                      uint16_t button, TouchTrigger trigger = TouchTrigger::kNone);

/// Builds an eight-way D-pad filling a circle of the given radius.
TouchControl MakeDpad(float cx, float cy, float radius);

/// Builds a free-placed stick that a touch anywhere in the given rectangle
/// plants.
TouchStick MakeStick(float zone_left, float zone_top, float zone_right, float zone_bottom,
                     float radius, TouchAxis axis);

/// Builds a stick pinned at cx/cy, grabbed by a touch inside its own ring.
TouchStick MakeFixedStick(float cx, float cy, float radius, TouchAxis axis);

/// Lays the pad out for a surface of `width` x `height`.
///
/// Called with the size of whichever surface the caller works in, so it must
/// be a pure function of that size: the driver hit-tests in window pixels
/// while the overlay draws in ImGui's logical points, and the two only agree
/// on the same pad because each rebuilds it in its own space.
using TouchLayoutProvider = std::function<TouchLayout(float width, float height)>;

/// Drawing state of one stick, in coordinates normalized to the surface.
struct TouchStickVisual {
  bool active = false;
  float cx = 0.0f, cy = 0.0f;
  float knob_cx = 0.0f, knob_cy = 0.0f;
};

/// What a renderer needs in order to draw the pad. Positions are normalized to
/// the surface (0..1 on both axes) so the caller can scale them into its own
/// space without knowing what the driver hit-tested against.
struct TouchVisualState {
  // Bit i is set while controls[i] of the layout is held.
  uint64_t pressed_mask = 0;
  // Parallel to the layout's controls: the X_INPUT_GAMEPAD_* mask each one is
  // currently reporting, or 0 when it is not held. Lets a renderer show which
  // way a D-pad is being pushed.
  std::vector<uint16_t> control_buttons;
  // Parallel to the layout's sticks.
  std::vector<TouchStickVisual> sticks;
  // 1 while the pad is in use, falling to 0 as the hide delay runs out.
  float fade = 1.0f;
};

/// Keeps the pad drawn while a settings slider is being dragged, even with the
/// input gate shut and no recent touch. Call every frame the slider is active.
void TouchKeepVisible();

/// True while a fast forward control of the layout is being held.
bool TouchFastForwardHeld();

/**
 * Synthesizes an Xbox 360 controller from touches on the game surface.
 *
 * The driver owns hit-testing, the multi-touch bookkeeping and the resulting
 * controller state; it does not own the pad's design. An application installs
 * one with SetLayoutProvider, and until it does the driver reports no device,
 * so a game that wants no touch controls pays nothing for this.
 *
 * Like the MnK driver this is not a physical device: it already emits the
 * logical button the user pressed, so it opts out of the remap_* table.
 */
class TouchInputDriver final : public InputDriver, public rex::ui::WindowInputListener {
 public:
  explicit TouchInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~TouchInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  bool is_physical_device() const override { return false; }

  // WindowInputListener
  void OnTouchEvent(rex::ui::TouchEvent& e) override;

  /// Installs the pad design. Safe to call at any point; anything currently
  /// held is released, since the controls it was held against are gone.
  void SetLayoutProvider(TouchLayoutProvider provider);

  /// Builds the layout for a surface of the given size, for callers that draw
  /// the pad. Returns an empty layout when no provider is installed.
  TouchLayout BuildLayout(float width, float height) const;

  /// True while the controls are enabled (the touch_controls cvar) and a
  /// layout has been installed.
  bool IsEnabled() const;

  /// Snapshot for whatever draws the pad. Returns false when there is nothing
  /// to draw.
  bool GetVisualState(TouchVisualState* out_state);

 private:
  // Recomputes layout_ for the attached window's current size. UI thread only,
  // called at the top of every touch event; state_mutex_ must be held.
  void RefreshLayout();

  // Drops every finger currently down. state_mutex_ must be held.
  void ReleaseAll();

  // Publishes whether a fast forward control is held. state_mutex_ must be held.
  void PublishFastForward();

  // What a finger currently down has hold of.
  struct FingerTarget {
    enum class Kind { kControl, kStick } kind;
    size_t index;
  };

  using FingerTargetMap = std::unordered_map<uint32_t, FingerTarget>;

  // Lets go of one finger and forgets it. state_mutex_ must be held.
  void ReleaseFinger(FingerTargetMap::iterator it);

  // Opens the device's own gyroscope and accelerometer while gyro aiming is
  // on, and closes them while it is off. state_mutex_ must be held.
  void UpdateDeviceSensors();

  // Adds the device's gyro to the pad. state_mutex_ must be held.
  bool ApplyDeviceGyro(X_INPUT_GAMEPAD* gamepad);

  // Live state of one stick, in surface pixels.
  struct StickState {
    bool active = false;
    float origin_x = 0.0f, origin_y = 0.0f;
    float x = 0.0f, y = 0.0f;
  };

  rex::ui::Window* attached_window_ = nullptr;

  mutable std::mutex state_mutex_;

  TouchLayoutProvider layout_provider_;
  TouchLayout layout_;
  float surface_width_ = 0.0f;
  float surface_height_ = 0.0f;
  double layout_scale_ = 0.0;
  std::string layout_ui_mode_button_;  // the toggle pill's button

  FingerTargetMap finger_targets_;

  uint64_t pressed_mask_ = 0;
  // Parallel to layout_.controls; see TouchVisualState::control_buttons.
  std::vector<uint16_t> control_buttons_;
  // Parallel to layout_.sticks.
  std::vector<StickState> stick_states_;

  bool sensors_initialized_ = false;
  SDL_Sensor* gyro_sensor_ = nullptr;
  SDL_Sensor* accel_sensor_ = nullptr;
  // Low passed accelerometer, pointing up, in device space.
  float gravity_[3] = {};
  bool gravity_valid_ = false;

  uint32_t packet_number_ = 0;
  std::chrono::steady_clock::time_point last_touch_time_ = std::chrono::steady_clock::now();
};

}  // namespace nocturne::touch
