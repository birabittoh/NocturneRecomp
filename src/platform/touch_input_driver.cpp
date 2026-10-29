/**
 * @file        touch_input_driver.cpp
 * @brief       On-screen touch controls -> Xbox 360 controller
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "touch_input_driver.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_video.h>

#include <rex/cvar.h>
#include <rex/input/gyro.h>
#include <rex/logging.h>
#include <rex/input/input_system.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>
#include <rexglue_guide/guide.h>

// Touch controls only make sense where there is a touch screen and no
// keyboard, so they default on for Android and off elsewhere. They stay
// switchable on the desktop, which is how a layout gets worked on without a
// device in hand.
REXCVAR_DEFINE_BOOL(touch_controls, REX_PLATFORM_ANDROID != 0, "Input/Touch",
                    "Show on-screen touch controls");
REXCVAR_DEFINE_DOUBLE(touch_scale, 1.0, "Input/Touch",
                      "Size of the on-screen touch controls (0.5..2)");
REXCVAR_DEFINE_BOOL(touch_function_keys, false, "Input/Touch",
                    "Show a second row of on-screen buttons, one per F-key bind");
REXCVAR_DEFINE_STRING(touch_movement, "dpad", "Input/Touch",
                      "Touch movement control: dpad or stick (free-placed analog stick)")
    .allowed({"dpad", "stick"});
REXCVAR_DEFINE_DOUBLE(touch_opacity, 0.45, "Input/Touch",
                      "Opacity of the on-screen touch controls (0..1)");
REXCVAR_DEFINE_DOUBLE(touch_hide_delay, 3.0, "Input/Touch",
                      "Seconds without a touch before the on-screen controls fade out (1..10, above 10 never)");

namespace nocturne::touch {

namespace {

std::atomic<bool> g_fast_forward_held{false};
std::atomic<int64_t> g_keep_visible_until_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr rex::input::DeviceId kTouchDevice = static_cast<rex::input::DeviceId>(0x544F5543);

// Deflection at which a stick reports full range, as a fraction of the travel
// from its centre to the edge of the ring. Short of 1.0 so the last sliver of
// travel isn't needed to run at full speed, which on a screen with no physical
// stop is the difference between "sprints" and "almost sprints".
constexpr float kStickFullDeflection = 0.85f;
// Below this fraction of the ring a stick reads as centred, absorbing the
// wobble of a thumb resting on the glass.
constexpr float kStickDeadzone = 0.12f;
// Fraction of a D-pad's radius around its centre that reports no direction, so
// a thumb landing dead centre doesn't pick one at random.
constexpr float kDpadDeadzone = 0.25f;

// Per poll weight of a new accelerometer sample in the gravity estimate, enough
// to shed hand shake without lagging a change of grip.
constexpr float kGravitySmoothing = 0.1f;

float Dot(const float* a, const float* b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool HitTest(const TouchControl& c, float x, float y) {
  const float dx = x - c.cx;
  const float dy = y - c.cy;
  if (c.shape == TouchControl::Shape::kPill) {
    return std::fabs(dx) <= c.half_width && std::fabs(dy) <= c.half_height;
  }
  if (c.shape == TouchControl::Shape::kSquare) {
    return std::fabs(dx) <= c.half_width && std::fabs(dy) <= c.half_width;
  }
  // Circles and D-pads are both round.
  return dx * dx + dy * dy <= c.half_width * c.half_width;
}

bool InZone(const TouchStick& s, float x, float y) {
  return x >= s.zone_left && x <= s.zone_right && y >= s.zone_top && y <= s.zone_bottom;
}

// Resolves the X_INPUT_GAMEPAD_* mask a held control currently reports. Only
// a D-pad depends on where within itself it is being touched.
uint16_t ResolveButtons(const TouchControl& c, float x, float y) {
  if (c.shape != TouchControl::Shape::kDpad) {
    return c.button;
  }
  const float dx = x - c.cx;
  const float dy = y - c.cy;
  const float deadzone = c.half_width * kDpadDeadzone;
  if (dx * dx + dy * dy <= deadzone * deadzone) {
    return 0;
  }
  // Eight sectors of 45 degrees, each centred on one of the four cardinals or
  // one of the four diagonals; a diagonal reports both of its neighbours.
  // Screen y grows downwards, so "up" is negative dy.
  uint16_t buttons = 0;
  const float abs_dx = std::fabs(dx);
  const float abs_dy = std::fabs(dy);
  // tan(22.5 degrees): the boundary between a cardinal and its diagonals.
  constexpr float kDiagonalRatio = 0.4142f;
  if (abs_dx > abs_dy * kDiagonalRatio) {
    buttons |= dx > 0.0f ? X_INPUT_GAMEPAD_DPAD_RIGHT : X_INPUT_GAMEPAD_DPAD_LEFT;
  }
  if (abs_dy > abs_dx * kDiagonalRatio) {
    buttons |= dy > 0.0f ? X_INPUT_GAMEPAD_DPAD_DOWN : X_INPUT_GAMEPAD_DPAD_UP;
  }
  return buttons;
}

}  // namespace

std::vector<TouchFunctionKey> TouchFunctionKeys() {
  std::vector<TouchFunctionKey> keys;
  for (const auto& bind : rex::ui::SnapshotBinds()) {
    if (!bind.active || bind.effective_key.size() < 2 || bind.effective_key[0] != 'F') {
      continue;
    }
    int number = 0;
    for (size_t i = 1; i < bind.effective_key.size(); ++i) {
      const char ch = bind.effective_key[i];
      if (ch < '0' || ch > '9') {
        number = 0;
        break;
      }
      number = number * 10 + (ch - '0');
    }
    if (number >= 1 && number <= 12) {
      keys.push_back({number, bind.name});
    }
  }
  std::stable_sort(keys.begin(), keys.end(),
                   [](const TouchFunctionKey& a, const TouchFunctionKey& b) {
                     return a.number < b.number;
                   });
  return keys;
}

// bind_ui_mode only toggles, and UI mode is what suppresses guest input, so
// check that first: a tap must never leave UI mode. The switch itself happens
// on the next UI frame, after the bind above has opened its overlay.
static void EnterUiMode() {
  auto* kernel = rex::system::kernel_state();
  auto* runtime = kernel ? kernel->emulator() : nullptr;
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (input && !input->IsGuestInputSuppressed()) {
    rex::ui::InvokeBind("bind_ui_mode");
  }
}

void TouchKeepVisible() { g_keep_visible_until_ms.store(NowMs() + 250, std::memory_order_relaxed); }

bool TouchFastForwardHeld() { return g_fast_forward_held.load(std::memory_order_relaxed); }

TouchControl MakeCircle(float cx, float cy, float radius, std::string label, uint16_t button,
                        TouchTrigger trigger) {
  TouchControl c;
  c.shape = TouchControl::Shape::kCircle;
  c.cx = cx;
  c.cy = cy;
  c.half_width = radius;
  c.half_height = radius;
  c.label = std::move(label);
  c.button = button;
  c.trigger = trigger;
  return c;
}

TouchControl MakePill(float cx, float cy, float half_width, float half_height, std::string label,
                      uint16_t button, TouchTrigger trigger) {
  TouchControl c;
  c.shape = TouchControl::Shape::kPill;
  c.cx = cx;
  c.cy = cy;
  c.half_width = half_width;
  c.half_height = half_height;
  c.label = std::move(label);
  c.button = button;
  c.trigger = trigger;
  return c;
}

TouchControl MakeDpad(float cx, float cy, float radius) {
  TouchControl c;
  c.shape = TouchControl::Shape::kDpad;
  c.cx = cx;
  c.cy = cy;
  c.half_width = radius;
  c.half_height = radius;
  return c;
}

TouchStick MakeStick(float zone_left, float zone_top, float zone_right, float zone_bottom,
                     float radius, TouchAxis axis) {
  TouchStick s;
  s.zone_left = zone_left;
  s.zone_top = zone_top;
  s.zone_right = zone_right;
  s.zone_bottom = zone_bottom;
  s.radius = radius;
  s.axis = axis;
  s.fixed = false;
  return s;
}

TouchStick MakeFixedStick(float cx, float cy, float radius, TouchAxis axis) {
  TouchStick s;
  // A pinned stick is grabbed by touching the ring it is drawn as, so its zone
  // is that ring's bounding box.
  s.zone_left = cx - radius;
  s.zone_top = cy - radius;
  s.zone_right = cx + radius;
  s.zone_bottom = cy + radius;
  s.radius = radius;
  s.axis = axis;
  s.fixed = true;
  s.cx = cx;
  s.cy = cy;
  return s;
}

TouchInputDriver::TouchInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

TouchInputDriver::~TouchInputDriver() {
  if (attached_window_) {
    attached_window_->RemoveInputListener(this);
    attached_window_ = nullptr;
  }
  if (gyro_sensor_) {
    SDL_CloseSensor(gyro_sensor_);
  }
  if (accel_sensor_) {
    SDL_CloseSensor(accel_sensor_);
  }
  if (sensors_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_SENSOR);
  }
}

X_STATUS TouchInputDriver::Setup() {
  // The device itself is the controller, so its own gyro can aim. Skipped
  // without touch controls, so desktops don't bring up a sensor backend for
  // nothing; toggling them on takes effect after a restart.
  if (REXCVAR_GET(touch_controls)) {
    if (SDL_InitSubSystem(SDL_INIT_SENSOR)) {
      sensors_initialized_ = true;
      // Read by polling, so the event queue need not carry every sample.
      SDL_SetEventEnabled(SDL_EVENT_SENSOR_UPDATE, false);
    } else {
      REXLOG_WARN("SDL_InitSubSystem(SDL_INIT_SENSOR) failed: {}", SDL_GetError());
    }
  }
  REXLOG_INFO("Touch input driver initialized");
  return X_STATUS_SUCCESS;
}

void TouchInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    attached_window_ = window;
    window->AddInputListener(this, window_z_order());
  }
}

void TouchInputDriver::SetLayoutProvider(TouchLayoutProvider provider) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  layout_provider_ = std::move(provider);
  // Force RefreshLayout to rebuild against the new provider rather than
  // short-circuit on an unchanged surface size.
  surface_width_ = 0.0f;
  surface_height_ = 0.0f;
  layout_ = TouchLayout();
  last_touch_time_ = std::chrono::steady_clock::now();
  ReleaseAll();
}

TouchLayout TouchInputDriver::BuildLayout(float width, float height) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!layout_provider_ || width <= 0.0f || height <= 0.0f) {
    return TouchLayout();
  }
  return layout_provider_(width, height);
}

bool TouchInputDriver::IsEnabled() const {
  if (!REXCVAR_GET(touch_controls)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  return layout_provider_ != nullptr;
}

// The overlays' gate, except the Guide's: it covers the screen with a window
// that captures the mouse, but is driven like a controller, and reads this
// pad through the title's XamInputGetState like any other.
bool TouchInputDriver::AcceptsInput() const { return is_active() || rexglue_guide::IsOpen(); }

void TouchInputDriver::PublishFastForward() {
  bool held = false;
  for (size_t i = 0; i < layout_.controls.size(); ++i) {
    held |= layout_.controls[i].fast_forward && (pressed_mask_ & (uint64_t(1) << i)) != 0;
  }
  g_fast_forward_held.store(held, std::memory_order_relaxed);
}

void TouchInputDriver::ReleaseAll() {
  finger_targets_.clear();
  pressed_mask_ = 0;
  control_buttons_.assign(layout_.controls.size(), 0);
  stick_states_.assign(layout_.sticks.size(), StickState());
}

void TouchInputDriver::ReleaseFinger(FingerTargetMap::iterator it) {
  const size_t index = it->second.index;
  if (it->second.kind == FingerTarget::Kind::kStick) {
    if (index < stick_states_.size()) {
      stick_states_[index] = StickState();
    }
  } else if (index < control_buttons_.size()) {
    pressed_mask_ &= ~(uint64_t(1) << index);
    control_buttons_[index] = 0;
  }
  finger_targets_.erase(it);
}

void TouchInputDriver::RefreshLayout() {
  if (!attached_window_ || !layout_provider_) {
    return;
  }
  const float width = float(attached_window_->GetActualPhysicalWidth());
  const float height = float(attached_window_->GetActualPhysicalHeight());
  const double scale = REXCVAR_GET(touch_scale);
  std::string ui_mode_button = rex::cvar::GetFlagByName("ui_mode_button");
  std::string movement = rex::cvar::GetFlagByName("touch_movement");

  // The bind list is only re-read a couple of times a second: this runs on
  // every touch event, and binds change rarely.
  std::string function_keys = layout_function_keys_;
  const auto now = std::chrono::steady_clock::now();
  if (!REXCVAR_GET(touch_function_keys)) {
    function_keys.clear();
  } else if (function_keys.empty() ||
             now - function_keys_checked_ > std::chrono::milliseconds(500)) {
    function_keys_checked_ = now;
    function_keys = "on";
    for (const auto& key : TouchFunctionKeys()) {
      function_keys += "," + std::to_string(key.number) + key.bind;
    }
  }
  if (width == surface_width_ && height == surface_height_ && scale == layout_scale_ &&
      ui_mode_button == layout_ui_mode_button_ && movement == layout_movement_ &&
      function_keys == layout_function_keys_) {
    return;
  }
  layout_scale_ = scale;
  layout_function_keys_ = std::move(function_keys);
  layout_movement_ = std::move(movement);
  layout_ui_mode_button_ = std::move(ui_mode_button);
  surface_width_ = width;
  surface_height_ = height;
  layout_ = (width > 0.0f && height > 0.0f) ? layout_provider_(width, height) : TouchLayout();
  // The old geometry is gone, so anything held under it would never see a
  // matching release. Drop every finger and let the next press re-acquire.
  ReleaseAll();
}

void TouchInputDriver::OnTouchEvent(rex::ui::TouchEvent& e) {
  // Fired after the driver's lock is released: a bind opens overlays, which
  // may call back into the driver.
  std::string bind;
  HandleTouchEvent(e, &bind);
  if (!bind.empty()) {
    rex::ui::InvokeBind(bind);
    EnterUiMode();
  }
}

void TouchInputDriver::HandleTouchEvent(rex::ui::TouchEvent& e, std::string* bind_to_fire) {
  const bool releasing = e.action() == rex::ui::TouchEvent::Action::kUp ||
                         e.action() == rex::ui::TouchEvent::Action::kCancel;
  // A release is always processed: the gate can close between a press and its
  // release (the controls are switched off, an overlay takes the input), and a
  // finger dropped that way would stay held and its stick drawn for good.
  if (!releasing && (!REXCVAR_GET(touch_controls) || !AcceptsInput())) {
    return;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  last_touch_time_ = std::chrono::steady_clock::now();
  if (!layout_provider_) {
    return;
  }
  struct PublishFastForward {
    TouchInputDriver& d;
    ~PublishFastForward() { d.PublishFastForward(); }
  } publish{*this};
  RefreshLayout();
  if (surface_width_ <= 0.0f || surface_height_ <= 0.0f) {
    return;
  }

  const uint32_t id = e.pointer_id();
  const float x = e.x();
  const float y = e.y();

  switch (e.action()) {
    case rex::ui::TouchEvent::Action::kDown: {
      // A press on an id that is still held means its release never arrived,
      // so let go of whatever it had before it claims anything new.
      if (auto stale = finger_targets_.find(id); stale != finger_targets_.end()) {
        ReleaseFinger(stale);
        ++packet_number_;
      }
      for (size_t i = 0; i < layout_.controls.size(); ++i) {
        if (!HitTest(layout_.controls[i], x, y)) {
          continue;
        }
        pressed_mask_ |= uint64_t(1) << i;
        control_buttons_[i] = ResolveButtons(layout_.controls[i], x, y);
        finger_targets_[id] = {FingerTarget::Kind::kControl, i};
        *bind_to_fire = layout_.controls[i].bind;
        ++packet_number_;
        e.set_handled(true);
        return;
      }
      // Stick zones are only consulted after the controls, so a control
      // overlapping one still wins the touch.
      for (size_t i = 0; i < layout_.sticks.size(); ++i) {
        const TouchStick& stick = layout_.sticks[i];
        if (stick.radius <= 0.0f || stick_states_[i].active || !InZone(stick, x, y)) {
          continue;
        }
        StickState& state = stick_states_[i];
        state.active = true;
        // A free-placed stick centres itself under the finger; a pinned one
        // stays where the layout put it and only tracks the deflection.
        state.origin_x = stick.fixed ? stick.cx : x;
        state.origin_y = stick.fixed ? stick.cy : y;
        state.x = x;
        state.y = y;
        finger_targets_[id] = {FingerTarget::Kind::kStick, i};
        ++packet_number_;
        e.set_handled(true);
        return;
      }
      break;
    }

    case rex::ui::TouchEvent::Action::kMove: {
      auto it = finger_targets_.find(id);
      if (it == finger_targets_.end()) {
        break;
      }
      const size_t index = it->second.index;
      if (it->second.kind == FingerTarget::Kind::kStick) {
        stick_states_[index].x = x;
        stick_states_[index].y = y;
        ++packet_number_;
      } else {
        // Sliding off a control releases it, the way a physical button would
        // if the thumb rolled off it, and sliding back on presses it again.
        // For a D-pad the same sweep is how the direction changes mid-press.
        const uint64_t bit = uint64_t(1) << index;
        const bool inside = HitTest(layout_.controls[index], x, y);
        const uint64_t updated_mask = inside ? (pressed_mask_ | bit) : (pressed_mask_ & ~bit);
        const uint16_t updated_buttons =
            inside ? ResolveButtons(layout_.controls[index], x, y) : uint16_t(0);
        if (updated_mask != pressed_mask_ || updated_buttons != control_buttons_[index]) {
          pressed_mask_ = updated_mask;
          control_buttons_[index] = updated_buttons;
          ++packet_number_;
        }
      }
      e.set_handled(true);
      break;
    }

    case rex::ui::TouchEvent::Action::kUp:
    case rex::ui::TouchEvent::Action::kCancel: {
      auto it = finger_targets_.find(id);
      if (it == finger_targets_.end()) {
        break;
      }
      ReleaseFinger(it);
      ++packet_number_;
      e.set_handled(true);
      break;
    }
  }
}

bool TouchInputDriver::GetVisualState(TouchVisualState* out_state) {
  if (!out_state || !REXCVAR_GET(touch_controls)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  // Touch events normally build the layout; without a touchscreen none arrive.
  RefreshLayout();
  if (!layout_provider_ || surface_width_ <= 0.0f || surface_height_ <= 0.0f) {
    return false;
  }
  const bool keep_visible = NowMs() < g_keep_visible_until_ms.load(std::memory_order_relaxed);
  if (!AcceptsInput() && !keep_visible) {
    // Events stop arriving while the gate is shut, so a finger that was down
    // when it closed has no way to report its release. Drop them here, on the
    // frame that notices, rather than leaving a stick drawn.
    ReleaseAll();
    return false;
  }
  out_state->fade = 1.0f;
  const double delay = REXCVAR_GET(touch_hide_delay);
  if (!keep_visible && finger_targets_.empty() && delay <= 10.0) {
    const double idle =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - last_touch_time_).count();
    const double left = 1.0 - idle / std::max(delay, 1.0);
    if (left <= 0.0) {
      return false;
    }
    out_state->fade = float(left);
  }
  out_state->pressed_mask = pressed_mask_;
  out_state->control_buttons = control_buttons_;

  out_state->sticks.clear();
  out_state->sticks.reserve(layout_.sticks.size());
  for (size_t i = 0; i < layout_.sticks.size(); ++i) {
    const TouchStick& stick = layout_.sticks[i];
    const StickState& state = stick_states_[i];
    TouchStickVisual visual;
    visual.active = state.active;
    // A pinned stick is drawn even while untouched, so report where it lives
    // rather than the stale origin of the last touch.
    const float cx = state.active ? state.origin_x : stick.cx;
    const float cy = state.active ? state.origin_y : stick.cy;
    float knob_x = state.active ? state.x : cx;
    float knob_y = state.active ? state.y : cy;
    // A finger can drag well past the ring; the knob it stands for cannot, so
    // clamp it the way a physical stick's gate would.
    const float dx = knob_x - cx;
    const float dy = knob_y - cy;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance > stick.radius && distance > 0.0f) {
      const float scale = stick.radius / distance;
      knob_x = cx + dx * scale;
      knob_y = cy + dy * scale;
    }
    visual.cx = cx / surface_width_;
    visual.cy = cy / surface_height_;
    visual.knob_cx = knob_x / surface_width_;
    visual.knob_cy = knob_y / surface_height_;
    out_state->sticks.push_back(visual);
  }
  return true;
}

void TouchInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Disabled means no device at all, so it never occupies a guest user slot.
  if (!IsEnabled()) {
    return;
  }
  DeviceInfo info;
  info.id = kTouchDevice;
  info.name = "Touchscreen Controls";
  info.synthetic = true;
  info.kind = DeviceKind::kTouch;
  out.push_back(info);
}

X_RESULT TouchInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                 X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (!IsEnabled() || id != kTouchDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
    out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

void TouchInputDriver::UpdateDeviceSensors() {
  if (!sensors_initialized_) {
    return;
  }
  const bool want = REXCVAR_GET(gyro_aim);
  if (!want) {
    if (gyro_sensor_) {
      SDL_CloseSensor(gyro_sensor_);
      gyro_sensor_ = nullptr;
    }
    if (accel_sensor_) {
      SDL_CloseSensor(accel_sensor_);
      accel_sensor_ = nullptr;
    }
    gravity_valid_ = false;
    return;
  }
  if (gyro_sensor_) {
    return;
  }
  int count = 0;
  SDL_SensorID* ids = SDL_GetSensors(&count);
  if (!ids) {
    return;
  }
  for (int i = 0; i < count; ++i) {
    const SDL_SensorType type = SDL_GetSensorTypeForID(ids[i]);
    if (type == SDL_SENSOR_GYRO && !gyro_sensor_) {
      gyro_sensor_ = SDL_OpenSensor(ids[i]);
    } else if (type == SDL_SENSOR_ACCEL && !accel_sensor_) {
      accel_sensor_ = SDL_OpenSensor(ids[i]);
    }
  }
  SDL_free(ids);
  if (gyro_sensor_) {
    REXLOG_INFO("Touch: gyro aiming with \"{}\"{}", SDL_GetSensorName(gyro_sensor_),
                accel_sensor_ ? "" : " (no accelerometer)");
  } else {
    // Stay closed for good rather than enumerating again every poll.
    REXLOG_INFO("Touch: no gyroscope, gyro aiming unavailable");
    if (accel_sensor_) {
      SDL_CloseSensor(accel_sensor_);
      accel_sensor_ = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_SENSOR);
    sensors_initialized_ = false;
  }
}

bool TouchInputDriver::ApplyDeviceGyro(X_INPUT_GAMEPAD* gamepad) {
  UpdateDeviceSensors();
  float gyro[3];
  if (!gyro_sensor_ || !SDL_GetSensorData(gyro_sensor_, gyro, 3)) {
    return false;
  }

  // Sensor axes are fixed to the device's natural orientation; find the
  // screen's right and up axes in them for the current rotation.
  const SDL_DisplayID display = SDL_GetPrimaryDisplay();
  int rotation = 0;
  switch (SDL_GetCurrentDisplayOrientation(display)) {
    case SDL_ORIENTATION_LANDSCAPE:
      rotation = 90;
      break;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED:
      rotation = 180;
      break;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
      rotation = 270;
      break;
    default:
      break;
  }
  // SDL counts orientation from portrait even on a landscape native tablet.
  if (SDL_GetNaturalDisplayOrientation(display) == SDL_ORIENTATION_LANDSCAPE) {
    rotation = (rotation + 270) % 360;
  }
  static constexpr float kAxes[4][2][3] = {
      {{1, 0, 0}, {0, 1, 0}},    // 0: right +X, up +Y
      {{0, -1, 0}, {1, 0, 0}},   // 90: right -Y, up +X
      {{-1, 0, 0}, {0, -1, 0}},  // 180
      {{0, 1, 0}, {-1, 0, 0}},   // 270
  };
  const float* right = kAxes[rotation / 90][0];
  const float* up = kAxes[rotation / 90][1];

  // Yaw is taken about gravity rather than a device axis, so turning reads
  // the same whether the device is held upright, tilted back or lying flat.
  float accel[3];
  if (accel_sensor_ && SDL_GetSensorData(accel_sensor_, accel, 3) && Dot(accel, accel) > 1.0f) {
    if (!gravity_valid_) {
      std::copy(accel, accel + 3, gravity_);
      gravity_valid_ = true;
    } else {
      for (int i = 0; i < 3; ++i) {
        gravity_[i] += (accel[i] - gravity_[i]) * kGravitySmoothing;
      }
    }
  }
  float yaw;
  if (gravity_valid_) {
    yaw = Dot(gyro, gravity_) / std::sqrt(Dot(gravity_, gravity_));
  } else {
    yaw = Dot(gyro, up);
  }
  return ApplyGyroToGamepad(yaw, Dot(gyro, right), gamepad);
}

X_RESULT TouchInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!REXCVAR_GET(touch_controls) || id != kTouchDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!layout_provider_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  for (size_t i = 0; i < layout_.controls.size(); ++i) {
    if (!(pressed_mask_ & (uint64_t(1) << i))) {
      continue;
    }
    buttons |= control_buttons_[i];
    // A touch is on or off, so a trigger it drives goes straight to the top of
    // its range rather than to whatever threshold the guest tests against.
    const TouchTrigger trigger = layout_.controls[i].trigger;
    if (trigger == TouchTrigger::kLeft) {
      left_trigger = 0xFF;
    } else if (trigger == TouchTrigger::kRight) {
      right_trigger = 0xFF;
    }
  }

  int16_t thumb[4] = {};  // lx, ly, rx, ry
  for (size_t i = 0; i < layout_.sticks.size(); ++i) {
    const TouchStick& stick = layout_.sticks[i];
    const StickState& state = stick_states_[i];
    if (!state.active || stick.radius <= 0.0f) {
      continue;
    }
    float dx = (state.x - state.origin_x) / stick.radius;
    // Screen y grows downwards; a thumbstick's does not.
    float dy = -(state.y - state.origin_y) / stick.radius;
    const float magnitude = std::sqrt(dx * dx + dy * dy);
    if (magnitude <= kStickDeadzone) {
      continue;
    }
    // Rescale so deflection ramps up from nothing at the edge of the deadzone
    // instead of jumping to the deadzone value on crossing it.
    const float scaled =
        std::min((magnitude - kStickDeadzone) / (kStickFullDeflection - kStickDeadzone), 1.0f);
    const float unit = scaled / magnitude;
    const size_t base = stick.axis == TouchAxis::kLeft ? 0 : 2;
    thumb[base] = int16_t(std::lround(std::clamp(dx * unit, -1.0f, 1.0f) * 32767.0f));
    thumb[base + 1] = int16_t(std::lround(std::clamp(dy * unit, -1.0f, 1.0f) * 32767.0f));
  }

  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = left_trigger;
    out_state->gamepad.right_trigger = right_trigger;
    out_state->gamepad.thumb_lx = thumb[0];
    out_state->gamepad.thumb_ly = thumb[1];
    out_state->gamepad.thumb_rx = thumb[2];
    out_state->gamepad.thumb_ry = thumb[3];
    if (is_active() && ApplyDeviceGyro(&out_state->gamepad)) {
      out_state->packet_number = ++packet_number_;
    }
  }
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  (void)vibration;
  if (!IsEnabled() || id != kTouchDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // No rumble motor behind a pane of glass.
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                              X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  (void)out_keystroke;
  if (!IsEnabled() || id != kTouchDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_EMPTY;
}

}  // namespace nocturne::touch
