#include <Arduino.h>
#include <cmath>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <buzz.h>
#include <display.h>
#include <map.h>
#include <panel.h>
#include <render.h>
#include <screens.h>
#include <scores.h>
#include <sim.h>

#include "game.h"
#include "pins.h"

// game shop: machine scan, proximity, INTERACT buys, revive lifts and the prompt
// order. Runs after sim::step, owns no screen: results ride _hint_buf/the strip.

uint8_t game::_roulette_active_at(uint16_t w, uint8_t n) {
  if (n == 0 || w == 0) {
    return 0;
  }
  // 3-wave epochs from wave 3 on: stable for waves 1-2, 3-5, 6-8, ... yet host
  // and client (plus late joiners) always derive the same pad with no extra net bytes
  const uint32_t e = (uint32_t)w / 3u;
  const uint32_t h = (e * 1103515245u + 12345u) & 0x7FFFFFFFu;
  return (uint8_t)((h >> 16u) % n);
}

uint8_t game::_roulette_active() {
  return _roulette_active_at(sim::view().wave, _shop_rn);
}

void game::_push_roll_marker() {
  if (_shop_rn == 0) {
    panel::set_roll(-1, -1);
    return;
  }
  const uint8_t act = _roulette_active();
  if (act >= _shop_rn || _shop_rx[act] < 0 || _shop_ry[act] < 0) {
    panel::set_roll(-1, -1);
    return;
  }
  // centres are exact tile multiples ((c+1)*TILE), so the top-left tile is one back
  panel::set_roll((int16_t)(_shop_rx[act] / tilemap::TILE - 1),
                  (int16_t)(_shop_ry[act] / tilemap::TILE - 1));
}

bool game::_roulette_moved() {
  // the banner fires only on a real relocation: never on wave 1, and never when
  // two epochs hash onto the same pad (the wheel did not move, nothing to say)
  const uint16_t w = sim::view().wave;
  if (w <= 1 || _shop_rn == 0) {
    return false;
  }
  return _roulette_active_at(w, _shop_rn) != _roulette_active_at(w - 1u, _shop_rn);
}

void game::_scan_shops() {
  _shop_hx = _shop_hy = -1;
  _shop_dx = _shop_dy = -1;
  _shop_sx = _shop_sy = -1;
  _shop_cx = _shop_cy = -1;
  for (uint8_t i = 0; i < MAX_PADS; ++i) {
    _shop_rx[i] = _shop_ry[i] = -1;
  }
  _shop_rn = 0;
  for (uint8_t r = 0; r < tilemap::ROWS; ++r) {
    for (uint8_t c = 0; c < tilemap::COLS; ++c) {
      const uint8_t t = tilemap::tiles[r][c];
      // top-left tile of the 2x2 block only, so each machine registers once
      if (c > 0 && tilemap::tiles[r][c - 1] == t) {
        continue;
      }
      if (r > 0 && tilemap::tiles[r - 1][c] == t) {
        continue;
      }
      int16_t* ox = nullptr;
      int16_t* oy = nullptr;
      switch (t) {
        case tilemap::VENDING: ox = &_shop_hx; oy = &_shop_hy; break;
        case tilemap::V_DMG: ox = &_shop_dx; oy = &_shop_dy; break;
        case tilemap::V_SPD: ox = &_shop_sx; oy = &_shop_sy; break;
        case tilemap::V_RPD: ox = &_shop_cx; oy = &_shop_cy; break;
        case tilemap::ROULETTE:
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
        *ox = (int16_t)(((uint16_t)c + 1u) * tilemap::TILE);
        *oy = (int16_t)(((uint16_t)r + 1u) * tilemap::TILE);
        if (t == tilemap::ROULETTE) {
          ++_shop_rn;
        }
      }
    }
  }
}

