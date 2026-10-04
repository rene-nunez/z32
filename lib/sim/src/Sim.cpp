#include <Arduino.h>
#include <cmath>

#include <Input.h>
#include <map.h>

#include "Sim.h"

sim::state sim::_s;
uint32_t sim::_last_ms = 0;
uint32_t sim::_last_shot[sim::NUM_PLAYERS] = {0, 0};
uint8_t sim::_burst_left[sim::NUM_PLAYERS] = {0, 0};
uint32_t sim::_burst_next[sim::NUM_PLAYERS] = {0, 0};
uint32_t sim::_last_damage[sim::NUM_PLAYERS] = {0, 0};
uint32_t sim::_bleed_acc[sim::NUM_PLAYERS] = {0, 0};
int16_t sim::_path_tx[sim::NUM_PLAYERS] = {-1, -1};
int16_t sim::_path_ty[sim::NUM_PLAYERS] = {-1, -1};
uint32_t sim::_last_aim[sim::NUM_PLAYERS] = {0, 0};
uint8_t sim::_face_want[sim::NUM_PLAYERS] = {2, 2};
uint8_t sim::_face_cnt[sim::NUM_PLAYERS] = {0, 0};
uint8_t sim::_zface_want[sim::MAX_ZOMBIES] = {2, 2, 2, 2, 2, 2, 2, 2};
uint8_t sim::_zface_cnt[sim::MAX_ZOMBIES] = {0};
sim::ctl sim::_p2ctl = {};

constexpr int8_t sim::nbr_x[8];
constexpr int8_t sim::nbr_y[8];

void sim::reset() {
  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    _s.bullets[i].active = false;
  }

  _s.kills = 0;
  _s.wave = 0;
  _s.points = 0;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    _s.guns[p] = weapon::pistol;
    _s.dmg_lvl[p] = 0;
    _s.spd_lvl[p] = 0;
    _s.rpd_lvl[p] = 0;
    _s.ammo[p] = _mag_size(weapon::pistol);
    _s.reload_end[p] = 0;
  }
  _s.last_event = event::none;
  _last_shot[0] = _last_shot[1] = 0;
  _burst_left[0] = _burst_left[1] = 0;
  _burst_next[0] = _burst_next[1] = 0;
  _last_aim[0] = _last_aim[1] = 0;
  _face_want[0] = _face_want[1] = 2;
  _face_cnt[0] = _face_cnt[1] = 0;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    _zface_want[i] = 2;
    _zface_cnt[i] = 0;
  }
  _last_damage[0] = _last_damage[1] = 0;
  _bleed_acc[0] = _bleed_acc[1] = 0;
  _p2ctl = {};

  _s.players[0] = {
      (float)tilemap::spawn_px - PLAYER_SIZE / 2.0f,
      (float)tilemap::spawn_py - PLAYER_SIZE / 2.0f,
      PLAYER_HP_MAX,
      true,
      false,
      0,
      2, // face south at spawn
  };
  _s.players[1] = {_s.players[0].x, _s.players[0].y, 0, false, false, 0,
                   2}; // Solo: no peer yet

  _path_tx[0] = _path_tx[1] = -1; // force fresh fields at the new spawn
  _path_ty[0] = _path_ty[1] = -1;

  _spawn_wave();
}

bool sim::_alive(uint8_t p) {
  return _s.players[p].active && !_s.players[p].downed && _s.players[p].hp > 0;
}

bool sim::_downed(uint8_t p) {
  return _s.players[p].active && _s.players[p].downed;
}

bool sim::revive(uint8_t p) {
  if (!_downed(p)) {
    return false;
  }
  _s.players[p].downed = false;
  _s.players[p].bleed = 0;
  _s.players[p].hp = REVIVE_HP;
  _bleed_acc[p] = 0;
  _last_damage[p] = 0; // a breath before the next hit lands
  _s.last_event = event::revive;
  return true;
}

