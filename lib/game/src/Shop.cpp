#include <Arduino.h>
#include <cmath>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <Buzz.h>
#include <Display.h>
#include <Map.h>
#include <Panel.h>
#include <Render.h>
#include <Screens.h>
#include <Scores.h>
#include <Sim.h>

#include "Game.h"
#include "pins.h"

// game shop: machine scan, proximity, INTERACT buys, revive lifts and the prompt
// order. Runs after Sim::step, owns no screen: results ride _hint_buf/the strip

uint8_t Game::_roulette_active_at(uint16_t w, uint8_t n) {
  if (n == 0 || w == 0) {
    return 0;
  }
  // 3-wave epochs from wave 3 on: stable for waves 1-2, 3-5, 6-8, ... yet host
  // and client (plus late joiners) always derive the same pad with no extra net bytes
  const uint32_t e = (uint32_t)w / 3u;
  const uint32_t h = (e * 1103515245u + 12345u) & 0x7FFFFFFFu;
  return (uint8_t)((h >> 16u) % n);
}

uint8_t Game::_roulette_active() {
  return _roulette_active_at(Sim::view().wave, _shop_rn);
}

void Game::_push_roll_marker() {
  if (_shop_rn == 0) {
    Panel::set_roll(-1, -1);
    return;
  }
  const uint8_t act = _roulette_active();
  if (act >= _shop_rn || _shop_rx[act] < 0 || _shop_ry[act] < 0) {
    Panel::set_roll(-1, -1);
    return;
  }
  // centres are exact tile multiples ((c+1)*TILE), so the top-left tile is one back
  Panel::set_roll((int16_t)(_shop_rx[act] / Tilemap::TILE - 1), (int16_t)(_shop_ry[act] / Tilemap::TILE - 1));
}

bool Game::_roulette_moved() {
  // the banner fires only on a real relocation: never on wave 1, and never when
  // two epochs hash onto the same pad (the wheel did not move, nothing to say)
  const uint16_t w = Sim::view().wave;
  if (w <= 1 || _shop_rn == 0) {
    return false;
  }
  return _roulette_active_at(w, _shop_rn) != _roulette_active_at(w - 1u, _shop_rn);
}

void Game::_scan_shops() {
  _shop_hx = _shop_hy = -1;
  _shop_dx = _shop_dy = -1;
  _shop_sx = _shop_sy = -1;
  _shop_cx = _shop_cy = -1;
  for (uint8_t i = 0; i < MAX_PADS; ++i) {
    _shop_rx[i] = _shop_ry[i] = -1;
  }
  _shop_rn = 0;
  for (uint8_t r = 0; r < Tilemap::ROWS; ++r) {
    for (uint8_t c = 0; c < Tilemap::COLS; ++c) {
      const uint8_t t = Tilemap::tiles[r][c];
      // top-left tile of the 2x2 block only, so each machine registers once
      if (c > 0 && Tilemap::tiles[r][c - 1] == t) {
        continue;
      }
      if (r > 0 && Tilemap::tiles[r - 1][c] == t) {
        continue;
      }
      int16_t* ox = nullptr;
      int16_t* oy = nullptr;
      switch (t) {
        case Tilemap::VENDING: ox = &_shop_hx; oy = &_shop_hy; break;
        case Tilemap::V_DMG: ox = &_shop_dx; oy = &_shop_dy; break;
        case Tilemap::V_SPD: ox = &_shop_sx; oy = &_shop_sy; break;
        case Tilemap::V_RPD: ox = &_shop_cx; oy = &_shop_cy; break;
        case Tilemap::ROULETTE:
          // row-major scan order defines the pad index, shared with render tags:
          // 0 = north, 1 = east, 2 = centre, 3 = SE on the shipped map
          if (_shop_rn < MAX_PADS && _shop_rx[_shop_rn] < 0) {
            ox = &_shop_rx[_shop_rn];
            oy = &_shop_ry[_shop_rn];
          }
          break;
        default: break;
      }
      // first (top-left) tile of the 2x2 block wins, so the centre is one tile in
      if (ox != nullptr && *ox < 0) {
        *ox = (int16_t)(((uint16_t)c + 1u) * Tilemap::TILE);
        *oy = (int16_t)(((uint16_t)r + 1u) * Tilemap::TILE);
        if (t == Tilemap::ROULETTE) {
          ++_shop_rn;
        }
      }
    }
  }
}

bool Game::_near_inactive_roulette(uint8_t p) {
  const Sim::state& v = Sim::view();
  if (p >= Sim::NUM_PLAYERS || !v.players[p].active || _shop_rn == 0) {
    return false;
  }
  const uint8_t act = _roulette_active();
  const float pcx = v.players[p].x + Sim::PLAYER_SIZE / 2.0f;
  const float pcy = v.players[p].y + Sim::PLAYER_SIZE / 2.0f;
  const float r2 = (float)(_shop_r * _shop_r);
  for (uint8_t i = 0; i < _shop_rn; ++i) {
    if (i == act || _shop_rx[i] < 0) {
      continue;
    }
    const float dx = pcx - (float)_shop_rx[i];
    const float dy = pcy - (float)_shop_ry[i];
    if (dx * dx + dy * dy <= r2) {
      return true;
    }
  }
  return false;
}

