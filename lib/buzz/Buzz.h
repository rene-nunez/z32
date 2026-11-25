#pragma once

// Passive buzzer via LEDC, non-blocking; call update() every frame

#include <cstdint>

class Buzz {
  public:
    enum class jingle : uint8_t { menu, shoot, buy, roulette, hurt, wave, over, denied, intro, reload };

    struct note {
      uint16_t freq;
      uint16_t ms;
    };

    static bool begin();
    static void play(jingle j);
    static void update(uint32_t now);
    static void stop();

  private:
    static constexpr uint8_t ch = 0;
    static constexpr uint8_t duty = 128;

    static const note* seq;
    static uint8_t n;
    static uint8_t i;
    static uint32_t until;

    static void start_note(uint8_t i, uint32_t now);
};