void sim::set_p2_active(bool active) {
  _s.players[1].active = active;
  if (active) {
    _respawn(1);
    _s.guns[1] = weapon::pistol; // fresh join: own loadout from scratch
    _s.dmg_lvl[1] = 0;
    _s.spd_lvl[1] = 0;
    _s.rpd_lvl[1] = 0;
    _s.ammo[1] = _mag_size(weapon::pistol);
    _s.reload_end[1] = 0;
    _burst_left[1] = 0;
  } else {
    _s.players[1].hp = 0;
  }
  _path_tx[1] = -1; // its field must rebuild for the new state
  _path_ty[1] = -1;
}

void sim::set_p2(const ctl& c) {
  _p2ctl = c;
}

void sim::_respawn(uint8_t p) {
  _s.players[p].x = (float)tilemap::spawn_px - PLAYER_SIZE / 2.0f + (float)(p * (PLAYER_SIZE + 2));
  _s.players[p].y = (float)tilemap::spawn_py - PLAYER_SIZE / 2.0f;
  _s.players[p].hp = PLAYER_HP_MAX; // fresh spawn at full (join); wave rejoins cut to 1 HP below
  _s.players[p].downed = false;
  _s.players[p].bleed = 0;
  _s.players[p].facing = 2;
  _bleed_acc[p] = 0;
  _last_damage[p] = 0;
}

uint8_t sim::dir_of(float dx, float dy) {
  // screen coords (+y south): 0=E 1=SE 2=S 3=SW 4=W 5=NW 6=N 7=NE
  const float a = atan2f(dy, dx) * 57.29578f; // -180..180
  return (uint8_t)(((int)(a + 360.0f + 22.5f) / 45) & 7);
}

void sim::_face_toward(uint8_t& facing, uint8_t& want, uint8_t& cnt, float dx, float dy) {
  const uint8_t sector = dir_of(dx, dy);
  if (sector == facing) {
    want = sector; // already there: nothing pending
    cnt = 0;
    return;
  }
  if (sector != want) {
    want = sector; // first sighting, needs repeats to count
    cnt = 1;
    return;
  }
  if (++cnt >= FACE_FRAMES) {
    facing = sector; // held long enough: a real turn, not border noise
  }
}

void sim::_move_entity(float& x, float& y, float dx, float dy, uint8_t size) {
  x = constrain(x, 0.0f, (float)(tilemap::WORLD_W - size));
  y = constrain(y, 0.0f, (float)(tilemap::WORLD_H - size));

  const float nx = x + dx;
  if (!tilemap::solid_rect((int16_t)nx, (int16_t)y, size, size)) {
    x = nx;
  }
  const float ny = y + dy;
  if (!tilemap::solid_rect((int16_t)x, (int16_t)ny, size, size)) {
    y = ny;
  }
}

bool sim::_step_zombie(uint8_t z, float ddx, float ddy, float dt) {
  const float d = sqrtf(ddx * ddx + ddy * ddy);
  if (d <= 0.5f) {
    return false;
  }
  const float spd = _zombie_speed(_s.zombies[z].kind);
  const float bx = _s.zombies[z].x, by = _s.zombies[z].y;
  // _move_entity takes references, so it has to get the real members, not copies
  _move_entity(_s.zombies[z].x, _s.zombies[z].y, ddx / d * spd * dt, ddy / d * spd * dt,
               ZOMBIE_SIZE);
  if (_s.zombies[z].x == bx && _s.zombies[z].y == by) {
    return false; // walled in: keep the last facing instead of flip-flopping
  }
  _face_toward(_s.zombies[z].facing, _zface_want[z], _zface_cnt[z], _s.zombies[z].x - bx,
               _s.zombies[z].y - by);
  return true;
}

