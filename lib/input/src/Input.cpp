#include <Arduino.h>

#include "Input.h"
#include "pins.h"

constexpr float _deadzone = 0.3f;
constexpr float _smoothing = 0.5f;
constexpr uint32_t _debounce_ms = 50;
constexpr uint8_t _calib_n = 20;

float Input::_jx = 0.0f;
float Input::_jy = 0.0f;
uint16_t Input::_cx = 2047; // calibrated rest centre, per board (pots differ)
uint16_t Input::_cy = 2047;

Input::_button Input::_fire;
Input::_button Input::_reload;
Input::_button Input::_interact;
Input::_button Input::_pause;

bool Input::begin() {
  pinMode(JOY_X, INPUT);
  pinMode(JOY_Y, INPUT);

  pinMode(BTN_FIRE, INPUT_PULLUP);
  pinMode(BTN_RELOAD, INPUT_PULLUP);
  pinMode(BTN_INTERACT, INPUT_PULLUP);
  pinMode(BTN_PAUSE, INPUT_PULLUP);

  // every pot rests elsewhere: average the stick at boot so release reads 0
  // hands off during the logo or the centre learns an offset (same 200ms window)
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

void Input::update() {
  _jx = _jx * _smoothing + _axis(JOY_X) * (1.0f - _smoothing);
  _jy = _jy * _smoothing + _axis(JOY_Y) * (1.0f - _smoothing);

  _poll_button(_fire, BTN_FIRE);
  _poll_button(_reload, BTN_RELOAD);
  _poll_button(_interact, BTN_INTERACT);
  _poll_button(_pause, BTN_PAUSE);
}

float Input::jx() {
  return _jx;
}

float Input::jy() {
  return _jy;
}

bool Input::fire_pressed() {
  return _fire.edge;
}

bool Input::fire_down() {
  return _fire.level;
}

bool Input::reload_pressed() {
  return _reload.edge;
}

bool Input::reload_down() {
  return _reload.level;
}

bool Input::interact_pressed() {
  return _interact.edge;
}

bool Input::interact_down() {
  return _interact.level;
}

bool Input::pause_pressed() {
  return _pause.edge;
}

bool Input::pause_down() {
  return _pause.level;
}

float Input::_axis(uint8_t pin) {
  const uint16_t c = (pin == JOY_X) ? _cx : _cy;
  const float span = (float)((c > 2047) ? (4095 - c) : c); // headroom to the rail
  const float v = (span > 0.0f) ? (float)((int)analogRead(pin) - (int)c) / span : 0.0f;

  if (v > -_deadzone && v < _deadzone) {
    return 0.0f;
  }

  return v > 0.0f ? (v - _deadzone) / (1.0f - _deadzone) : (v + _deadzone) / (1.0f - _deadzone);
}

void Input::_poll_button(Input::_button& b, uint8_t pin) {
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
