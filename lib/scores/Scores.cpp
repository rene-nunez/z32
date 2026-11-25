#include <Arduino.h>

#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

#include <cstring>

#include <pins.h>

#include "Scores.h"

namespace {
  constexpr uint32_t MAGIC = 0x5A3C21F0u; // bumped: run.wave went u8->u16 (kills/wave wrap fix)
  constexpr const char* RECENT = "/z32_recent.json"; // last-4 cache, {"runs":[{p,k,w}]}
  constexpr const char* LOG = "/z32_log.jsonl"; // full history, one {"p","k","w"} per line
  constexpr uint32_t TAIL_BYTES = 1024; // ~25 runs, we only ever need the last 4

  struct rtc_scores {
    uint32_t magic;
    Scores::run hist[Scores::HISTORY_N]; // recent first, index 0 is the newest
    uint8_t len; // runs actually stored, 0..HISTORY_N
  };

  RTC_NOINIT_ATTR rtc_scores rtc;
  Scores::run hist[Scores::HISTORY_N] = {};
  uint8_t len = 0;
  bool sd_ready = false;

  void push(const Scores::run& r) {
    for (int8_t i = Scores::HISTORY_N - 1; i > 0; --i) {
      hist[i] = hist[i - 1];
    }
    hist[0] = r;
    if (len < Scores::HISTORY_N) {
      ++len;
    }
  }

  void mirror_rtc() {
    rtc.magic = MAGIC;
    rtc.len = len;
    for (uint8_t i = 0; i < Scores::HISTORY_N; ++i) {
      rtc.hist[i] = hist[i];
    }
  }