bool game::_near_inactive_roulette(uint8_t p) {
  const sim::state& v = sim::view();
  if (p >= sim::NUM_PLAYERS || !v.players[p].active || _shop_rn == 0) {
    return false;
  }
  const uint8_t act = _roulette_active();
  const float pcx = v.players[p].x + sim::PLAYER_SIZE / 2.0f;
  const float pcy = v.players[p].y + sim::PLAYER_SIZE / 2.0f;
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

uint8_t game::_shop_at(uint8_t p) {
  const sim::state& v = sim::view();
  if (p >= sim::NUM_PLAYERS || !v.players[p].active) {
    return 0;
  }
  const float pcx = v.players[p].x + sim::PLAYER_SIZE / 2.0f;
  const float pcy = v.players[p].y + sim::PLAYER_SIZE / 2.0f;
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

bool game::_revive_near(uint8_t p) {
  const sim::state& v = sim::view();
  if (p >= sim::NUM_PLAYERS || !v.players[p].active || v.players[p].hp == 0) {
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

bool game::_revive_update(uint32_t now) {
  const sim::state& v = sim::view();
  const bool d0 = v.players[0].active && v.players[0].downed;
  const bool d1 = _net_multi && v.players[1].active && v.players[1].downed;
  // bleed-outs announce once (2s): the strip frees after, the panel keeps the DOWN
  // countdown. Falls stay silent now: the downed shout below + the panel cover it.
  // A lift below overwrites with the risen's THX.
  const bool p0_died = _was_down0 && !d0 && v.players[0].hp == 0;
  const bool p1_died = _was_down1 && !d1 && v.players[1].hp == 0;
  if (p0_died || p1_died) {
    snprintf(_hint_buf, sizeof(_hint_buf), p0_died ? "P1: I'M OUT!" : "P2: I'M OUT!");
    _hint_col = p0_died ? colour::green : _mate_p2col; // the dead one's colour
    _hint_until = now + 2000;
  }
  // bled-out wave rejoins are announced in the wave-banner block below (BACK is
  // the least important of the three wave messages); downed players that held on
  // till the break rise silently (they never bled out). _was_dead advances after
  // the banners so they still see the previous frame.
  if (!_was_down0 && d0) {
    _shouted0 = false; // fall edge: HELP shows again until the first shout
  }
  if (!_was_down1 && d1) {
    _shouted1 = false;
  }
  _was_down0 = d0;
  _was_down1 = d1;
  const bool edge[2] = {input::interact_pressed(), _p2_interact};
  for (uint8_t p = 0; p < sim::NUM_PLAYERS; ++p) {
    if (!edge[p] || !_revive_near(p)) {
      continue;
    }
    const uint8_t q = (p == 0) ? 1 : 0;
    if (sim::revive(q)) {
      snprintf(_hint_buf, sizeof(_hint_buf), p == 0 ? "P2: THX!" : "P1: THX!");
      _hint_col = (p == 0) ? _mate_p2col : colour::green; // the risen one's colour
      _hint_until = now + 1500;
      if (p == 1) {
        _p2_interact = false; // consumed: no accidental buy next frame
      }
      return true;
    }
  }
  return false;
}

void game::_shop_update(uint32_t now) {
  const sim::state& v = sim::view();
  const uint8_t shop = _shop_at(0);
  const bool p2_out = v.players[1].active && v.players[1].hp > 0;
  const uint8_t shop2 = p2_out ? _shop_at(1) : 0;

  if (input::interact_pressed() && shop != 0) {
    bool ok = false;
    switch (shop) {
      case 1:
        ok = sim::buy_heal(now);
        if (ok) {
          snprintf(_hint_buf, sizeof(_hint_buf), "HEALED +2HP");
          _hint_col = colour::green;
        } else {
          snprintf(_hint_buf, sizeof(_hint_buf),
                   v.players[0].hp >= sim::PLAYER_HP_MAX ? "HP FULL" : "NEED %lu PTS",
                   (unsigned long)sim::PRICE_HEAL);
          _hint_col = colour::white; // info, not danger: red never shows in the strip
        }
        break;
      case 2:
        if (v.dmg_lvl[0] >= sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "DMG MAX");
          _hint_col = colour::white;
        } else {
          ok = sim::buy_damage(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "DMG +%u%%!",
                     sim::dmg_bonus(sim::view().dmg_lvl[0]));
            _hint_col = colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS",
                     (unsigned long)sim::price_for(sim::PRICE_DMG, v.dmg_lvl[0]));
            _hint_col = colour::white;
          }
        }
        break;
      case 3:
        if (v.spd_lvl[0] >= sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "SPD MAX");
          _hint_col = colour::white;
        } else {
          ok = sim::buy_speed(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "SPD +%u%%!",
                     sim::spd_bonus(sim::view().spd_lvl[0]));
            _hint_col = colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS",
                     (unsigned long)sim::price_for(sim::PRICE_SPD, v.spd_lvl[0]));
            _hint_col = colour::white;
          }
        }
        break;
      case 5:
        if (v.rpd_lvl[0] >= sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "ROF MAX");
          _hint_col = colour::white;
        } else {
          ok = sim::buy_rapid(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "ROF -%u%%!",
                     sim::rpd_cut(sim::view().rpd_lvl[0]));
            _hint_col = colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS",
                     (unsigned long)sim::price_for(sim::PRICE_RPD, v.rpd_lvl[0]));
            _hint_col = colour::white;
          }
        }
        break;
      default:
        ok = sim::roll_roulette(now);
        if (ok) {
          snprintf(_hint_buf, sizeof(_hint_buf), "NEW GUN: %s", sim::gun_name_p(0));
          _hint_col = colour::green;
        } else {
          snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS", (unsigned long)sim::PRICE_ROLL);
          _hint_col = colour::white;
        }
        break;
    }
    _hint_until = now + 1500;
  }

  // player 2 shops from the shared wallet on its own INTERACT edge (net, F6.3 sets it).
  // silent on the host screen: the buy runs, no P2 prompt/hint is painted here.
  // the client paints its own proximity off the snapshot.
  if (_p2_interact && shop2 != 0) {
    switch (shop2) {
      case 1: sim::buy_heal(now, 1); break;
      case 2: sim::buy_damage(now, 1); break;
      case 3: sim::buy_speed(now, 1); break;
      case 5: sim::buy_rapid(now, 1); break;
      default: sim::roll_roulette(now, 1); break;
    }
  }
  _p2_interact = false; // consumed every frame, edge semantics

  // downed P1 mashing INTERACT with nothing actionable nearby screams SAVE ME
  // (near a machine the buy below still wins, like a standing player)
  if (v.players[0].downed && v.players[1].hp > 0 && input::interact_pressed() &&
      !_revive_near(1) && shop == 0) {
    _send_chat(0);
  }

  if (now < _hint_until && _hint_buf[0] != '\0') {
    render::prompt(_hint_buf, _hint_col); // recent P1 result wins over the prompt
    return;
  }
  if (_revive_near(0)) {
    render::prompt("PRESS INT TO REVIVE"); // INT verb stays yellow
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
      render::prompt("NO LUCK HERE", colour::white);
      return;
    }
  }
  _shop_prompt(shop, "", 0);
}

