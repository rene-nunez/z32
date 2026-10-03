#pragma once

#include <cstdint>

// Passive buzzer on PIN_BUZZ via LEDC, non-blocking: play() starts a jingle and
// update(now) steps the note sequencer, so the frame loop never waits for sound.
// game fires play() from sim::last_event and the menu edges, and calls update()
// every frame. sim never touches this: it only emits the events.
class buzz {
  public:
    enum class jingle : uint8_t { menu, shoot, buy, roulette, hurt, wave, over, denied, intro };

    struct note {
      uint16_t freq; // Hz, 0 = rest
      uint16_t ms;
    };

    static bool begin();        // LEDC setup, silent
    static void play(jingle j); // (re)starts a jingle, preempts the current one
    static void update(uint32_t now); // advance the sequencer; call every frame
    static void stop();         // silence now

  private:
    static constexpr uint8_t _ch = 0;    // LEDC channel, free (display BL is GPIO)
    static constexpr uint8_t _duty = 128; // 50%: max volume on a square wave (0..255)

    static const note* _seq; // current sequence in flash, null = idle
    static uint8_t _n;       // notes in _seq
    static uint8_t _i;       // index of the sounding note
    static uint32_t _until;  // millis() when the sounding note ends

    static void _start_note(uint8_t i, uint32_t now);
};
