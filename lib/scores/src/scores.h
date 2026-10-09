#pragma once

#include <cstdint>

// The last runs: RTC memory (survives deep sleep), mirrored to /z32_recent.json
// (survives power loss) plus the full history appended to /z32_log.jsonl
// (one JSON per line, oldest first). No sums, no records: each death pushes one
// {pts,kills,wave} run, RAM and the menu keep only the last 4, the log keeps them all
// Only the storage inside scores.cpp changes, the calls below do not
class scores {
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
