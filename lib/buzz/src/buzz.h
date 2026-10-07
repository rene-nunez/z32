#pragma once

#include <cstdint>

// Passive buzzer via LEDC, non-blocking: call update() every frame
class buzz {
  public:
    enum class jingle : uint8_t { menu, shoot, buy, roulette, hurt, wave, over, denied, intro, reload };

    struct note {
      uint16_t freq; // Hz, 0 = rest
      uint16_t ms;
    };

    static bool begin();
    static void play(jingle j); // preempts the current one
    static void update(uint32_t now);
    static void stop();

  private:
    static constexpr uint8_t _ch = 0;
    static constexpr uint8_t _duty = 128;

    static const note* _seq; // null = idle
    static uint8_t _n;
    static uint8_t _i;
    static uint32_t _until;

    static void _start_note(uint8_t i, uint32_t now);
};
