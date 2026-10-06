#include <Arduino.h>
#include <cmath>

#include <Input.h>
#include <map.h>

#include "Sim.h"

// sim shop: wallet, permanent levels, roulette and the display bonuses. Called
// by game on INTERACT edges; exact points pay, denial stamps the denied event.

uint32_t sim::_kill_reward(actor_kind kind, uint16_t wave) {
  switch (kind) {
    case actor_kind::runner: return runner_reward + 2u * (uint32_t)wave; // +5 over a normal
    case actor_kind::boss: return 150u + 10u * (uint32_t)wave; // 200 at w5, 250 at w10
    default: return 10u + 2u * (uint32_t)wave; // base income with a mild wave slope
  }
}

uint32_t sim::price_for(uint32_t base, uint8_t lvl) {
  return base + LVL_PRICE_STEP * (uint32_t)lvl;
}

bool sim::buy_heal(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS || !_alive(p) || _s.players[p].hp >= PLAYER_HP_MAX ||
      _s.points < PRICE_HEAL) {
    _s.last_event = event::denied; // full HP, broke, or no such player
    return false;
  }
  _s.points -= PRICE_HEAL; // exact points pay
  _s.players[p].hp = (uint8_t)(_s.players[p].hp + 2 > PLAYER_HP_MAX ? PLAYER_HP_MAX
                                                                   : _s.players[p].hp + 2);
  _s.last_event = event::buy_heal;
  return true;
}

bool sim::buy_damage(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS) {
    _s.last_event = event::denied;
    return false;
  }
  if (_s.dmg_lvl[p] >= MAX_LVL) {
    _s.last_event = event::denied; // capped, like a full HP bar
    return false;
  }
  const uint32_t price = price_for(PRICE_DMG, _s.dmg_lvl[p]);
  if (_s.points < price) {
    _s.last_event = event::denied;
    return false;
  }
  _s.points -= price;
  ++_s.dmg_lvl[p]; // permanent, part of that player's build
  _s.last_event = event::buy_dmg;
  return true;
}

bool sim::buy_speed(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS) {
    _s.last_event = event::denied;
    return false;
  }
  if (_s.spd_lvl[p] >= MAX_LVL) {
    _s.last_event = event::denied; // capped, like a full HP bar
    return false;
  }
  const uint32_t price = price_for(PRICE_SPD, _s.spd_lvl[p]);
  if (_s.points < price) {
    _s.last_event = event::denied;
    return false;
  }
  _s.points -= price;
  ++_s.spd_lvl[p]; // permanent, part of that player's build
  _s.last_event = event::buy_spd;
  return true;
}

bool sim::buy_rapid(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS) {
    _s.last_event = event::denied;
    return false;
  }
  if (_s.rpd_lvl[p] >= MAX_LVL) {
    _s.last_event = event::denied; // capped, like a full HP bar
    return false;
  }
  const uint32_t price = price_for(PRICE_RPD, _s.rpd_lvl[p]);
  if (_s.points < price) {
    _s.last_event = event::denied;
    return false;
  }
  _s.points -= price;
  ++_s.rpd_lvl[p]; // permanent, part of that player's build
  _s.last_event = event::buy_rpd;
  return true;
}

bool sim::roll_roulette(uint32_t now, uint8_t p) {
  (void)now;
  if (p >= NUM_PLAYERS) {
    _s.last_event = event::denied;
    return false;
  }
  if (_s.points < PRICE_ROLL) {
    _s.last_event = event::denied;
    return false;
  }
  _s.points -= PRICE_ROLL;
  _s.guns[p] = _roll_weapon((uint8_t)(esp_random() % 100u));
  _s.ammo[p] = _mag_size(_s.guns[p]); // fresh gun, full mag
  _s.reload_end[p] = 0; // the swap interrupts a reload, like a burst
  _burst_left[p] = 0;
  _s.last_event = event::roulette;
  return true;
}

sim::weapon sim::_roll_weapon(uint8_t r) {
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

const char* sim::gun_name(weapon w) {
  switch (w) {
    case weapon::smg: return "MP9";
    case weapon::shotgun: return "SPAS-12";
    case weapon::rifle: return "AR-15";
    case weapon::m16: return "FAMAS";
    case weapon::sniper: return "M82A1";
    default: return "GLOCK-19";
  }
}

const char* sim::gun_name_p(uint8_t p) {
  return gun_name(_s.guns[p < NUM_PLAYERS ? p : 0]);
}

uint8_t sim::dmg_bonus(uint8_t lvl) {
  return (uint8_t)(25u * lvl); // +25%/level, +250% at max
}

uint8_t sim::spd_bonus(uint8_t lvl) {
  return (uint8_t)(8u * lvl); // +8%/level, +80% at max
}

uint8_t sim::rpd_cut(uint8_t lvl) {
  // the real accumulated cooldown cut in %: L1 6, L5 27, L10 46
  return (uint8_t)((1.0f - _rpd_mult(lvl)) * 100.0f + 0.5f);
}