void sim::_zombie_steer(uint8_t z, float pcx, float pcy, float dt,
                         const uint16_t f[tilemap::ROWS][tilemap::COLS]) {
  const float zcx = _s.zombies[z].x + ZOMBIE_SIZE / 2.0f;
  const float zcy = _s.zombies[z].y + ZOMBIE_SIZE / 2.0f;
  const float sdx = pcx - zcx;
  const float sdy = pcy - zcy;
  if (sdx * sdx + sdy * sdy <= contact_dist * contact_dist) {
    return; // standoff ring: contact hits land anyway, no need to pile onto the centre
  }
  int16_t ztx = (int16_t)(zcx / tilemap::TILE);
  int16_t zty = (int16_t)(zcy / tilemap::TILE);

  if (ztx < 0) { // keep every field[] read provably in bounds
    ztx = 0;
  } else if (ztx >= tilemap::COLS) {
    ztx = tilemap::COLS - 1;
  }
  if (zty < 0) {
    zty = 0;
  } else if (zty >= tilemap::ROWS) {
    zty = tilemap::ROWS - 1;
  }

  const uint16_t here = f[zty][ztx];

  if (here == 0) {
    // on the player's tile: close in directly. The standoff guard above stops
    // the walk at contact range, so hits land without piling onto the centre.
    _step_zombie(z, pcx - zcx, pcy - zcy, dt);
    return;
  }
  if (here == tilemap::UNREACHABLE) { // walled off from the player: straight chase as a fallback
    _step_zombie(z, pcx - zcx, pcy - zcy, dt);
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
      if (nx < 0 || nx >= tilemap::COLS || ny < 0 || ny >= tilemap::ROWS) {
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
    const float tx_c = (float)(ztx + nbr_x[best]) * tilemap::TILE + tilemap::TILE / 2.0f;
    const float ty_c = (float)(zty + nbr_y[best]) * tilemap::TILE + tilemap::TILE / 2.0f;
    if (_step_zombie(z, tx_c - zcx, ty_c - zcy, dt)) {
      return;
    }
  }
}


void sim::_spawn_wave() {
  ++_s.wave;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (!_s.players[p].active || _s.players[p].hp > 0) {
      continue;
    }
    if (_s.players[p].downed) {
      _s.players[p].downed = false; // held on till the wave broke: up at revive HP
      _s.players[p].bleed = 0;
      _s.players[p].hp = REVIVE_HP;
      _bleed_acc[p] = 0;
    } else {
      _respawn(p); // the bled-out rejoin every wave
      _s.players[p].hp = REVIVE_HP; // ...back at 1 HP, not full
    }
  }
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    _s.zombies[i].active = false;
  }

  const uint8_t count = _wave_total(_s.wave);
  // composition: the boss steals slot 0 every 5th wave, then up to half the
  // wave (from wave 2) are runners, the rest normals. 8 slots max, always.
  const bool boss = _wave_boss(_s.wave);
  const uint8_t runners = _wave_runners(_s.wave, count);
  const float px0 = _s.players[0].x + PLAYER_SIZE / 2.0f;
  const float py0 = _s.players[0].y + PLAYER_SIZE / 2.0f;
  const float px1 = _s.players[1].x + PLAYER_SIZE / 2.0f;
  const float py1 = _s.players[1].y + PLAYER_SIZE / 2.0f;
  const bool p1_out = _alive(1);
  const uint16_t total = (uint16_t)(tilemap::COLS * tilemap::ROWS);
  const int16_t off = (int16_t)((tilemap::TILE - ZOMBIE_SIZE) / 2);

  for (uint8_t i = 0; i < count; ++i) {
    const actor_kind kind = (boss && i == 0) ? actor_kind::boss
        : (i < (uint8_t)(runners + (boss ? 1u : 0u))) ? actor_kind::runner
                                                     : actor_kind::normal;
    // scan every tile from a random offset, so a spot is always found
    const uint16_t start = (uint16_t)(esp_random() % total);
    for (uint16_t k = 0; k < total; ++k) {
      const uint16_t idx = (uint16_t)((start + k) % total);
      const int16_t tx = (int16_t)(idx % tilemap::COLS);
      const int16_t ty = (int16_t)(idx / tilemap::COLS);
      if (tilemap::solid(tx, ty)) {
        continue;
      }
      const float x = (float)(tx * tilemap::TILE + off);
      const float y = (float)(ty * tilemap::TILE + off);
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
      _s.zombies[i] = { x, y, _zombie_hp(kind, _s.wave), true, kind, 2 };
      _zface_want[i] = 2;
      _zface_cnt[i] = 0;
      break;
    }
  }
  _s.last_event = event::wave;
}

uint8_t sim::_wave_total(uint16_t wave) {
  const uint16_t total = wave + 3u;
  return total > MAX_ZOMBIES ? MAX_ZOMBIES : (uint8_t)total;
}

