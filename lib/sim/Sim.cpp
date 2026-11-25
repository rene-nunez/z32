// Sim step: actors, bullets and net sync

#include <Arduino.h>
#include <cmath>

#include <Input.h>
#include <Map.h>

#include "Sim.h"

Sim::state Sim::s;
uint32_t Sim::last_ms = 0;
uint32_t Sim::last_shot[Sim::NUM_PLAYERS] = {0, 0};
uint8_t Sim::burst_left[Sim::NUM_PLAYERS] = {0, 0};
uint32_t Sim::burst_next[Sim::NUM_PLAYERS] = {0, 0};
uint32_t Sim::last_damage[Sim::NUM_PLAYERS] = {0, 0};
uint32_t Sim::bleed_acc[Sim::NUM_PLAYERS] = {0, 0};
int16_t Sim::path_tx[Sim::NUM_PLAYERS] = {-1, -1};
int16_t Sim::path_ty[Sim::NUM_PLAYERS] = {-1, -1};
uint32_t Sim::last_aim[Sim::NUM_PLAYERS] = {0, 0};
uint8_t Sim::face_want[Sim::NUM_PLAYERS] = {2, 2};
uint8_t Sim::face_cnt[Sim::NUM_PLAYERS] = {0, 0};
uint8_t Sim::zface_want[Sim::MAX_ZOMBIES] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
uint8_t Sim::zface_cnt[Sim::MAX_ZOMBIES] = {0};
uint16_t Sim::wave_quota = 0;
uint16_t Sim::wave_spawned = 0;
uint32_t Sim::wave_break_until = 0;
Sim::ctl Sim::p2ctl = {};

constexpr int8_t Sim::nbr_x[8];
constexpr int8_t Sim::nbr_y[8];

void Sim::reset() {
  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    s.bullets[i].active = false;
  }

  s.kills = 0;
  s.wave = 0;
  s.points = 0;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    s.guns[p] = weapon::pistol;
    s.dmg_lvl[p] = 0;
    s.spd_lvl[p] = 0;
    s.rpd_lvl[p] = 0;
    s.ammo[p] = mag_size(weapon::pistol);
    s.reload_end[p] = 0;
  }
  s.last_event = event::none;
  last_shot[0] = last_shot[1] = 0;
  burst_left[0] = burst_left[1] = 0;
  burst_next[0] = burst_next[1] = 0;
  last_aim[0] = last_aim[1] = 0;
  face_want[0] = face_want[1] = 2;
  face_cnt[0] = face_cnt[1] = 0;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    zface_want[i] = 2;
    zface_cnt[i] = 0;
  }
  last_damage[0] = last_damage[1] = 0;
  bleed_acc[0] = bleed_acc[1] = 0;
  p2ctl = {};

  s.players[0] = {(float)Tilemap::spawn_px - PLAYER_SIZE / 2.0f, (float)Tilemap::spawn_py - PLAYER_SIZE / 2.0f, PLAYER_HP_MAX, true, false, 0, 2};
  s.players[1] = {s.players[0].x, s.players[0].y, 0, false, false, 0, 2};

  path_tx[0] = path_tx[1] = -1;
  path_ty[0] = path_ty[1] = -1;
  wave_break_until = 0;

  spawn_wave();
}

bool Sim::alive(uint8_t p) {
  return s.players[p].active && !s.players[p].downed && s.players[p].hp > 0;
}

bool Sim::downed(uint8_t p) {
  return s.players[p].active && s.players[p].downed;
}

bool Sim::revive(uint8_t p) {
  if (!downed(p)) {
    return false;
  }
  s.players[p].downed = false;
  s.players[p].bleed = 0;
  s.players[p].hp = REVIVE_HP;
  bleed_acc[p] = 0;
  last_damage[p] = 0;
  s.last_event = event::revive;
  return true;
}

