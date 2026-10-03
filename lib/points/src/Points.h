#pragma once

#include <cstdint>

// The last runs, in RTC memory so they survive a deep
// sleep, mirrored to /z32.json on the microSD (same TFT SPI bus, CS 22) so they survive
// a power loss. No sums, no records: each death pushes one {pts,kills,wave} run and the
// menu lists the last 4. Only the storage inside Points.cpp changes, the calls below
// do not.
class points {
  public:
    static constexpr uint8_t HISTORY_N = 4; // recent runs kept, most recent first

    struct run {
      uint32_t pts;
      uint32_t kills;
      uint16_t wave;
    };

    static void load(); // boot, magic guarded
    static void add_run(uint32_t kills, uint32_t wallet, uint16_t wave); // push one run and save
    static const run* history(); // HISTORY_N slots, recent first
    static uint8_t history_len(); // runs actually stored, 0..HISTORY_N
};
