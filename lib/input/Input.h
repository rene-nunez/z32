#pragma once

#include <cstdint>

class Input {
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
    struct button {
      bool raw = false;
      bool level = false;
      bool edge = false;
      uint32_t since = 0;
    };

    static float stick_x;
    static float stick_y;
    static uint16_t cx;
    static uint16_t cy;

    static button fire;
    static button reload;
    static button interact;
    static button pause;

    static float axis(uint8_t pin);
    static void poll_button(button& b, uint8_t pin);
};
