#pragma once

// Menu chrome: screen ids, item tables and repaint cache

#include <cstdint>

class Screens {
  public:
    enum class id : uint8_t { logo, team, menu, mode, scores, playing, pause, game_over, waiting };

    static uint8_t count(id scr);

    static void paint(id scr, uint8_t sel);

    static void invalidate();
};
