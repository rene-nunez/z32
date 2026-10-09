#include <Arduino.h>
#include <cmath>

#include <input.h>
#include <map.h>

#include "sim.h"

// sim combat: weapon tables, fire pipeline, mags and reloads. Damage scales
// with the owner's dmg_lvl, cadence with rpd_lvl; step() only calls in

uint8_t sim::_eff_dmg(uint8_t base, uint8_t lvl) {
  const float dmg = (float)base * (1.0f + 0.25f * (float)lvl);
  const uint8_t eff = (uint8_t)(dmg + 0.5f); // half-up: 1-dmg guns step up at lvl 2 and 4
  return eff < 1 ? 1 : eff;
}

float sim::_spd_mult(uint8_t lvl) {
  return 1.0f + 0.08f * (float)lvl; // +8%/level, +80% at max (balance pass owns the curve)
}

float sim::_rpd_mult(uint8_t lvl) {
  // multiplicative: ~-6%/level compounding, 0.54 at max, no dead levels past a floor
  float m = 1.0f;
  for (uint8_t i = 0; i < lvl; ++i) {
    m *= 0.94f;
  }
  return m;
}

uint32_t sim::_fire_cd(weapon w) {
  switch (w) {
    case weapon::smg: return 180;
    case weapon::shotgun: return 900;
    case weapon::rifle: return 350; // laser: fast mid punch, still below the MP9 hose
    case weapon::m16: return 600;   // gap between bursts, rounds tick at burst_gap_ms
    case weapon::sniper: return 1400;
    default: return 500; // pistol
  }
}

uint8_t sim::_base_dmg(weapon w) {
  switch (w) {
    case weapon::rifle: return 3; // steady mid punch, one-shots early waves
    case weapon::sniper: return 6; // one heavy round, worth the wait
    case weapon::shotgun: return 2; // each pellet: 6/trigger split over 3 bodies
    default: return 1; // pistol, smg and m16
  }
}

float sim::_fire_range(weapon w) {
  return (w == weapon::sniper) ? sniper_range : fire_range;
}

uint8_t sim::_fire_one(uint32_t now, float dx, float dy, uint8_t dmg, uint8_t p) {
  const float bx = _s.players[p].x + PLAYER_SIZE / 2.0f;
  const float by = _s.players[p].y + PLAYER_SIZE / 2.0f;
  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    if (!_s.bullets[i].active) {
      _s.bullets[i] = {bx, by, dx * bullet_speed, dy * bullet_speed, dmg, true};
      _last_shot[p] = now;
      return 1;
    }
  }
  return 0; // rack is full: keep the cooldown so the next press retries
}

uint8_t sim::_mag_size(weapon w) {
  switch (w) {
    case weapon::smg: return 30;
    case weapon::shotgun: return 8;
    case weapon::rifle: return 30;
    case weapon::m16: return 30;
    case weapon::sniper: return 10;
    default: return 15; // pistol
  }
}

uint32_t sim::_reload_ms(weapon w) {
  switch (w) {
    case weapon::sniper: return 2000; // heavy mag, worth the wait
    case weapon::shotgun: return 1500; // shells, one by one
    default: return 1000;
  }
}

bool sim::reloading(uint8_t p) {
  return p < NUM_PLAYERS && _s.reload_end[p] != 0;
}

bool sim::start_reload(uint32_t now, uint8_t p) {
  if (p >= NUM_PLAYERS || !_alive(p) || reloading(p) ||
      _s.ammo[p] >= _mag_size(_s.guns[p])) {
    return false; // no such player, busy, or nothing to top up
  }
  _s.reload_end[p] = now + _reload_ms(_s.guns[p]);
  _burst_left[p] = 0; // the swap interrupts a burst mid-flight
  _s.last_event = event::reload;
  return true;
}

void sim::_do_fire(uint32_t now, uint8_t p) {
  if (reloading(p)) {
    return; // hands busy with the mag
  }
  if (_s.ammo[p] == 0) {
    start_reload(now, p); // auto on empty: the swap starts itself, no dry click
    return;
  }
  const uint32_t cd = (uint32_t)((float)_fire_cd(_s.guns[p]) * _rpd_mult(_s.rpd_lvl[p]));
  if (now - _last_shot[p] < (cd < 50 ? 50 : cd)) {
    return;
  }
  if (_fire_single(now, p) && _s.guns[p] == weapon::m16) {
    _burst_left[p] = (uint8_t)(burst_shots - 1u); // first round out, the rest tick in step()
    _burst_next[p] = now + burst_gap_ms;
  }
}

bool sim::_fire_single(uint32_t now, uint8_t p) {
  const float range = _fire_range(_s.guns[p]);
  const float ox = _s.players[p].x;
  const float oy = _s.players[p].y;
  int16_t best = -1;
  float best_d = range * range;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    if (!_s.zombies[i].active) {
      continue;
    }
    const float dx = _s.zombies[i].x - ox;
    const float dy = _s.zombies[i].y - oy;
    const float d = dx * dx + dy * dy;
    if (d <= best_d) {
      best_d = d;
      best = (int16_t)i;
    }
  }
  if (best < 0) {
    return false; // nothing in reach: no round leaves, no burst is armed
  }
  if (_s.ammo[p] == 0) {
    start_reload(now, p); // burst tick ran dry mid-flight: swap, don't spin retrying
    return false;
  }

  const float bx = ox + PLAYER_SIZE / 2.0f;
  const float by = oy + PLAYER_SIZE / 2.0f;
  float dx = _s.zombies[best].x + ZOMBIE_SIZE / 2.0f - bx;
  float dy = _s.zombies[best].y + ZOMBIE_SIZE / 2.0f - by;
  const float len = sqrtf(dx * dx + dy * dy);
  dx /= len;
  dy /= len;
  _s.players[p].facing = dir_of(dx, dy); // aim wins over the move dir
  _last_aim[p] = now;

  const uint8_t dmg = _eff_dmg(_base_dmg(_s.guns[p]), _s.dmg_lvl[p]);
  uint8_t fired = 0;
  if (_s.guns[p] == weapon::shotgun) {
    // 3 pellets fanned around the aim: straight, -0.15rad, +0.15rad
    constexpr float c = 0.988771f; // cos(0.15)
    constexpr float s = 0.149438f; // sin(0.15)
    fired += _fire_one(now, dx, dy, dmg, p);
    fired += _fire_one(now, dx * c - dy * s, dx * s + dy * c, dmg, p);
    fired += _fire_one(now, dx * c + dy * s, -dx * s + dy * c, dmg, p);
  } else {
    fired = _fire_one(now, dx, dy, dmg, p);
  }
  if (fired > 0) {
    --_s.ammo[p]; // one trigger pull, one round (pellets ride the same shell)
    _s.last_event = event::shoot;
    if (_s.ammo[p] == 0) {
      start_reload(now, p); // last round out: swap starts at once (reload wins the jingle)
    }
  }
  return fired > 0;
}

bool sim::auto_fire(weapon w) {
  return w == weapon::smg || w == weapon::rifle; // MP9 + AR-15 hold, rest press
}
