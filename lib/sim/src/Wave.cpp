#include <Arduino.h>
#include <cmath>

#include <Input.h>
#include <Map.h>

#include "Sim.h"

// sim wave: zombie steering, spawn tables, quotas and per-kind stats. The wave
// order (dithered runners) and the refill live here; step() only calls in

bool Sim::step_zombie(uint8_t z, float ddx, float ddy, float dt) {
  const float d = sqrtf(ddx * ddx + ddy * ddy);
  if (d <= 0.5f) {
    return false;
  }
  const float spd = speed_for(s.zombies[z].kind);
  const float bx = s.zombies[z].x, by = s.zombies[z].y;
  // move_entity takes references, so it has to get the real members, not copies
  move_entity(s.zombies[z].x, s.zombies[z].y, ddx / d * spd * dt, ddy / d * spd * dt, ZOMBIE_SIZE);
  if (s.zombies[z].x == bx && s.zombies[z].y == by) {
    return false; // walled in: keep the last facing instead of flip-flopping
  }
  face_toward(s.zombies[z].facing, zface_want[z], zface_cnt[z], s.zombies[z].x - bx, s.zombies[z].y - by);
  return true;
}

void Sim::zombie_steer(uint8_t z, float pcx, float pcy, float dt, const uint16_t f[Tilemap::ROWS][Tilemap::COLS]) {
  const float zcx = s.zombies[z].x + ZOMBIE_SIZE / 2.0f;
  const float zcy = s.zombies[z].y + ZOMBIE_SIZE / 2.0f;
  const float sdx = pcx - zcx;
  const float sdy = pcy - zcy;
  if (sdx * sdx + sdy * sdy <= contact_dist * contact_dist) {
    return; // standoff ring: contact hits land anyway, no need to pile onto the centre
  }
  int16_t ztx = (int16_t)(zcx / Tilemap::TILE);
  int16_t zty = (int16_t)(zcy / Tilemap::TILE);

  if (ztx < 0) { // keep every field[] read provably in bounds
    ztx = 0;
  } else if (ztx >= Tilemap::COLS) {
    ztx = Tilemap::COLS - 1;
  }
  if (zty < 0) {
    zty = 0;
  } else if (zty >= Tilemap::ROWS) {
    zty = Tilemap::ROWS - 1;
  }

  const uint16_t here = f[zty][ztx];

  if (here == 0) {
    // on the player's tile: close in directly. The standoff guard above stops
    // the walk at contact range, so hits land without piling onto the centre
    step_zombie(z, pcx - zcx, pcy - zcy, dt);
    return;
  }
  if (here == Tilemap::UNREACHABLE) { // walled off from the player: straight chase as a fallback
    step_zombie(z, pcx - zcx, pcy - zcy, dt);
    return;
  }

  uint8_t tried = 0;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) { // retry the next best tile if one is blocked
    int8_t best = -1;
    uint16_t best_d = here;
    for (uint8_t k = 0; k < 8; ++k) {
      if (tried & (1u << k)) {
        continue;
      }
      const int16_t nx = (int16_t)(ztx + nbr_x[k]);
      const int16_t ny = (int16_t)(zty + nbr_y[k]);
      if (nx < 0 || nx >= Tilemap::COLS || ny < 0 || ny >= Tilemap::ROWS) {
        continue;
      }
      const uint16_t d = f[ny][nx];
      if (d < best_d) {
        best_d = d;
        best = (int8_t)k;
      }
    }
    if (best < 0) {
      return;
    }
    tried = (uint8_t)(tried | (1u << best));

    // aim at the centre of the chosen tile, so a diagonal reads as drift not a shuffle
    const float tx_c = (float)(ztx + nbr_x[best]) * Tilemap::TILE + Tilemap::TILE / 2.0f;
    const float ty_c = (float)(zty + nbr_y[best]) * Tilemap::TILE + Tilemap::TILE / 2.0f;
    if (step_zombie(z, tx_c - zcx, ty_c - zcy, dt)) {
      return;
    }
  }
}

void Sim::spawn_wave() {
  ++s.wave;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (!s.players[p].active || s.players[p].hp > 0) {
      continue;
    }
    if (s.players[p].downed) {
      s.players[p].downed = false; // held on till the wave broke: up at revive HP
      s.players[p].bleed = 0;
      s.players[p].hp = REVIVE_HP;
      bleed_acc[p] = 0;
    } else {
      respawn(p); // the bled-out rejoin every wave
      s.players[p].hp = REVIVE_HP; // ...back at 3 HP, not full
    }
  }
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    s.zombies[i].active = false;
  }

  // quota: wave+3 kills to clear, uncapped; at most MAX_ZOMBIES alive at once
  // composition: the boss steals spawn idx 0 every 5th wave, then up to half the
  // quota (from wave 2) are runners spread evenly over the wave (dither, so the
  // wave never opens with a runner wall), the rest normals. 10 alive max, always
  wave_quota = wave_total(s.wave);
  wave_spawned = 0;
  const bool boss = wave_boss(s.wave);
  const uint8_t runners = wave_runners(s.wave, wave_quota);

  const uint8_t initial = wave_quota > MAX_ZOMBIES ? MAX_ZOMBIES : (uint8_t)wave_quota;
  for (uint8_t i = 0; i < initial; ++i) {
    spawn_into(i, wave_kind(wave_spawned, boss, runners, wave_quota));
    ++wave_spawned;
  }
  s.last_event = event::wave;
}

