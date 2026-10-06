#include <Arduino.h>

#include "buzz.h"
#include "pins.h"

namespace {
  const buzz::note _j_menu[] = {{700, 40}};
  const buzz::note _j_shoot[] = {{880, 50}};
  const buzz::note _j_buy[] = {{660, 70}, {990, 90}};
  const buzz::note _j_roulette[] = {{523, 70}, {659, 70}, {784, 70}, {1046, 120}};
  const buzz::note _j_hurt[] = {{150, 200}};
  const buzz::note _j_wave[] = {{392, 90}, {523, 90}, {659, 140}};
  const buzz::note _j_over[] = {{400, 120}, {300, 120}, {200, 120}, {150, 200}};
  const buzz::note _j_denied[] = {{200, 90}, {0, 40}, {200, 120}};
  const buzz::note _j_reload[] = {{1200, 30}, {0, 40}, {900, 50}}; // mag out, mag in
  // intro lament: stepwise D-minor song rising D-E-F, sinking to the modal C
  // and resolving from low A, ~2.15s so it fills one 2.5s intro screen.
  // Fired on logo entry and re-fired on team entry.
  const buzz::note _j_intro[] = {{294, 150}, {330, 150}, {349, 150}, {330, 150},
                                 {294, 200}, {262, 200}, {294, 450}, {0, 150},
                                 {220, 200}, {294, 350}};

  struct _entry {
    const buzz::note* seq;
    uint8_t n;
  };

  // indexed by buzz::jingle, so the order here is the enum order
  const _entry _seqs[] = {
      {_j_menu, 1}, {_j_shoot, 1}, {_j_buy, 2}, {_j_roulette, 4},
      {_j_hurt, 1}, {_j_wave, 3}, {_j_over, 4}, {_j_denied, 3},
      {_j_intro, 10}, {_j_reload, 3},
  };
  static_assert(sizeof(_seqs) / sizeof(_seqs[0]) == 10, "one row per buzz::jingle");
} // namespace

const buzz::note* buzz::_seq = nullptr;
uint8_t buzz::_n = 0;
uint8_t buzz::_i = 0;
uint32_t buzz::_until = 0;

bool buzz::begin() {
  ledcSetup(_ch, 2000, 8);
  ledcAttachPin(PIN_BUZZ, _ch);
  ledcWrite(_ch, _duty);
  ledcWriteTone(_ch, 0);
  return true;
}

void buzz::play(jingle j) {
  const _entry& e = _seqs[(uint8_t)j];
  _seq = e.seq;
  _n = e.n;
  _start_note(0, millis());
}

void buzz::update(uint32_t now) {
  if (_seq == nullptr) {
    return; // idle
  }
  if ((int32_t)(now - _until) < 0) {
    return; // sounding note still has time left (wrap-safe)
  }
  const uint8_t next = (uint8_t)(_i + 1u);
  if (next >= _n) {
    stop();
    return;
  }
  _start_note(next, now);
}

void buzz::stop() {
  ledcWriteTone(_ch, 0);
  _seq = nullptr;
  _n = 0;
  _i = 0;
}

void buzz::_start_note(uint8_t i, uint32_t now) {
  _i = i;
  ledcWriteTone(_ch, _seq[i].freq); // 0 silences the channel, duty persists
  _until = now + _seq[i].ms;
}