uint8_t Game::_shop_at(uint8_t p) {
  const Sim::state& v = Sim::view();
  if (p >= Sim::NUM_PLAYERS || !v.players[p].active) {
    return 0;
  }
  const float pcx = v.players[p].x + Sim::PLAYER_SIZE / 2.0f;
  const float pcy = v.players[p].y + Sim::PLAYER_SIZE / 2.0f;
  const float r2 = (float)(_shop_r * _shop_r);
  auto near = [&](int16_t sx, int16_t sy) -> bool {
    if (sx < 0) {
      return false;
    }
    const float dx = pcx - (float)sx;
    const float dy = pcy - (float)sy;
    return dx * dx + dy * dy <= r2;
  };
  if (near(_shop_hx, _shop_hy)) {
    return 1;
  } else if (near(_shop_dx, _shop_dy)) {
    return 2;
  } else if (near(_shop_sx, _shop_sy)) {
    return 3;
  } else if (near(_shop_cx, _shop_cy)) {
    return 5;
  } else if (_shop_rn > 0) {
    const uint8_t act = _roulette_active();
    if (act < _shop_rn && near(_shop_rx[act], _shop_ry[act])) {
      return 4;
    }
  }
  return 0;
}

bool Game::_revive_near(uint8_t p) {
  const Sim::state& v = Sim::view();
  if (p >= Sim::NUM_PLAYERS || !v.players[p].active || v.players[p].hp == 0) {
    return false; // only the standing can lift
  }
  const uint8_t q = (p == 0) ? 1 : 0;
  if (!v.players[q].active || !v.players[q].downed) {
    return false;
  }
  const float dx = (v.players[p].x - v.players[q].x);
  const float dy = (v.players[p].y - v.players[q].y);
  return dx * dx + dy * dy <= (float)(_revive_r * _revive_r);
}

bool Game::_revive_update(uint32_t now) {
  const Sim::state& v = Sim::view();
  const bool d0 = v.players[0].active && v.players[0].downed;
  const bool d1 = _net_multi && v.players[1].active && v.players[1].downed;
  // bleed-outs announce once (2s): the strip frees after, the panel keeps the DOWN
  // countdown. Falls stay silent now: the downed shout below + the panel cover it
  // A lift below overwrites with the risen's THX
  const bool p0_died = _was_down0 && !d0 && v.players[0].hp == 0;
  const bool p1_died = _was_down1 && !d1 && v.players[1].hp == 0;
  if (p0_died || p1_died) {
    snprintf(_hint_buf, sizeof(_hint_buf), p0_died ? "P1: I'M OUT!" : "P2: I'M OUT!");
    _hint_col = p0_died ? Colour::green : _mate_p2col; // the dead one's Colour
    _hint_until = now + 2000;
  }
  // bled-out wave rejoins are announced in the wave-banner block below (BACK is
  // the least important of the three wave messages); downed players that held on
  // till the break rise silently (they never bled out). _was_dead advances after
  // the banners so they still see the previous frame
  if (!_was_down0 && d0) {
    _shouted0 = false; // fall edge: HELP shows again until the first shout
  }
  if (!_was_down1 && d1) {
    _shouted1 = false;
  }
  _was_down0 = d0;
  _was_down1 = d1;
  const bool edge[2] = {Input::interact_pressed(), _p2_interact};
  for (uint8_t p = 0; p < Sim::NUM_PLAYERS; ++p) {
    if (!edge[p] || !_revive_near(p)) {
      continue;
    }
    const uint8_t q = (p == 0) ? 1 : 0;
    if (Sim::revive(q)) {
      snprintf(_hint_buf, sizeof(_hint_buf), p == 0 ? "P2: THX!" : "P1: THX!");
      _hint_col = (p == 0) ? _mate_p2col : Colour::green; // the risen one's Colour
      _hint_until = now + 1500;
      if (p == 1) {
        _p2_interact = false; // consumed: no accidental buy next frame
      }
      return true;
    }
  }
  return false;
}

