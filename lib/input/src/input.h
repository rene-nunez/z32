#pragma once

#include <cstdint>

class input {
  public:
    static bool begin();
    static void update();

    static float jx();
    static float jy();

    static bool fire_pressed();
    static bool fire_down();
    static bool reload_pressed();
    static bool reload_down();
    static bool interact_pressed();
    static bool interact_down();
    static bool pause_pressed();
    static bool pause_down();

  private:
    struct _button {
      bool raw = false;
      bool level = false;
      bool edge = false;
      uint32_t since = 0;
    };

    static float _jx;
    static float _jy;
    static uint16_t _cx;
    static uint16_t _cy;

    static _button _fire;
    static _button _reload;
    static _button _interact;
    static _button _pause;

    static float _axis(uint8_t pin);
    static void _poll_button(_button& b, uint8_t pin);
};
