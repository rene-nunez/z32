// Score storage: RTC cache, SD mirror and JSONL log

#include <Arduino.h>

#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

#include <cstring>

#include <pins.h>

#include "Scores.h"

namespace {
  constexpr uint32_t MAGIC = 0x5A3C21F0u;
  constexpr const char* RECENT = "/z32_recent.json";
  constexpr const char* LOG = "/z32_log.jsonl";
  constexpr uint32_t TAIL_BYTES = 1024;

  struct rtc_scores {
    uint32_t magic;
    Scores::run hist[Scores::HISTORY_N];
    uint8_t len;
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

  bool sd_mount() {
    if (sd_ready) {
      return true;
    }
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    SPI.begin(18, 19, 23, -1);
    for (uint8_t i = 0; i < 10; ++i) {
      SPI.transfer(0xFF);
    }
    for (uint8_t attempt = 0; attempt < 3 && !sd_ready; ++attempt) {
      if (attempt > 0) {
        delay(200);
      }
      sd_ready = SD.begin(SD_CS, SPI, 4000000);
    }
    if (!sd_ready) {
      Serial.println("[scores] no sd, rtc only");
    } else {
      Serial.printf("[scores] microsd ok, type %u size %lluMB\n", SD.cardType(), SD.cardSize() / (1024u * 1024u));
    }
    digitalWrite(TFT_CS, HIGH);
    return sd_ready;
  }

  bool load_recent_file(const char* path) {
    if (!SD.exists(path)) {
      return false;
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
    if (len == 0) {
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

  void log_tail_load() {
    if (!SD.exists(LOG)) {
      return;
    }
    File f = SD.open(LOG, FILE_READ);
    if (!f) {
      return;
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

    char* cur = tail;
    if (size > want) {
      char* nl = strchr(cur, '\n');
      if (!nl) {
        return;
      }
      cur = nl + 1;
    }
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
      log_tail_load();
    }
  }

  void sd_save() {
    if (!sd_mount()) {
      return;
    }
    SD.remove(RECENT);
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
      log_append(hist[0]);
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
  sd_load_merge();
  mirror_rtc();
}

void Scores::add_run(uint32_t kills, uint32_t wallet, uint16_t wave) {
  push({wallet, kills, wave});
  mirror_rtc();
  sd_save();
}

const Scores::run* Scores::history() {
  return hist;
}

uint8_t Scores::history_len() {
  return len;
}