void Sim::set_p2_active(bool active) {
  s.players[1].active = active;
  if (active) {
    respawn(1);
    s.guns[1] = weapon::pistol;
    s.dmg_lvl[1] = 0;
    s.spd_lvl[1] = 0;
    s.rpd_lvl[1] = 0;
    s.ammo[1] = mag_size(weapon::pistol);
    s.reload_end[1] = 0;
    burst_left[1] = 0;
  } else {
    s.players[1].hp = 0;
  }
  path_tx[1] = -1;
  path_ty[1] = -1;
}

void Sim::set_p2(const ctl& c) {
  p2ctl = c;
}

void Sim::respawn(uint8_t p) {
  s.players[p].x = (float)Tilemap::spawn_px - PLAYER_SIZE / 2.0f + (float)(p * (PLAYER_SIZE + 2));
  s.players[p].y = (float)Tilemap::spawn_py - PLAYER_SIZE / 2.0f;
  s.players[p].hp = PLAYER_HP_MAX;
  s.players[p].downed = false;
  s.players[p].bleed = 0;
  s.players[p].facing = 2;
  bleed_acc[p] = 0;
  last_damage[p] = 0;
}

uint8_t Sim::dir_of(float dx, float dy) {
  const float a = atan2f(dy, dx) * 57.29578f;
  return (uint8_t)(((int)(a + 360.0f + 22.5f) / 45) & 7);
}

void Sim::face_toward(uint8_t& facing, uint8_t& want, uint8_t& cnt, float dx, float dy) {
  const uint8_t sector = dir_of(dx, dy);
  if (sector == facing) {
    want = sector;
    cnt = 0;
    return;
  }
  if (sector != want) {
    want = sector;
    cnt = 1;
    return;
  }
  if (++cnt >= FACE_FRAMES) {
    facing = sector;
  }
}

void Sim::move_entity(float& x, float& y, float dx, float dy, uint8_t size) {
  x = constrain(x, 0.0f, (float)(Tilemap::WORLD_W - size));
  y = constrain(y, 0.0f, (float)(Tilemap::WORLD_H - size));

  const float nx = x + dx;
  if (!Tilemap::solid_rect((int16_t)nx, (int16_t)y, size, size)) {
    x = nx;
  }
  const float ny = y + dy;
  if (!Tilemap::solid_rect((int16_t)x, (int16_t)ny, size, size)) {
    y = ny;
  }
}

