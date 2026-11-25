// Wallet, permanent levels, roulette and bonuses

#include <Arduino.h>

#include "Sim.h"

uint32_t Sim::kill_reward(actor_kind kind, uint16_t wave) {
  switch (kind) {
    case actor_kind::runner: return runner_reward + 2u * (uint32_t)wave;
    case actor_kind::boss: return 150u + 10u * (uint32_t)wave;
    default: return 10u + 2u * (uint32_t)wave;
  }
}

uint32_t Sim::price_for(uint32_t base, uint8_t lvl) {
  return base + LVL_PRICE_STEP * (uint32_t)lvl;
}

bool Sim::buy_heal(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS || !alive(p) || s.players[p].hp >= PLAYER_HP_MAX ||
      s.points < PRICE_HEAL) {
    s.last_event = event::denied;
    return false;
  }
  s.points -= PRICE_HEAL;
  s.players[p].hp = (uint8_t)(s.players[p].hp + 2 > PLAYER_HP_MAX ? PLAYER_HP_MAX : s.players[p].hp + 2);
  s.last_event = event::buy_heal;
  return true;
}

bool Sim::buy_damage(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS) {
    s.last_event = event::denied;
    return false;
  }
  if (s.dmg_lvl[p] >= MAX_LVL) {
    s.last_event = event::denied;
    return false;
  }
  const uint32_t price = price_for(PRICE_DMG, s.dmg_lvl[p]);
  if (s.points < price) {
    s.last_event = event::denied;
    return false;
  }
  s.points -= price;
  ++s.dmg_lvl[p];
  s.last_event = event::buy_dmg;
  return true;
}

bool Sim::buy_speed(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS) {
    s.last_event = event::denied;
    return false;
  }
  if (s.spd_lvl[p] >= MAX_LVL) {
    s.last_event = event::denied;
    return false;
  }
  const uint32_t price = price_for(PRICE_SPD, s.spd_lvl[p]);
  if (s.points < price) {
    s.last_event = event::denied;
    return false;
  }
  s.points -= price;
  ++s.spd_lvl[p];
  s.last_event = event::buy_spd;
  return true;
}

bool Sim::buy_rapid(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS) {
    s.last_event = event::denied;
    return false;
  }
  if (s.rpd_lvl[p] >= MAX_LVL) {
    s.last_event = event::denied;
    return false;
  }
  const uint32_t price = price_for(PRICE_RPD, s.rpd_lvl[p]);
  if (s.points < price) {
    s.last_event = event::denied;
    return false;
  }
  s.points -= price;
  ++s.rpd_lvl[p];
  s.last_event = event::buy_rpd;
  return true;
}

bool Sim::roll_roulette(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS) {
    s.last_event = event::denied;
    return false;
  }
  if (s.points < PRICE_ROLL) {
    s.last_event = event::denied;
    return false;
  }
  s.points -= PRICE_ROLL;
  s.guns[p] = roll_weapon((uint8_t)(esp_random() % 100u));
  s.ammo[p] = mag_size(s.guns[p]);
  s.reload_end[p] = 0;
  burst_left[p] = 0;
  s.last_event = event::roulette;
  return true;
}

Sim::weapon Sim::roll_weapon(uint8_t r) {
  if (r < 30) {
    return weapon::smg;
  }
  if (r < 40) {
    return weapon::pistol;
  }
  if (r < 65) {
    return weapon::shotgun;
  }
  if (r < 77) {
    return weapon::rifle;
  }
  if (r < 90) {
    return weapon::m16;
  }
  return weapon::sniper;
}

const char* Sim::gun_name(weapon w) {
  switch (w) {
    case weapon::smg: return "MP9";
    case weapon::shotgun: return "SPAS-12";
    case weapon::rifle: return "AR-15";
    case weapon::m16: return "FAMAS";
    case weapon::sniper: return "M82A1";
    default: return "GLOCK-19";
  }
}

const char* Sim::gun_name_p(uint8_t p) {
  return gun_name(s.guns[p < NUM_PLAYERS ? p : 0]);
}

uint8_t Sim::dmg_bonus(uint8_t lvl) {
  return (uint8_t)(25u * lvl);
}

uint8_t Sim::spd_bonus(uint8_t lvl) {
  return (uint8_t)(8u * lvl);
}

uint8_t Sim::rpd_cut(uint8_t lvl) {
  return (uint8_t)((1.0f - rpd_mult(lvl)) * 100.0f + 0.5f);
}