void game::_shop_prompt(uint8_t shop, const char* who, uint8_t p) {
  const sim::state& v = sim::view();
  switch (shop) {
    case 1:
      snprintf(_hint_buf, sizeof(_hint_buf), "%sGET HEAL +2HP", who);
      render::prompt(_hint_buf);
      break;
    case 2:
      if (v.dmg_lvl[p] >= sim::MAX_LVL) {
        render::prompt("DMG MAX", colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET DMG +%u%%", who,
                 sim::dmg_bonus((uint8_t)(v.dmg_lvl[p] + 1u)));
        render::prompt(_hint_buf);
      }
      break;
    case 3:
      if (v.spd_lvl[p] >= sim::MAX_LVL) {
        render::prompt("SPD MAX", colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET SPD +%u%%", who,
                 sim::spd_bonus((uint8_t)(v.spd_lvl[p] + 1u)));
        render::prompt(_hint_buf);
      }
      break;
    case 4:
      snprintf(_hint_buf, sizeof(_hint_buf), "%sGET ROLL 100", who);
      render::prompt(_hint_buf);
      break;
    case 5:
      if (v.rpd_lvl[p] >= sim::MAX_LVL) {
        render::prompt("ROF MAX", colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET ROF -%u%%", who,
                 sim::rpd_cut((uint8_t)(v.rpd_lvl[p] + 1u)));
        render::prompt(_hint_buf);
      }
      break;
    default: render::prompt(nullptr); break;
  }
}
