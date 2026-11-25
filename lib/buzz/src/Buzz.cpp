#include <Arduino.h>

#include "Buzz.h"
#include "pins.h"

namespace {
  const Buzz::note j_menu[] = {{700, 40}};
  const Buzz::note j_shoot[] = {{880, 50}};
  const Buzz::note j_buy[] = {{660, 70}, {990, 90}};
  const Buzz::note j_roulette[] = {{523, 70}, {659, 70}, {784, 70}, {1046, 120}};
  const Buzz::note j_hurt[] = {{150, 200}};
  const Buzz::note j_wave[] = {{392, 90}, {523, 90}, {659, 140}};
  const Buzz::note j_over[] = {{400, 120}, {300, 120}, {200, 120}, {150, 200}};
  const Buzz::note j_denied[] = {{200, 90}, {0, 40}, {200, 120}};
  const Buzz::note j_reload[] = {{1200, 30}, {0, 40}, {900, 50}};
  // ~2.15s to fill one intro screen.
  const Buzz::note j_intro[] = {{294, 150}, {330, 150}, {349, 150}, {330, 150},
                                 {294, 200}, {262, 200}, {294, 450}, {0, 150},
                                 {220, 200}, {294, 350}};

  struct entry {
    const Buzz::note* seq;
    uint8_t n;
  };

  // indexed by Buzz::jingle, so the order here is the enum order
  const entry seqs[] = {
      {j_menu, 1}, {j_shoot, 1}, {j_buy, 2}, {j_roulette, 4},
      {j_hurt, 1}, {j_wave, 3}, {j_over, 4}, {j_denied, 3},
      {j_intro, 10}, {j_reload, 3},
  };
  static_assert(sizeof(seqs) / sizeof(seqs[0]) == 10, "one row per Buzz::jingle");
} // namespace

const Buzz::note* Buzz::seq = nullptr;
uint8_t Buzz::n = 0;
uint8_t Buzz::i = 0;
uint32_t Buzz::until = 0;

bool Buzz::begin() {
  ledcSetup(ch, 2000, 8);
  ledcAttachPin(PIN_BUZZ, ch);
  ledcWrite(ch, duty);
  ledcWriteTone(ch, 0);
  return true;
}

void Buzz::play(jingle j) {
  const entry& e = seqs[(uint8_t)j];
  seq = e.seq;
  n = e.n;
  start_note(0, millis());
}

void Buzz::update(uint32_t now) {
  if (seq == nullptr) {
    return;
  }
  if ((int32_t)(now - until) < 0) {
    return; // wrap-safe
  }
  const uint8_t next = (uint8_t)(i + 1u);
  if (next >= n) {
    stop();
    return;
  }
  start_note(next, now);
}

void Buzz::stop() {
  ledcWriteTone(ch, 0);
  seq = nullptr;
  n = 0;
  i = 0;
}

void Buzz::start_note(uint8_t i, uint32_t now) {
  i = i;
  ledcWriteTone(ch, seq[i].freq); // 0 silences the channel, duty persists
  until = now + seq[i].ms;
}