bool Sim::step(uint32_t now) {
  float dt = (float)(now - last_ms) / 1000.0f;
  if (dt > 0.05f) {
    dt = 0.05f;
  }
  last_ms = now;
  s.last_event = event::none;

  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (!alive(p)) {
      continue;
    }
    float dx = (p == 0) ? Input::jx() : p2ctl.jx;
    float dy = (p == 0) ? Input::jy() : p2ctl.jy;

    const float len = sqrtf(dx * dx + dy * dy);
    if (len > 1.0f) {
      dx /= len;
      dy /= len;
    }
    if (len < 0.2f) {
      dx = dy = 0.0f;
    } else if (now - last_aim[p] > AIM_HOLD_MS) {
      face_toward(s.players[p].facing, face_want[p], face_cnt[p], dx, dy);
    }

    const float spd = player_speed * spd_mult(s.spd_lvl[p]);
    move_entity(s.players[p].x, s.players[p].y, dx * spd * dt, dy * spd * dt, PLAYER_SIZE);

    const bool held = (p == 0) ? Input::fire_down() : p2ctl.fire;
    const bool edge = (p == 0) ? Input::fire_pressed() : p2ctl.fire;
    if (auto_fire(s.guns[p]) ? held : edge) {
      do_fire(now, p);
    }
    if ((p == 0) ? Input::reload_pressed() : p2ctl.reload) {
      start_reload(now, p);
    }
    if (s.reload_end[p] != 0 && now >= s.reload_end[p]) {
      s.reload_end[p] = 0;
      s.ammo[p] = mag_size(s.guns[p]);
      s.last_event = event::reloaded;
    }
    if (burst_left[p] > 0) {
      if (s.guns[p] != weapon::m16) {
        burst_left[p] = 0;
      } else if (s.ammo[p] == 0) {
        burst_left[p] = 0;
        start_reload(now, p);
      } else if (now >= burst_next[p]) {
        if (fire_single(now, p)) {
          --burst_left[p];
          burst_next[p] = now + burst_gap_ms;
        } else {
          burst_next[p] = now + 50;
        }
      }
    }
  }

  const float pcx[NUM_PLAYERS] = {s.players[0].x + PLAYER_SIZE / 2.0f, s.players[1].x + PLAYER_SIZE / 2.0f};
  const float pcy[NUM_PLAYERS] = {s.players[0].y + PLAYER_SIZE / 2.0f, s.players[1].y + PLAYER_SIZE / 2.0f};

  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    if (!s.bullets[i].active) {
      continue;
    }
    s.bullets[i].x += s.bullets[i].vx * dt;
    s.bullets[i].y += s.bullets[i].vy * dt;
    if (s.bullets[i].x < 0.0f || s.bullets[i].x > (float)(Tilemap::WORLD_W - BULLET_SIZE) ||
        s.bullets[i].y < 0.0f || s.bullets[i].y > (float)(Tilemap::WORLD_H - BULLET_SIZE) ||
        Tilemap::solid_rect((int16_t)s.bullets[i].x, (int16_t)s.bullets[i].y, BULLET_SIZE, BULLET_SIZE)) {
      s.bullets[i].active = false;
      continue;
    }
    for (uint8_t z = 0; z < MAX_ZOMBIES; ++z) {
      if (!s.zombies[z].active) {
        continue;
      }
      const float zcx = s.zombies[z].x + ZOMBIE_SIZE / 2.0f;
      const float zcy = s.zombies[z].y + ZOMBIE_SIZE / 2.0f;
      const float hdx = s.bullets[i].x - zcx;
      const float hdy = s.bullets[i].y - zcy;
      if (hdx * hdx + hdy * hdy <= hit_dist * hit_dist) {
        s.bullets[i].active = false;
        const uint8_t dmg = s.bullets[i].dmg;
        if (dmg >= s.zombies[z].hp) {
          s.zombies[z].hp = 0;
          s.zombies[z].active = false;
          ++s.kills;
          s.points += kill_reward(s.zombies[z].kind, s.wave);
          if (wave_spawned < wave_quota) {
            const bool boss = wave_boss(s.wave);
            const uint8_t runners = wave_runners(s.wave, wave_quota);
            spawn_into(z, wave_kind(wave_spawned, boss, runners, wave_quota));
            ++wave_spawned;
          }
        } else {
          s.zombies[z].hp = (uint8_t)(s.zombies[z].hp - dmg);
        }
        break;
      }
    }
  }

  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (!s.players[p].active) {
      continue;
    }
    const int16_t ptx = (int16_t)pcx[p] / Tilemap::TILE;
    const int16_t pty = (int16_t)pcy[p] / Tilemap::TILE;
    if (ptx != path_tx[p] || pty != path_ty[p]) {
      path_tx[p] = ptx;
      path_ty[p] = pty;
      if (p == 0) {
        Tilemap::build_field(ptx, pty);
      } else {
        Tilemap::build_field2(ptx, pty);
      }
    }
  }

  for (uint8_t z = 0; z < MAX_ZOMBIES; ++z) {
    if (!s.zombies[z].active) {
      continue;
    }
    float zcx = s.zombies[z].x + ZOMBIE_SIZE / 2.0f;
    float zcy = s.zombies[z].y + ZOMBIE_SIZE / 2.0f;

    uint8_t tgt = 0;
    float best = 1e30f;
    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      if (!alive(p)) {
        continue;
      }
      const float tdx = pcx[p] - zcx;
      const float tdy = pcy[p] - zcy;
      const float d2 = tdx * tdx + tdy * tdy;
      if (d2 < best) {
        best = d2;
        tgt = p;
      }
    }
    zombie_steer(z, pcx[tgt], pcy[tgt], dt, (tgt == 0) ? Tilemap::field : Tilemap::field2);
    zcx = s.zombies[z].x + ZOMBIE_SIZE / 2.0f;
    zcy = s.zombies[z].y + ZOMBIE_SIZE / 2.0f;

    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      if (!alive(p)) {
        continue;
      }
      const float cdx = pcx[p] - zcx;
      const float cdy = pcy[p] - zcy;
      if (cdx * cdx + cdy * cdy <= contact_dist * contact_dist &&
          now - last_damage[p] >= damage_cd_ms) {
        last_damage[p] = now;
        const uint8_t dmg = zombie_dmg(s.zombies[z].kind);
        if (s.players[p].hp > dmg) {
          s.players[p].hp = (uint8_t)(s.players[p].hp - dmg);
        } else {
          s.players[p].hp = 0;
          s.players[p].downed = true;
          s.players[p].bleed = BLEED_SECS;
          bleed_acc[p] = 0;
        }
        s.last_event = event::hurt;
      }
    }
  }

  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    if (!s.zombies[i].active) {
      continue;
    }
    for (uint8_t j = (uint8_t)(i + 1u); j < MAX_ZOMBIES; ++j) {
      if (!s.zombies[j].active) {
        continue;
      }
      float dx = s.zombies[j].x - s.zombies[i].x;
      float dy = s.zombies[j].y - s.zombies[i].y;
      float d2 = dx * dx + dy * dy;
      if (d2 >= declump_dist * declump_dist) {
        continue;
      }
      float d = sqrtf(d2);
      if (d < 0.1f) {
        dx = 1.0f;
        dy = 0.0f;
        d = 1.0f;
      }
      const float push = (declump_dist - d) / 2.0f;
      const float nx = dx / d;
      const float ny = dy / d;
      move_entity(s.zombies[i].x, s.zombies[i].y, -nx * push, -ny * push, ZOMBIE_SIZE);
      move_entity(s.zombies[j].x, s.zombies[j].y, nx * push, ny * push, ZOMBIE_SIZE);
    }
  }

  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (!downed(p)) {
      continue;
    }
    bleed_acc[p] += (uint32_t)(dt * 1000.0f);
    while (bleed_acc[p] >= 1000 && s.players[p].bleed > 0) {
      bleed_acc[p] -= 1000;
      --s.players[p].bleed;
    }
    if (s.players[p].bleed == 0) {
      s.players[p].downed = false;
    }
  }

  bool anyone = false;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    anyone |= alive(p);
  }
  if (!anyone) {
    s.last_event = event::over;
    return false;
  }

  bool any = false;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    any |= s.zombies[i].active;
  }
  if (!any && wave_spawned >= wave_quota) {
    if (wave_break_until == 0) {
      wave_break_until = now + WAVE_BREAK_MS;
    } else if (now >= wave_break_until) {
      wave_break_until = 0;
      spawn_wave();
    }
  }
  return true;
}