  // shares the TFT SPI bus (23/19/18, default VSPI pins) with the dedicated CS 22
  // Best-effort: a missing card only logs, the RTC mirror keeps the game going
  bool sd_mount() {
    if (sd_ready) {
      return true;
    }
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH); // park the TFT, we own the bus for init
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH); // deselect the card for the wake-up clocks
    SPI.begin(18, 19, 23, -1); // route the VSPI pins (-1 = no bus-wide SS, CS is per device)
    // a soft reset (EN button) keeps power on, so a card stuck mid-init stays deaf
    // until real clocks arrive: 80+ idle clocks with CS high plus a few attempts
    // usually wake it without replugging the board
    for (uint8_t i = 0; i < 10; ++i) {
      SPI.transfer(0xFF); // 10 bytes = 80 clocks
    }
    for (uint8_t attempt = 0; attempt < 3 && !sd_ready; ++attempt) {
      if (attempt > 0) {
        delay(200);
      }
      sd_ready = SD.begin(SD_CS, SPI, 4000000); // 4MHz: dupont wires + a shared bus
    }
    if (!sd_ready) {
      Serial.println("[scores] no sd, rtc only");
    } else {
      Serial.printf("[scores] microsd ok, type %u size %lluMB\n", SD.cardType(), SD.cardSize() / (1024u * 1024u));
    }
    digitalWrite(TFT_CS, HIGH); // leave the bus parked for the TFT
    return sd_ready;
  }

  // parses a {"runs":[{p,k,w}]} cache; adopts it only when the RTC came up empty
  // Returns true when the file existed and parsed, even when there was nothing to adopt
  bool load_recent_file(const char* path) {
    if (!SD.exists(path)) {
      return false; // clean boot: stay silent, the VFS logs an error on missing reads
    }
    File f = SD.open(path, FILE_READ);
    if (!f) {
      return false;
    }
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      Serial.println("[scores] recent corrupt, starting empty");
      return false;
    }
    if (len == 0) { // rtc empty after a power loss: adopt the card history
      JsonArray runs = doc["runs"].as<JsonArray>();
      for (JsonObject r : runs) {
        if (len >= Scores::HISTORY_N) {
          break;
        }
        hist[len].pts = r["p"] | 0u;
        hist[len].kills = r["k"] | 0u;
        hist[len].wave = r["w"] | 0u;
        ++len;
      }
    }
    return true;
  }

  void log_append(const Scores::run& r) {
    // one buffer for both sinks, so the serial line is byte-identical to the file line
    char line[64];
    JsonDocument doc;
    doc["p"] = r.pts;
    doc["k"] = r.kills;
    doc["w"] = r.wave;
    if (!serializeJson(doc, line, sizeof(line))) {
      Serial.println("[scores] log write failed");
      return;
    }
    File f = SD.open(LOG, FILE_APPEND);
    if (!f) {
      Serial.println("[scores] log write failed");
      return;
    }
    f.print(line);
    f.print('\n');
    f.close();
    Serial.printf("[scores] run saved %s\n", line);
  }

  // last resort when both the RTC and the recent cache are empty: replays the tail
  // of the log. Reads at most TAIL_BYTES so a years-old log still costs O(1) RAM
  void log_tail_load() {
    if (!SD.exists(LOG)) {
      return; // clean boot: stay silent, the VFS logs an error on missing reads
    }
    File f = SD.open(LOG, FILE_READ);
    if (!f) {
      return; // clean boot, nothing stored yet: stay silent
    }
    const size_t size = f.size();
    const size_t want = size > TAIL_BYTES ? (size_t)TAIL_BYTES : size;
    if (want == 0) {
      f.close();
      return;
    }
    if (size > want) {
      f.seek(size - want);
    }
    char tail[TAIL_BYTES + 1];
    const size_t got = (size_t)f.readBytes(tail, want);
    f.close();
    tail[got] = '\0';

    // A mid-line seek leaves a partial first line: skip it unless we read the whole file
    char* cur = tail;
    if (size > want) {
      char* nl = strchr(cur, '\n');
      if (!nl) {
        return;
      }
      cur = nl + 1;
    }
    // collect forward, keep only the last 4 valid lines (file order = oldest first)
    Scores::run kept[Scores::HISTORY_N] = {};
    uint8_t n = 0;
    while (*cur != '\0') {
      char* nl = strchr(cur, '\n');
      if (nl) {
        *nl = '\0';
      }
      if (*cur != '\0') {
        JsonDocument doc;
        if (!deserializeJson(doc, cur)) {
          const Scores::run r = {(uint32_t)(doc["p"] | 0u), (uint32_t)(doc["k"] | 0u), (uint16_t)(doc["w"] | 0u)};
          if (n < Scores::HISTORY_N) {
            kept[n++] = r;
          } else {
            kept[0] = kept[1];
            kept[1] = kept[2];
            kept[2] = kept[3];
            kept[3] = r;
          }
        }
      }
      if (!nl) {
        break;
      }
      cur = nl + 1;
    }
    // store recent-first: the last log line is the newest run
    for (uint8_t i = 0; i < n; ++i) {
      hist[i] = kept[n - 1 - i];
    }
    len = n;
  }

  void sd_load_merge() {
    if (!sd_mount()) {
      return;
    }
    const bool have_cache = load_recent_file(RECENT);
    if (len == 0 && !have_cache) {
      log_tail_load(); // recent missing/corrupt: replay the log tail
    }
  }

  void sd_save() {
    if (!sd_mount()) {
      return;
    }
    SD.remove(RECENT); // FILE_WRITE appends, so truncate first
    File f = SD.open(RECENT, FILE_WRITE);
    if (!f) {
      Serial.println("[scores] recent open failed");
      return;
    }
    JsonDocument doc;
    JsonArray runs = doc["runs"].to<JsonArray>();
    for (uint8_t i = 0; i < len; ++i) {
      JsonObject r = runs.add<JsonObject>();
      r["p"] = hist[i].pts;
      r["k"] = hist[i].kills;
      r["w"] = hist[i].wave;
    }
    if (!serializeJson(doc, f)) {
      Serial.println("[scores] recent write failed");
    }
    f.close();
    if (len > 0) {
      log_append(hist[0]); // newest run, one line per death
    }
  }
}

void Scores::load() {
  if (rtc.magic == MAGIC) {
    len = rtc.len > HISTORY_N ? HISTORY_N : rtc.len;
    for (uint8_t i = 0; i < HISTORY_N; ++i) {
      hist[i] = rtc.hist[i];
    }
  } else {
    len = 0;
    for (uint8_t i = 0; i < HISTORY_N; ++i) {
      hist[i] = {0, 0, 0};
    }
  }
  sd_load_merge(); // power-loss recovery: adopt the card history when the RTC is empty
  mirror_rtc();
}

void Scores::add_run(uint32_t kills, uint32_t wallet, uint16_t wave) {
  push({wallet, kills, wave});
  mirror_rtc();
  sd_save(); // once per death, cheap enough to mount+write here
}

const Scores::run* Scores::history() {
  return hist;
}

uint8_t Scores::history_len() {
  return len;
}
