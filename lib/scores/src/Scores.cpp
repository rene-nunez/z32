#include <Arduino.h>

#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

#include <cstring>

#include <pins.h>

#include "scores.h"

namespace {
  constexpr uint32_t _MAGIC = 0x5A3C21F0u; // bumped: run.wave went u8->u16 (kills/wave wrap fix)
  constexpr const char* _RECENT = "/z32_recent.json"; // last-4 cache, {"runs":[{p,k,w}]}
  constexpr const char* _LOG = "/z32_log.jsonl"; // full history, one {"p","k","w"} per line
  constexpr uint32_t _TAIL_BYTES = 1024; // ~25 runs, we only ever need the last 4

  struct _rtc_scores {
    uint32_t magic;
    scores::run hist[scores::HISTORY_N]; // recent first, index 0 is the newest
    uint8_t len; // runs actually stored, 0..HISTORY_N
  };

  RTC_NOINIT_ATTR _rtc_scores _rtc;
  scores::run _hist[scores::HISTORY_N] = {};
  uint8_t _len = 0;
  bool _sd_ready = false;

  void _push(const scores::run& r) {
    for (int8_t i = scores::HISTORY_N - 1; i > 0; --i) {
      _hist[i] = _hist[i - 1];
    }
    _hist[0] = r;
    if (_len < scores::HISTORY_N) {
      ++_len;
    }
  }

  void _mirror_rtc() {
    _rtc.magic = _MAGIC;
    _rtc.len = _len;
    for (uint8_t i = 0; i < scores::HISTORY_N; ++i) {
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
      Serial.println("[scores] no sd, rtc only");
    } else {
      Serial.printf("[scores] microsd ok, type %u size %lluMB\n", SD.cardType(),
                    SD.cardSize() / (1024u * 1024u));
    }
    digitalWrite(TFT_CS, HIGH); // leave the bus parked for the TFT
    return _sd_ready;
  }

  // parses a {"runs":[{p,k,w}]} cache; adopts it only when the RTC came up empty.
  // Returns true when the file existed and parsed, even when there was nothing to adopt.
  bool _load_recent_file(const char* path) {
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
    if (_len == 0) { // rtc empty after a power loss: adopt the card history
      JsonArray runs = doc["runs"].as<JsonArray>();
      for (JsonObject r : runs) {
        if (_len >= scores::HISTORY_N) {
          break;
        }
        _hist[_len].pts = r["p"] | 0u;
        _hist[_len].kills = r["k"] | 0u;
        _hist[_len].wave = r["w"] | 0u;
        ++_len;
      }
    }
    return true;
  }

  void _log_append(const scores::run& r) {
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
    File f = SD.open(_LOG, FILE_APPEND);
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
  // of the log. Reads at most _TAIL_BYTES so a years-old log still costs O(1) RAM.
  void _log_tail_load() {
    if (!SD.exists(_LOG)) {
      return; // clean boot: stay silent, the VFS logs an error on missing reads
    }
    File f = SD.open(_LOG, FILE_READ);
    if (!f) {
      return; // clean boot, nothing stored yet: stay silent
    }
    const size_t size = f.size();
    const size_t want = size > _TAIL_BYTES ? (size_t)_TAIL_BYTES : size;
    if (want == 0) {
      f.close();
      return;
    }
    if (size > want) {
      f.seek(size - want);
    }
    char tail[_TAIL_BYTES + 1];
    const size_t got = (size_t)f.readBytes(tail, want);
    f.close();
    tail[got] = '\0';

    // A mid-line seek leaves a partial first line: skip it unless we read the whole file.
    char* cur = tail;
    if (size > want) {
      char* nl = strchr(cur, '\n');
      if (!nl) {
        return;
      }
      cur = nl + 1;
    }
    // collect forward, keep only the last 4 valid lines (file order = oldest first)
    scores::run kept[scores::HISTORY_N] = {};
    uint8_t n = 0;
    while (*cur != '\0') {
      char* nl = strchr(cur, '\n');
      if (nl) {
        *nl = '\0';
      }
      if (*cur != '\0') {
        JsonDocument doc;
        if (!deserializeJson(doc, cur)) {
          const scores::run r = {(uint32_t)(doc["p"] | 0u), (uint32_t)(doc["k"] | 0u),
                                 (uint16_t)(doc["w"] | 0u)};
          if (n < scores::HISTORY_N) {
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
      _hist[i] = kept[n - 1 - i];
    }
    _len = n;
  }

  void _sd_load_merge() {
    if (!_sd_mount()) {
      return;
    }
    const bool have_cache = _load_recent_file(_RECENT);
    if (_len == 0 && !have_cache) {
      _log_tail_load(); // recent missing/corrupt: replay the log tail
    }
  }

  void _sd_save() {
    if (!_sd_mount()) {
      return;
    }
    SD.remove(_RECENT); // FILE_WRITE appends, so truncate first
    File f = SD.open(_RECENT, FILE_WRITE);
    if (!f) {
      Serial.println("[scores] recent open failed");
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
      Serial.println("[scores] recent write failed");
    }
    f.close();
    if (_len > 0) {
      _log_append(_hist[0]); // newest run, one line per death
    }
  }
}

void scores::load() {
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

void scores::add_run(uint32_t kills, uint32_t wallet, uint16_t wave) {
  _push({wallet, kills, wave});
  _mirror_rtc();
  _sd_save(); // once per death, cheap enough to mount+write here
}

const scores::run* scores::history() {
  return _hist;
}

uint8_t scores::history_len() {
  return _len;
}