void Game::_shop_update(uint32_t now) {
  const Sim::state& v = Sim::view();
  const uint8_t shop = _shop_at(0);
  const bool p2_out = v.players[1].active && v.players[1].hp > 0;
  const uint8_t shop2 = p2_out ? _shop_at(1) : 0;

  if (Input::interact_pressed() && shop != 0) {
    bool ok = false;
    switch (shop) {
      case 1:
        ok = Sim::buy_heal(now);
        if (ok) {
          snprintf(_hint_buf, sizeof(_hint_buf), "HEALED +2HP");
          _hint_col = Colour::green;
        } else {
          snprintf(_hint_buf, sizeof(_hint_buf), v.players[0].hp >= Sim::PLAYER_HP_MAX ? "HP FULL" : "NEED %lu PTS", (unsigned long)Sim::PRICE_HEAL);
          _hint_col = Colour::white; // info, not danger: red never shows in the strip
        }
        break;
      case 2:
        if (v.dmg_lvl[0] >= Sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "DMG MAX");
          _hint_col = Colour::white;
        } else {
          ok = Sim::buy_damage(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "DMG +%u%%!", Sim::dmg_bonus(Sim::view().dmg_lvl[0]));
            _hint_col = Colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS", (unsigned long)Sim::price_for(Sim::PRICE_DMG, v.dmg_lvl[0]));
            _hint_col = Colour::white;
          }
        }
        break;
      case 3:
        if (v.spd_lvl[0] >= Sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "SPD MAX");
          _hint_col = Colour::white;
        } else {
          ok = Sim::buy_speed(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "SPD +%u%%!", Sim::spd_bonus(Sim::view().spd_lvl[0]));
            _hint_col = Colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS", (unsigned long)Sim::price_for(Sim::PRICE_SPD, v.spd_lvl[0]));
            _hint_col = Colour::white;
          }
        }
        break;
      case 5:
        if (v.rpd_lvl[0] >= Sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "ROF MAX");
          _hint_col = Colour::white;
        } else {
          ok = Sim::buy_rapid(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "ROF -%u%%!", Sim::rpd_cut(Sim::view().rpd_lvl[0]));
            _hint_col = Colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS", (unsigned long)Sim::price_for(Sim::PRICE_RPD, v.rpd_lvl[0]));
            _hint_col = Colour::white;
          }
        }
        break;
      default:
        ok = Sim::roll_roulette(now);
        if (ok) {
          snprintf(_hint_buf, sizeof(_hint_buf), "NEW GUN: %s", Sim::gun_name_p(0));
          _hint_col = Colour::green;
        } else {
          snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS", (unsigned long)Sim::PRICE_ROLL);
          _hint_col = Colour::white;
        }
        break;
    }
    _hint_until = now + 1500;
  }

  // player 2 shops from the shared wallet on its own INTERACT edge (net, F6.3 sets it)
  // silent on the host screen: the buy runs, no P2 prompt/hint is painted here
  // the client paints its own proximity off the snapshot
  if (_p2_interact && shop2 != 0) {
    switch (shop2) {
      case 1: Sim::buy_heal(now, 1); break;
      case 2: Sim::buy_damage(now, 1); break;
      case 3: Sim::buy_speed(now, 1); break;
      case 5: Sim::buy_rapid(now, 1); break;
      default: Sim::roll_roulette(now, 1); break;
    }
  }
  _p2_interact = false; // consumed every frame, edge semantics

  // downed P1 mashing INTERACT with nothing actionable nearby screams SAVE ME
  // (near a machine the buy below still wins, like a standing player)
  if (v.players[0].downed && v.players[1].hp > 0 && Input::interact_pressed() &&
      !_revive_near(1) && shop == 0) {
    _send_chat(0);
  }

  if (now < _hint_until && _hint_buf[0] != '\0') {
    Render::prompt(_hint_buf, _hint_col); // recent P1 result wins over the prompt
    return;
  }
  if (_revive_near(0)) {
    Render::prompt("PRESS INT TO REVIVE"); // INT verb stays yellow
    return;
  }
  if (_urgent_callout(0)) {
    return; // voluntary SAVE ME over the reload nag
  }
  if (_help_hint(0)) {
    return; // PRESS INT FOR HELP until the first shout
  }
  if (_reload_prompt(0)) {
    return; // own mag swap under the shout
  }
  if (shop == 0) {
    // standing on a dead wheel reads as moved, not as silence (P1 view only)
    if (_near_inactive_roulette(0)) {
      Render::prompt("NO LUCK HERE", Colour::white);
      return;
    }
  }
  _shop_prompt(shop, "", 0);
}

void Game::_shop_prompt(uint8_t shop, const char* who, uint8_t p) {
  const Sim::state& v = Sim::view();
  switch (shop) {
    case 1:
      snprintf(_hint_buf, sizeof(_hint_buf), "%sGET HEAL +2HP", who);
      Render::prompt(_hint_buf);
      break;
    case 2:
      if (v.dmg_lvl[p] >= Sim::MAX_LVL) {
        Render::prompt("DMG MAX", Colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET DMG +%u%%", who, Sim::dmg_bonus((uint8_t)(v.dmg_lvl[p] + 1u)));
        Render::prompt(_hint_buf);
      }
      break;
    case 3:
      if (v.spd_lvl[p] >= Sim::MAX_LVL) {
        Render::prompt("SPD MAX", Colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET SPD +%u%%", who, Sim::spd_bonus((uint8_t)(v.spd_lvl[p] + 1u)));
        Render::prompt(_hint_buf);
      }
      break;
    case 4:
      snprintf(_hint_buf, sizeof(_hint_buf), "%sGET ROLL 100", who);
      Render::prompt(_hint_buf);
      break;
    case 5:
      if (v.rpd_lvl[p] >= Sim::MAX_LVL) {
        Render::prompt("ROF MAX", Colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET ROF -%u%%", who, Sim::rpd_cut((uint8_t)(v.rpd_lvl[p] + 1u)));
        Render::prompt(_hint_buf);
      }
      break;
    default: Render::prompt(nullptr); break;
  }
}
