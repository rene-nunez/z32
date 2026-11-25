// Weapons, fire cadence, magazines and reloads

#include <Arduino.h>
#include <cmath>

#include "Sim.h"

uint8_t Sim::eff_dmg(uint8_t base, uint8_t lvl) {
  const float dmg = (float)base * (1.0f + 0.25f * (float)lvl);
  const uint8_t eff = (uint8_t)(dmg + 0.5f);
  return eff < 1 ? 1 : eff;
}

float Sim::spd_mult(uint8_t lvl) {
  return 1.0f + 0.08f * (float)lvl;
}

float Sim::rpd_mult(uint8_t lvl) {
  float m = 1.0f;
  for (uint8_t i = 0; i < lvl; ++i) {
    m *= 0.94f;
  }
  return m;
}

uint32_t Sim::fire_cd(weapon w) {
  switch (w) {
    case weapon::smg: return 180;
    case weapon::shotgun: return 900;
    case weapon::rifle: return 350;
    case weapon::m16: return 600;
    case weapon::sniper: return 1400;
    default: return 500;
  }
}

uint8_t Sim::base_dmg(weapon w) {
  switch (w) {
    case weapon::rifle: return 3;
    case weapon::sniper: return 6;
    case weapon::shotgun: return 2;
    default: return 1;
  }
}

float Sim::weapon_range(weapon w) {
  return (w == weapon::sniper) ? sniper_range : fire_range;
}

uint8_t Sim::fire_one(uint32_t now, float dx, float dy, uint8_t dmg, uint8_t p) {
  const float bx = s.players[p].x + PLAYER_SIZE / 2.0f;
  const float by = s.players[p].y + PLAYER_SIZE / 2.0f;
  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    if (!s.bullets[i].active) {
      s.bullets[i] = {bx, by, dx * bullet_speed, dy * bullet_speed, dmg, true};
      last_shot[p] = now;
      return 1;
    }
  }
  return 0;
}

uint8_t Sim::mag_size(weapon w) {
  switch (w) {
    case weapon::smg: return 30;
    case weapon::shotgun: return 8;
    case weapon::rifle: return 30;
    case weapon::m16: return 30;
    case weapon::sniper: return 10;
    default: return 15;
  }
}

uint32_t Sim::reload_ms(weapon w) {
  switch (w) {
    case weapon::sniper: return 2000;
    case weapon::shotgun: return 1500;
    default: return 1000;
  }
}

bool Sim::reloading(uint8_t p) {
  return p < NUM_PLAYERS && s.reload_end[p] != 0;
}

bool Sim::start_reload(uint32_t now, uint8_t p) {
  if (p >= NUM_PLAYERS || !alive(p) || reloading(p) ||
      s.ammo[p] >= mag_size(s.guns[p])) {
    return false;
  }
  s.reload_end[p] = now + reload_ms(s.guns[p]);
  burst_left[p] = 0;
  s.last_event = event::reload;
  return true;
}

void Sim::do_fire(uint32_t now, uint8_t p) {
  if (reloading(p)) {
    return;
  }
  if (s.ammo[p] == 0) {
    start_reload(now, p);
    return;
  }
  const uint32_t cd = (uint32_t)((float)fire_cd(s.guns[p]) * rpd_mult(s.rpd_lvl[p]));
  if (now - last_shot[p] < (cd < 50 ? 50 : cd)) {
    return;
  }
  if (fire_single(now, p) && s.guns[p] == weapon::m16) {
    burst_left[p] = (uint8_t)(burst_shots - 1u);
    burst_next[p] = now + burst_gap_ms;
  }
}

bool Sim::fire_single(uint32_t now, uint8_t p) {
  const float range = weapon_range(s.guns[p]);
  const float ox = s.players[p].x;
  const float oy = s.players[p].y;
  int16_t best = -1;
  float best_d = range * range;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    if (!s.zombies[i].active) {
      continue;
    }
    const float dx = s.zombies[i].x - ox;
    const float dy = s.zombies[i].y - oy;
    const float d = dx * dx + dy * dy;
    if (d <= best_d) {
      best_d = d;
      best = (int16_t)i;
    }
  }
  if (best < 0) {
    return false;
  }
  if (s.ammo[p] == 0) {
    start_reload(now, p);
    return false;
  }

  const float bx = ox + PLAYER_SIZE / 2.0f;
  const float by = oy + PLAYER_SIZE / 2.0f;
  float dx = s.zombies[best].x + ZOMBIE_SIZE / 2.0f - bx;
  float dy = s.zombies[best].y + ZOMBIE_SIZE / 2.0f - by;
  const float len = sqrtf(dx * dx + dy * dy);
  dx /= len;
  dy /= len;
  s.players[p].facing = dir_of(dx, dy);
  last_aim[p] = now;

  const uint8_t dmg = eff_dmg(base_dmg(s.guns[p]), s.dmg_lvl[p]);
  uint8_t fired = 0;
  if (s.guns[p] == weapon::shotgun) {
    constexpr float c = 0.988771f;
    constexpr float s = 0.149438f;
    fired += fire_one(now, dx, dy, dmg, p);
    fired += fire_one(now, dx * c - dy * s, dx * s + dy * c, dmg, p);
    fired += fire_one(now, dx * c + dy * s, -dx * s + dy * c, dmg, p);
  } else {
    fired = fire_one(now, dx, dy, dmg, p);
  }
  if (fired > 0) {
    --s.ammo[p];
    s.last_event = event::shoot;
    if (s.ammo[p] == 0) {
      start_reload(now, p);
    }
  }
  return fired > 0;
}

bool Sim::auto_fire(weapon w) {
  return w == weapon::smg || w == weapon::rifle;
}
