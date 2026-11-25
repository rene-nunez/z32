#pragma once

// Last runs in RTC, mirrored to SD; full history in the log

#include <cstdint>

class Scores {
  public:
    static constexpr uint8_t HISTORY_N = 4;

    struct run {
      uint32_t pts;
      uint32_t kills;
      uint16_t wave;
    };

    static void load();
    static void add_run(uint32_t kills, uint32_t wallet, uint16_t wave);
    static const run* history();
    static uint8_t history_len();
};
