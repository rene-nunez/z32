#include <Arduino.h>

#include "input.h"
#include "pins.h"

constexpr float _deadzone = 0.3f;
constexpr float _smoothing = 0.5f;
constexpr uint32_t _debounce_ms = 50;
constexpr uint8_t _calib_n = 20; // boot samples per axis, stick at rest

float input::_jx = 0.0f;
float input::_jy = 0.0f;
uint16_t input::_cx = 2047; // calibrated rest centre, per board (pots differ)
uint16_t input::_cy = 2047;

input::_button input::_fire;
input::_button input::_reload;
input::_button input::_interact;
input::_button input::_pause;

bool input::begin() {
  pinMode(JOY_X, INPUT);
  pinMode(JOY_Y, INPUT);

  pinMode(BTN_FIRE, INPUT_PULLUP);
  pinMode(BTN_RELOAD, INPUT_PULLUP);
  pinMode(BTN_INTERACT, INPUT_PULLUP);
  pinMode(BTN_PAUSE, INPUT_PULLUP);

  // every pot rests elsewhere: average the stick at boot so release reads 0.
  // hands off during the logo or the centre learns an offset (same 200ms window).
  uint32_t sx = 0, sy = 0;
  for (uint8_t i = 0; i < _calib_n; ++i) {
    sx += analogRead(JOY_X);
    sy += analogRead(JOY_Y);
    delay(10);
  }
  _cx = (uint16_t)(sx / _calib_n);
  _cy = (uint16_t)(sy / _calib_n);

  return true;
}

void input::update() {
  _jx = _jx * _smoothing + _axis(JOY_X) * (1.0f - _smoothing);
  _jy = _jy * _smoothing + _axis(JOY_Y) * (1.0f - _smoothing);

  _poll_button(_fire, BTN_FIRE);
  _poll_button(_reload, BTN_RELOAD);
  _poll_button(_interact, BTN_INTERACT);
  _poll_button(_pause, BTN_PAUSE);
}

float input::jx() {
  return _jx;
}

float input::jy() {
  return _jy;
}

bool input::fire_pressed() {
  return _fire.edge;
}

bool input::fire_down() {
  return _fire.level;
}

bool input::reload_pressed() {
  return _reload.edge;
}

bool input::reload_down() {
  return _reload.level;
}

bool input::interact_pressed() {
  return _interact.edge;
}

bool input::interact_down() {
  return _interact.level;
}

bool input::pause_pressed() {
  return _pause.edge;
}

bool input::pause_down() {
  return _pause.level;
}

float input::_axis(uint8_t pin) {
  const uint16_t c = (pin == JOY_X) ? _cx : _cy;
  const float span = (float)((c > 2047) ? (4095 - c) : c); // headroom to the rail
  const float v = (span > 0.0f) ? (float)((int)analogRead(pin) - (int)c) / span : 0.0f;
  if (v > -_deadzone && v < _deadzone) {
    return 0.0f;
  }
  return v > 0.0f ? (v - _deadzone) / (1.0f - _deadzone) : (v + _deadzone) / (1.0f - _deadzone);
}

void input::_poll_button(input::_button& b, uint8_t pin) {
  const bool raw = (digitalRead(pin) == LOW); // active low, pull-up
  b.edge = false;

  if (raw != b.raw) {
    b.raw = raw;
    b.since = millis();
    return;
  }

  if (raw != b.level && (uint32_t)(millis() - b.since) >= _debounce_ms) {
    b.level = raw;
    b.edge = raw;
  }
}