uint8_t sim::_wave_runners(uint16_t wave, uint8_t total) {
  if (wave < 2) {
    return 0; // gentle start: wave 1 is all normals
  }
  uint16_t runners = wave / 2u;
  if (runners > total / 2u) {
    runners = (uint16_t)(total / 2u);
  }
  return (uint8_t)runners;
}

bool sim::_wave_boss(uint16_t wave) {
  return wave % 5u == 0u;
}

uint8_t sim::_zombie_hp(actor_kind kind, uint16_t wave) {
  switch (kind) {
    case actor_kind::runner: return (uint8_t)(1u + wave / 6u); // frail long, 2 hits from w6
    case actor_kind::boss: { // 25 at w5, 30 at w10, capped: hp rides one byte
      const uint16_t h = 20u + wave;
      return h > 255u ? 255u : (uint8_t)h;
    }
    default: return (uint8_t)(2u + wave / 2u); // tougher every two waves
  }
}

uint8_t sim::_zombie_dmg(actor_kind kind) {
  return (kind == actor_kind::boss) ? 2 : 1; // the tank hits back, the rest scratch
}

float sim::_zombie_speed(actor_kind kind) {
  switch (kind) {
    case actor_kind::runner: return runner_speed;
    case actor_kind::boss: return boss_speed;
    default: return zombie_speed;
  }
}

uint8_t sim::_eff_dmg(uint8_t base, uint8_t lvl) {
  const float dmg = (float)base * (1.0f + 0.25f * (float)lvl);
  const uint8_t eff = (uint8_t)(dmg + 0.5f); // half-up: 1-dmg guns step up at lvl 2 and 4
  return eff < 1 ? 1 : eff;
}

uint32_t sim::_kill_reward(actor_kind kind, uint16_t wave) {
  switch (kind) {
    case actor_kind::runner: return runner_reward + 2u * (uint32_t)wave; // +5 over a normal
    case actor_kind::boss: return 150u + 10u * (uint32_t)wave; // 200 at w5, 250 at w10
    default: return 10u + 2u * (uint32_t)wave; // base income with a mild wave slope
  }
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
    default: return 1; // pistol, smg, m16 and each shotgun pellet
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
      _s.bullets[i] = {
          bx,
          by,
          dx * bullet_speed,
          dy * bullet_speed,
          dmg,
          true,
      };
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
    _s.last_event = event::empty; // dry click: RELOAD, no auto-rescue
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
    _s.last_event = event::empty; // burst tick ran dry mid-flight
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
  }
  return fired > 0;
}