void Sim::snapshot(Net::game_state_msg& n) {
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    n.players[p].x = Net::qpos(s.players[p].x);
    n.players[p].y = Net::qpos(s.players[p].y);
    n.players[p].hp = s.players[p].hp;
    n.players[p].flags = (s.players[p].active ? Net::PF_ACTIVE : 0) |
                         (s.players[p].downed ? Net::PF_DOWNED : 0) |
                         ((s.reload_end[p] != 0) ? Net::PF_RELOADING : 0) |
                         (uint8_t)((s.players[p].facing & Net::PF_DIR_MASK) << Net::PF_DIR_SHIFT);
    n.players[p].bleed = s.players[p].bleed;
  }
  n.wave = s.wave;
  n.kills = s.kills;
  n.points = s.points;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    n.guns[p] = (uint8_t)s.guns[p];
    n.dmg_lvl[p] = s.dmg_lvl[p];
    n.spd_lvl[p] = s.spd_lvl[p];
    n.rpd_lvl[p] = s.rpd_lvl[p];
    n.ammo[p] = s.ammo[p];
  }
  n.event = (uint8_t)s.last_event;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    n.zombies[i].x = Net::qpos(s.zombies[i].x);
    n.zombies[i].y = Net::qpos(s.zombies[i].y);
    n.zombies[i].hp = s.zombies[i].hp;
    n.zombies[i].flags = (s.zombies[i].active ? Net::ZF_ACTIVE : 0) |
                         (uint8_t)((uint8_t)s.zombies[i].kind << Net::ZF_KIND_SHIFT) |
                         (uint8_t)((s.zombies[i].facing & Net::ZF_DIR_MASK) << Net::ZF_DIR_SHIFT);
  }
  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    n.bullets[i].x = Net::qpos(s.bullets[i].x);
    n.bullets[i].y = Net::qpos(s.bullets[i].y);
    n.bullets[i].angle = Net::qangle(s.bullets[i].vx, s.bullets[i].vy);
    n.bullets[i].flags = (s.bullets[i].active ? Net::BF_ACTIVE : 0) |
                         (uint8_t)(s.bullets[i].dmg << Net::BF_DMG_SHIFT);
  }
}