Sim::actor_kind Sim::wave_kind(uint16_t idx, bool boss, uint8_t runners, uint16_t total) {
  if (boss && idx == 0) {
    return actor_kind::boss;
  }
  const uint16_t slots = (uint16_t)(total - (boss ? 1u : 0u)); // non-boss slots
  const uint16_t i = (uint16_t)(idx - (boss ? 1u : 0u));
  // dither over the wave: exactly `runners` spread evenly (u32 math, quota is uncapped)
  if ((uint32_t)i * (uint32_t)runners % (uint32_t)slots < (uint32_t)runners) {
    return actor_kind::runner;
  }
  return actor_kind::normal;
}

void Sim::spawn_into(uint8_t slot, actor_kind kind) {
  const float px0 = s.players[0].x + PLAYER_SIZE / 2.0f;
  const float py0 = s.players[0].y + PLAYER_SIZE / 2.0f;
  const float px1 = s.players[1].x + PLAYER_SIZE / 2.0f;
  const float py1 = s.players[1].y + PLAYER_SIZE / 2.0f;
  const bool p1_out = alive(1);
  const uint16_t total = (uint16_t)(Tilemap::COLS * Tilemap::ROWS);
  const int16_t off = (int16_t)((Tilemap::TILE - ZOMBIE_SIZE) / 2);

  // scan every tile from a random offset, so a spot is always found
  const uint16_t start = (uint16_t)(esp_random() % total);
  for (uint16_t k = 0; k < total; ++k) {
    const uint16_t idx = (uint16_t)((start + k) % total);
    const int16_t tx = (int16_t)(idx % Tilemap::COLS);
    const int16_t ty = (int16_t)(idx / Tilemap::COLS);
    if (Tilemap::solid(tx, ty)) {
      continue;
    }
    const float x = (float)(tx * Tilemap::TILE + off);
    const float y = (float)(ty * Tilemap::TILE + off);
    const float dx0 = x - px0;
    const float dy0 = y - py0;
    bool close = dx0 * dx0 + dy0 * dy0 < (float)spawn_min_d2;
    if (!close && p1_out) { // keep clear of both players, not just player 1
      const float dx1 = x - px1;
      const float dy1 = y - py1;
      close = dx1 * dx1 + dy1 * dy1 < (float)spawn_min_d2;
    }
    if (close && k + 1 < total) {
      continue; // too close to a player, keep looking
    }
    s.zombies[slot] = { x, y, zombie_hp(kind, s.wave), true, kind, 2 };
    zface_want[slot] = 2;
    zface_cnt[slot] = 0;
    break;
  }
}

uint16_t Sim::wave_total(uint16_t wave) {
  return wave + 3u; // quota, uncapped: the alive cap is enforced at spawn time
}

uint8_t Sim::wave_runners(uint16_t wave, uint16_t total) {
  if (wave < 2) {
    return 0; // gentle start: wave 1 is all normals
  }
  uint16_t runners = (2u * wave) / 3u;
  if (runners > total / 2u) {
    runners = total / 2u;
  }
  return (uint8_t)runners;
}

bool Sim::wave_boss(uint16_t wave) {
  return wave % 5u == 0u;
}

uint8_t Sim::zombie_hp(actor_kind kind, uint16_t wave) {
  switch (kind) {
    case actor_kind::runner: return (uint8_t)(1u + wave / 4u); // 2 hits from w4, 3 from w8
    case actor_kind::boss: { // 25 at w5, 30 at w10, capped: hp rides one byte
      const uint16_t h = 20u + wave;
      return h > 255u ? 255u : (uint8_t)h;
    }
    default: return (uint8_t)(2u + wave / 2u); // tougher every two waves
  }
}

uint8_t Sim::zombie_dmg(actor_kind kind) {
  // the tank mauls (one-shots a freshly revived 3HP player), the rest scratch
  return (kind == actor_kind::boss) ? 3 : 1;
}

float Sim::speed_for(actor_kind kind) {
  switch (kind) {
    case actor_kind::runner: return runner_speed;
    case actor_kind::boss: return boss_speed;
    default: return zombie_speed;
  }
}
