// Joystick ADC1 and buttons; stick centre calibrated at boot

#include <Arduino.h>

#include "Input.h"
#include "pins.h"

constexpr float deadzone = 0.3f;
constexpr float smoothing = 0.5f;
constexpr uint32_t debounce_ms = 50;
constexpr uint8_t calib_n = 20;

float Input::stick_x = 0.0f;
float Input::stick_y = 0.0f;
uint16_t Input::cx = 2047;
uint16_t Input::cy = 2047;

Input::button Input::fire;
Input::button Input::reload;
Input::button Input::interact;
Input::button Input::pause;

bool Input::begin() {
  pinMode(JOY_X, INPUT);
  pinMode(JOY_Y, INPUT);

  pinMode(BTN_FIRE, INPUT_PULLUP);
  pinMode(BTN_RELOAD, INPUT_PULLUP);
  pinMode(BTN_INTERACT, INPUT_PULLUP);
  pinMode(BTN_PAUSE, INPUT_PULLUP);

  uint32_t sx = 0, sy = 0;

  // Hands off during the ~200ms calibration window
  for (uint8_t i = 0; i < calib_n; ++i) {
    sx += analogRead(JOY_X);
    sy += analogRead(JOY_Y);
    delay(10);
  }

  cx = (uint16_t)(sx / calib_n);
  cy = (uint16_t)(sy / calib_n);

  return true;
}

void Input::update() {
  stick_x = stick_x * smoothing + axis(JOY_X) * (1.0f - smoothing);
  stick_y = stick_y * smoothing + axis(JOY_Y) * (1.0f - smoothing);

  poll_button(fire, BTN_FIRE);
  poll_button(reload, BTN_RELOAD);
  poll_button(interact, BTN_INTERACT);
  poll_button(pause, BTN_PAUSE);
}

float Input::jx() {
  return stick_x;
}

float Input::jy() {
  return stick_y;
}

bool Input::fire_pressed() {
  return fire.edge;
}

bool Input::fire_down() {
  return fire.level;
}

bool Input::reload_pressed() {
  return reload.edge;
}

bool Input::reload_down() {
  return reload.level;
}

bool Input::interact_pressed() {
  return interact.edge;
}

bool Input::interact_down() {
  return interact.level;
}

bool Input::pause_pressed() {
  return pause.edge;
}

bool Input::pause_down() {
  return pause.level;
}

float Input::axis(uint8_t pin) {
  const uint16_t c = (pin == JOY_X) ? cx : cy;
  const float span = (float)((c > 2047) ? (4095 - c) : c);
  const float v = (span > 0.0f) ? (float)((int)analogRead(pin) - (int)c) / span : 0.0f;

  if (v > -deadzone && v < deadzone) {
    return 0.0f;
  }

  return v > 0.0f ? (v - deadzone) / (1.0f - deadzone) : (v + deadzone) / (1.0f - deadzone);
}

void Input::poll_button(Input::button& b, uint8_t pin) {
  const bool raw = (digitalRead(pin) == LOW);
  b.edge = false;

  if (raw != b.raw) {
    b.raw = raw;
    b.since = millis();
    return;
  }

  if (raw != b.level && (uint32_t)(millis() - b.since) >= debounce_ms) {
    b.level = raw;
    b.edge = raw;
  }
}