bool sim::step(uint32_t now) {
  float dt = (float)(now - _last_ms) / 1000.0f;
  if (dt > 0.05f) { // clamp big gaps (serial pauses, menu)
    dt = 0.05f;
  }
  _last_ms = now;
  _s.last_event = event::none; // buys after step() overwrite this for their frame

  // move + fire per player: player 0 reads the local sticks, player 1 the net ctl
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (!_alive(p)) {
      continue;
    }
    float dx = (p == 0) ? input::jx() : _p2ctl.jx;
    float dy = (p == 0) ? input::jy() : _p2ctl.jy;

    const float len = sqrtf(dx * dx + dy * dy);
    if (len > 1.0f) { // keep diagonal speed equal
      dx /= len;
      dy /= len;
    }
    if (len < 0.2f) { // stick at rest: no slide, same threshold as facing
      dx = dy = 0.0f;
    } else if (now - _last_aim[p] > AIM_HOLD_MS) {
      _face_toward(_s.players[p].facing, _face_want[p], _face_cnt[p], dx, dy);
    }

    const float spd = player_speed * _spd_mult(_s.spd_lvl[p]);
    _move_entity(_s.players[p].x, _s.players[p].y, dx * spd * dt, dy * spd * dt, PLAYER_SIZE);

    if ((p == 0) ? input::fire_pressed() : _p2ctl.fire) {
      _do_fire(now, p);
    }
    if ((p == 0) ? input::reload_pressed() : _p2ctl.reload) {
      start_reload(now, p); // manual only: no auto-rescue on empty
    }
    if (_s.reload_end[p] != 0 && now >= _s.reload_end[p]) {
      _s.reload_end[p] = 0;
      _s.ammo[p] = _mag_size(_s.guns[p]);
      _s.last_event = event::reloaded;
    }
    if (_burst_left[p] > 0) {
      if (_s.guns[p] != weapon::m16) {
        _burst_left[p] = 0; // swapped mid-burst: cancel the rest
      } else if (_s.ammo[p] == 0) {
        _burst_left[p] = 0; // ran dry mid-burst: RELOAD, don't spin retrying
      } else if (now >= _burst_next[p]) {
        if (_fire_single(now, p)) {
          --_burst_left[p];
          _burst_next[p] = now + burst_gap_ms;
        } else {
          _burst_next[p] = now + 50; // rack full or no target: retry shortly
        }
      }
    }
  }

  const float pcx[NUM_PLAYERS] = {
      _s.players[0].x + PLAYER_SIZE / 2.0f,
      _s.players[1].x + PLAYER_SIZE / 2.0f,
  };
  const float pcy[NUM_PLAYERS] = {
      _s.players[0].y + PLAYER_SIZE / 2.0f,
      _s.players[1].y + PLAYER_SIZE / 2.0f,
  };

  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    if (!_s.bullets[i].active) {
      continue;
    }
    _s.bullets[i].x += _s.bullets[i].vx * dt;
    _s.bullets[i].y += _s.bullets[i].vy * dt;
    if (_s.bullets[i].x < 0.0f || _s.bullets[i].x > (float)(tilemap::WORLD_W - BULLET_SIZE) ||
        _s.bullets[i].y < 0.0f || _s.bullets[i].y > (float)(tilemap::WORLD_H - BULLET_SIZE) ||
        tilemap::solid_rect((int16_t)_s.bullets[i].x, (int16_t)_s.bullets[i].y, BULLET_SIZE, BULLET_SIZE)) {
      _s.bullets[i].active = false;
      continue;
    }
    for (uint8_t z = 0; z < MAX_ZOMBIES; ++z) {
      if (!_s.zombies[z].active) {
        continue;
      }
      const float zcx = _s.zombies[z].x + ZOMBIE_SIZE / 2.0f;
      const float zcy = _s.zombies[z].y + ZOMBIE_SIZE / 2.0f;
      const float hdx = _s.bullets[i].x - zcx;
      const float hdy = _s.bullets[i].y - zcy;
      if (hdx * hdx + hdy * hdy <= hit_dist * hit_dist) {
        _s.bullets[i].active = false;
        const uint8_t dmg = _s.bullets[i].dmg;
        if (dmg >= _s.zombies[z].hp) {
          _s.zombies[z].hp = 0;
          _s.zombies[z].active = false;
          ++_s.kills;
          _s.points += _kill_reward(_s.zombies[z].kind, _s.wave);
        } else {
          _s.zombies[z].hp = (uint8_t)(_s.zombies[z].hp - dmg);
        }
        break;
      }
    }
  }

  // one distance field per player, rebuilt when that player's tile changes
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (!_s.players[p].active) {
      continue;
    }
    const int16_t ptx = (int16_t)pcx[p] / tilemap::TILE;
    const int16_t pty = (int16_t)pcy[p] / tilemap::TILE;
    if (ptx != _path_tx[p] || pty != _path_ty[p]) {
      _path_tx[p] = ptx;
      _path_ty[p] = pty;
      if (p == 0) {
        tilemap::build_field(ptx, pty);
      } else {
        tilemap::build_field2(ptx, pty);
      }
    }
  }

  for (uint8_t z = 0; z < MAX_ZOMBIES; ++z) {
    if (!_s.zombies[z].active) {
      continue;
    }
    float zcx = _s.zombies[z].x + ZOMBIE_SIZE / 2.0f;
    float zcy = _s.zombies[z].y + ZOMBIE_SIZE / 2.0f;

    // chase the nearest alive player, down that player's field
    uint8_t tgt = 0;
    float best = 1e30f;
    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      if (!_alive(p)) {
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
    _zombie_steer(z, pcx[tgt], pcy[tgt], dt, (tgt == 0) ? tilemap::field : tilemap::field2);
    zcx = _s.zombies[z].x + ZOMBIE_SIZE / 2.0f; // post-move: the contact test lands same-frame
    zcy = _s.zombies[z].y + ZOMBIE_SIZE / 2.0f;

    for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
      if (!_alive(p)) {
        continue;
      }
      const float cdx = pcx[p] - zcx;
      const float cdy = pcy[p] - zcy;
      if (cdx * cdx + cdy * cdy <= contact_dist * contact_dist &&
          now - _last_damage[p] >= damage_cd_ms) {
        _last_damage[p] = now;
        const uint8_t dmg = _zombie_dmg(_s.zombies[z].kind);
        if (_s.players[p].hp > dmg) {
          _s.players[p].hp = (uint8_t)(_s.players[p].hp - dmg);
        } else {
          _s.players[p].hp = 0; // down, not out: bleed-out decides the rest
          _s.players[p].downed = true;
          _s.players[p].bleed = BLEED_SECS;
          _bleed_acc[p] = 0;
        }
        _s.last_event = event::hurt;
      }
    }
  }

  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    if (!_s.zombies[i].active) {
      continue;
    }
    for (uint8_t j = (uint8_t)(i + 1u); j < MAX_ZOMBIES; ++j) {
      if (!_s.zombies[j].active) {
        continue;
      }
      float dx = _s.zombies[j].x - _s.zombies[i].x;
      float dy = _s.zombies[j].y - _s.zombies[i].y;
      float d2 = dx * dx + dy * dy;
      if (d2 >= declump_dist * declump_dist) {
        continue; // already side by side
      }
      float d = sqrtf(d2);
      if (d < 0.1f) { // same spot: split along x so the pair opens up
        dx = 1.0f;
        dy = 0.0f;
        d = 1.0f;
      }
      const float push = (declump_dist - d) / 2.0f;
      const float nx = dx / d;
      const float ny = dy / d;
      _move_entity(_s.zombies[i].x, _s.zombies[i].y, -nx * push, -ny * push,
                   ZOMBIE_SIZE);
      _move_entity(_s.zombies[j].x, _s.zombies[j].y, nx * push, ny * push,
                   ZOMBIE_SIZE);
    }
  }

  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    if (!_downed(p)) {
      continue;
    }
    _bleed_acc[p] += (uint32_t)(dt * 1000.0f);
    while (_bleed_acc[p] >= 1000 && _s.players[p].bleed > 0) {
      _bleed_acc[p] -= 1000;
      --_s.players[p].bleed;
    }
    if (_s.players[p].bleed == 0) {
      _s.players[p].downed = false; // bled out: dead until the next wave
    }
  }

  bool anyone = false;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    anyone |= _alive(p);
  }
  if (!anyone) {
    _s.last_event = event::over;
    return false; // the caller raises the game over screen, sim never touches it
  }

  bool any = false;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    any |= _s.zombies[i].active;
  }
  if (!any) {
    _spawn_wave();
  }
  return true;
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
    case weapon::rifle: return "SKS";
    case weapon::m16: return "FAMAS";
    case weapon::sniper: return "M82A1";
    default: return "GLOCK-19";
  }
}

