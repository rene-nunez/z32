#include <Arduino.h>
#include <cmath>

#include <input.h>
#include <map.h>

#include "sim.h"

// sim core: state, reset, players, the frame step and net snapshots. Wave,
// combat and shop tables live in their own files; every static below is
// defined once here and used across them.

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
uint8_t sim::_zface_want[sim::MAX_ZOMBIES] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
uint8_t sim::_zface_cnt[sim::MAX_ZOMBIES] = {0};
uint16_t sim::_wave_quota = 0;
uint16_t sim::_wave_spawned = 0;
uint32_t sim::_wave_break_until = 0;
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
  _wave_break_until = 0; // run starts hot: wave 1 spawns below with no breather

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

    const bool held = (p == 0) ? input::fire_down() : _p2ctl.fire;
    const bool edge = (p == 0) ? input::fire_pressed() : _p2ctl.fire;
    if (auto_fire(_s.guns[p]) ? held : edge) {
      _do_fire(now, p); // autos gate by gun cooldown, semis by the finger
    }
    if ((p == 0) ? input::reload_pressed() : _p2ctl.reload) {
      start_reload(now, p); // manual top-up while partially spent (auto covers empty)
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
        _burst_left[p] = 0; // ran dry mid-burst: swap at once, don't spin retrying
        start_reload(now, p);
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
          if (_wave_spawned < _wave_quota) {
            // refill inmediato en el slot liberado: la oleada dura la cuota entera
            const bool boss = _wave_boss(_s.wave);
            const uint8_t runners = _wave_runners(_s.wave, _wave_quota);
            _spawn_into(z, _wave_kind(_wave_spawned, boss, runners, _wave_quota));
            ++_wave_spawned;
          }
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
  if (!any && _wave_spawned >= _wave_quota) {
    // cuota agotada y mesa limpia: 5s silent breather, then the next wave.
    // No prompt: the wave event (jingle + boss/roll banners) fires at spawn.
    if (_wave_break_until == 0) {
      _wave_break_until = now + WAVE_BREAK_MS;
    } else if (now >= _wave_break_until) {
      _wave_break_until = 0;
      _spawn_wave();
    }
  }
  return true;
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
