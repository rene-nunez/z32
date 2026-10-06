#include <Arduino.h>

#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

#include <pins.h>

#include "Points.h"

namespace {
  constexpr uint32_t _MAGIC = 0x5A3C21F0u; // bumped: run.wave went u8->u16 (kills/wave wrap fix)
  constexpr const char* _PATH = "/z32.json";

  struct _rtc_points {
    uint32_t magic;
    points::run hist[points::HISTORY_N]; // recent first, index 0 is the newest
    uint8_t len; // runs actually stored, 0..HISTORY_N
  };

  RTC_NOINIT_ATTR _rtc_points _rtc;
  points::run _hist[points::HISTORY_N] = {};
  uint8_t _len = 0;
  bool _sd_ready = false;

  void _push(const points::run& r) {
    for (int8_t i = points::HISTORY_N - 1; i > 0; --i) {
      _hist[i] = _hist[i - 1];
    }
    _hist[0] = r;
    if (_len < points::HISTORY_N) {
      ++_len;
    }
  }

  void _mirror_rtc() {
    _rtc.magic = _MAGIC;
    _rtc.len = _len;
    for (uint8_t i = 0; i < points::HISTORY_N; ++i) {
      _rtc.hist[i] = _hist[i];
    }
  }

  // shares the TFT SPI bus (23/19/18, default VSPI pins) with the dedicated CS 22.
  // Best-effort: a missing card only logs, the RTC mirror keeps the game going.
  bool _sd_mount() {
    if (_sd_ready) {
      return true;
    }
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH); // park the TFT, we own the bus for init
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH); // deselect the card for the wake-up clocks
    SPI.begin(18, 19, 23, -1); // route the VSPI pins (-1 = no bus-wide SS, CS is per device)
    // a soft reset (EN button) keeps power on, so a card stuck mid-init stays deaf
    // until real clocks arrive: 80+ idle clocks with CS high plus a few attempts
    // usually wake it without replugging the board.
    for (uint8_t i = 0; i < 10; ++i) {
      SPI.transfer(0xFF); // 10 bytes = 80 clocks
    }
    for (uint8_t attempt = 0; attempt < 3 && !_sd_ready; ++attempt) {
      if (attempt > 0) {
        delay(200);
      }
      _sd_ready = SD.begin(SD_CS, SPI, 4000000); // 4MHz: dupont wires + a shared bus
    }
    if (!_sd_ready) {
      Serial.println("[points] no sd, rtc only");
    } else {
      Serial.printf("[points] sd ok, type %u size %lluMB\n", SD.cardType(),
                    SD.cardSize() / (1024u * 1024u));
    }
    digitalWrite(TFT_CS, HIGH); // leave the bus parked for the TFT
    return _sd_ready;
  }

  void _sd_load_merge() {
    if (!_sd_mount()) {
      return;
    }
    File f = SD.open(_PATH, FILE_READ);
    if (!f) {
      Serial.println("[points] no save file");
      return;
    }
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      Serial.println("[points] save corrupt");
      return;
    }
    if (_len == 0) { // rtc empty after a power loss: adopt the card history
      JsonArray runs = doc["runs"].as<JsonArray>();
      for (JsonObject r : runs) {
        if (_len >= points::HISTORY_N) {
          break;
        }
        _hist[_len].pts = r["p"] | 0u;
        _hist[_len].kills = r["k"] | 0u;
        _hist[_len].wave = r["w"] | 0u;
        ++_len;
      }
    }
  }

  void _sd_save() {
    if (!_sd_mount()) {
      return;
    }
    SD.remove(_PATH); // FILE_WRITE appends, so truncate first
    File f = SD.open(_PATH, FILE_WRITE);
    if (!f) {
      Serial.println("[points] save open failed");
      return;
    }
    JsonDocument doc;
    JsonArray runs = doc["runs"].to<JsonArray>();
    for (uint8_t i = 0; i < _len; ++i) {
      JsonObject r = runs.add<JsonObject>();
      r["p"] = _hist[i].pts;
      r["k"] = _hist[i].kills;
      r["w"] = _hist[i].wave;
    }
    if (!serializeJson(doc, f)) {
      Serial.println("[points] save write failed");
    }
    f.close();
  }
}

void points::load() {
  if (_rtc.magic == _MAGIC) {
    _len = _rtc.len > HISTORY_N ? HISTORY_N : _rtc.len;
    for (uint8_t i = 0; i < HISTORY_N; ++i) {
      _hist[i] = _rtc.hist[i];
    }
  } else {
    _len = 0;
    for (uint8_t i = 0; i < HISTORY_N; ++i) {
      _hist[i] = {0, 0, 0};
    }
  }
  _sd_load_merge(); // power-loss recovery: adopt the card history when the RTC is empty
  _mirror_rtc();
}

void points::add_run(uint32_t kills, uint32_t wallet, uint16_t wave) {
  _push({wallet, kills, wave});
  _mirror_rtc();
  _sd_save(); // once per death, cheap enough to mount+write here
}

const points::run* points::history() {
  return _hist;
}

uint8_t points::history_len() {
  return _len;
}