const char* sim::gun_name_p(uint8_t p) {
  return gun_name(_s.guns[p < NUM_PLAYERS ? p : 0]);
}

void sim::snapshot(net::game_state_msg& n) {
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    n.players[p].x = net::qpos(_s.players[p].x);
    n.players[p].y = net::qpos(_s.players[p].y);
    n.players[p].hp = _s.players[p].hp;
    n.players[p].flags = (_s.players[p].active ? net::PF_ACTIVE : 0) |
                         (_s.players[p].downed ? net::PF_DOWNED : 0) |
                         ((_s.reload_end[p] != 0) ? net::PF_RELOADING : 0) |
                         (uint8_t)((_s.players[p].facing & net::PF_DIR_MASK) << net::PF_DIR_SHIFT);
    n.players[p].bleed = _s.players[p].bleed;
  }
  n.wave = _s.wave;
  n.kills = _s.kills;
  n.points = _s.points;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    n.guns[p] = (uint8_t)_s.guns[p];
    n.dmg_lvl[p] = _s.dmg_lvl[p];
    n.spd_lvl[p] = _s.spd_lvl[p];
    n.rpd_lvl[p] = _s.rpd_lvl[p];
    n.ammo[p] = _s.ammo[p];
  }
  n.event = (uint8_t)_s.last_event;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    n.zombies[i].x = net::qpos(_s.zombies[i].x);
    n.zombies[i].y = net::qpos(_s.zombies[i].y);
    n.zombies[i].hp = _s.zombies[i].hp;
    n.zombies[i].flags = (_s.zombies[i].active ? net::ZF_ACTIVE : 0) |
                         (uint8_t)((uint8_t)_s.zombies[i].kind << net::ZF_KIND_SHIFT) |
                         (uint8_t)((_s.zombies[i].facing & net::ZF_DIR_MASK) << net::ZF_DIR_SHIFT);
  }
  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    n.bullets[i].x = net::qpos(_s.bullets[i].x);
    n.bullets[i].y = net::qpos(_s.bullets[i].y);
    n.bullets[i].angle = net::qangle(_s.bullets[i].vx, _s.bullets[i].vy);
    n.bullets[i].flags = (_s.bullets[i].active ? net::BF_ACTIVE : 0) |
                         (uint8_t)(_s.bullets[i].dmg << net::BF_DMG_SHIFT);
  }
}