void Sim::apply_snapshot(const Net::game_state_msg& n) {
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    s.players[p].x = Net::uqpos(n.players[p].x);
    s.players[p].y = Net::uqpos(n.players[p].y);
    s.players[p].hp = n.players[p].hp;
    s.players[p].active = (n.players[p].flags & Net::PF_ACTIVE) != 0;
    s.players[p].downed = (n.players[p].flags & Net::PF_DOWNED) != 0;
    s.players[p].facing = (uint8_t)((n.players[p].flags >> Net::PF_DIR_SHIFT) & Net::PF_DIR_MASK);
    s.players[p].bleed = n.players[p].bleed;
    s.reload_end[p] = (n.players[p].flags & Net::PF_RELOADING) ? 1 : 0;
  }
  s.wave = n.wave;
  s.kills = n.kills;
  s.points = n.points;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    s.guns[p] = (weapon)n.guns[p];
    s.dmg_lvl[p] = n.dmg_lvl[p];
    s.spd_lvl[p] = n.spd_lvl[p];
    s.rpd_lvl[p] = n.rpd_lvl[p];
    s.ammo[p] = n.ammo[p];
  }
  s.last_event = (event)n.event;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    s.zombies[i].x = Net::uqpos(n.zombies[i].x);
    s.zombies[i].y = Net::uqpos(n.zombies[i].y);
    s.zombies[i].hp = n.zombies[i].hp;
    s.zombies[i].active = (n.zombies[i].flags & Net::ZF_ACTIVE) != 0;
    s.zombies[i].kind = (actor_kind)((n.zombies[i].flags >> Net::ZF_KIND_SHIFT) & 0x03);
    s.zombies[i].facing = (uint8_t)((n.zombies[i].flags >> Net::ZF_DIR_SHIFT) & Net::ZF_DIR_MASK);
  }
  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    s.bullets[i].x = Net::uqpos(n.bullets[i].x);
    s.bullets[i].y = Net::uqpos(n.bullets[i].y);
    float dx, dy;
    Net::uqangle(n.bullets[i].angle, dx, dy);
    s.bullets[i].vx = dx * bullet_speed;
    s.bullets[i].vy = dy * bullet_speed;
    s.bullets[i].dmg = (uint8_t)((n.bullets[i].flags >> Net::BF_DMG_SHIFT) & 0x1F);
    s.bullets[i].active = (n.bullets[i].flags & Net::BF_ACTIVE) != 0;
  }
}