void sim::apply_snapshot(const net::game_state_msg& n) {
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    _s.players[p].x = net::uqpos(n.players[p].x);
    _s.players[p].y = net::uqpos(n.players[p].y);
    _s.players[p].hp = n.players[p].hp;
    _s.players[p].active = (n.players[p].flags & net::PF_ACTIVE) != 0;
    _s.players[p].downed = (n.players[p].flags & net::PF_DOWNED) != 0;
    _s.players[p].facing = (uint8_t)((n.players[p].flags >> net::PF_DIR_SHIFT) & net::PF_DIR_MASK);
    _s.players[p].bleed = n.players[p].bleed;
    // client mirror only: nonzero paints RELOADING, the host owns the real timestamp
    _s.reload_end[p] = (n.players[p].flags & net::PF_RELOADING) ? 1 : 0;
  }
  _s.wave = n.wave;
  _s.kills = n.kills;
  _s.points = n.points;
  for (uint8_t p = 0; p < NUM_PLAYERS; ++p) {
    _s.guns[p] = (weapon)n.guns[p];
    _s.dmg_lvl[p] = n.dmg_lvl[p];
    _s.spd_lvl[p] = n.spd_lvl[p];
    _s.rpd_lvl[p] = n.rpd_lvl[p];
    _s.ammo[p] = n.ammo[p];
  }
  _s.last_event = (event)n.event;
  for (uint8_t i = 0; i < MAX_ZOMBIES; ++i) {
    _s.zombies[i].x = net::uqpos(n.zombies[i].x);
    _s.zombies[i].y = net::uqpos(n.zombies[i].y);
    _s.zombies[i].hp = n.zombies[i].hp;
    _s.zombies[i].active = (n.zombies[i].flags & net::ZF_ACTIVE) != 0;
    _s.zombies[i].kind = (actor_kind)((n.zombies[i].flags >> net::ZF_KIND_SHIFT) & 0x03);
    _s.zombies[i].facing = (uint8_t)((n.zombies[i].flags >> net::ZF_DIR_SHIFT) & net::ZF_DIR_MASK);
  }
  for (uint8_t i = 0; i < MAX_BULLETS; ++i) {
    _s.bullets[i].x = net::uqpos(n.bullets[i].x);
    _s.bullets[i].y = net::uqpos(n.bullets[i].y);
    float dx, dy;
    net::uqangle(n.bullets[i].angle, dx, dy);
    _s.bullets[i].vx = dx * bullet_speed;
    _s.bullets[i].vy = dy * bullet_speed;
    _s.bullets[i].dmg = (uint8_t)((n.bullets[i].flags >> net::BF_DMG_SHIFT) & 0x1F);
    _s.bullets[i].active = (n.bullets[i].flags & net::BF_ACTIVE) != 0;
  }
}
