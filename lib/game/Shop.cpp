// Game shop: proximity, buys, revive and prompts

#include <cstdio>

#include <Display.h>
#include <Map.h>
#include <Panel.h>
#include <Render.h>
#include <Sim.h>

#include "Game.h"

uint8_t Game::roulette_active_at(uint16_t w, uint8_t n) {
  if (n == 0 || w == 0) {
    return 0;
  }
  const uint32_t e = (uint32_t)w / 3u;
  const uint32_t h = (e * 1103515245u + 12345u) & 0x7FFFFFFFu;
  return (uint8_t)((h >> 16u) % n);
}

uint8_t Game::roulette_active() {
  return roulette_active_at(Sim::view().wave, shop_rn);
}

void Game::push_roll_marker() {
  if (shop_rn == 0) {
    Panel::set_roll(-1, -1);
    return;
  }
  const uint8_t act = roulette_active();
  if (act >= shop_rn || shop_rx[act] < 0 || shop_ry[act] < 0) {
    Panel::set_roll(-1, -1);
    return;
  }
  Panel::set_roll((int16_t)(shop_rx[act] / Tilemap::TILE - 1), (int16_t)(shop_ry[act] / Tilemap::TILE - 1));
}

bool Game::roulette_moved() {
  const uint16_t w = Sim::view().wave;
  if (w <= 1 || shop_rn == 0) {
    return false;
  }
  return roulette_active_at(w, shop_rn) != roulette_active_at(w - 1u, shop_rn);
}

void Game::scan_shops() {
  shop_hx = shop_hy = -1;
  shop_dx = shop_dy = -1;
  shop_sx = shop_sy = -1;
  shop_cx = shop_cy = -1;
  for (uint8_t i = 0; i < MAX_PADS; ++i) {
    shop_rx[i] = shop_ry[i] = -1;
  }
  shop_rn = 0;
  for (uint8_t r = 0; r < Tilemap::ROWS; ++r) {
    for (uint8_t c = 0; c < Tilemap::COLS; ++c) {
      const uint8_t t = Tilemap::tiles[r][c];
      if (c > 0 && Tilemap::tiles[r][c - 1] == t) {
        continue;
      }
      if (r > 0 && Tilemap::tiles[r - 1][c] == t) {
        continue;
      }
      int16_t* ox = nullptr;
      int16_t* oy = nullptr;
      switch (t) {
        case Tilemap::VENDING: ox = &shop_hx; oy = &shop_hy; break;
        case Tilemap::V_DMG: ox = &shop_dx; oy = &shop_dy; break;
        case Tilemap::V_SPD: ox = &shop_sx; oy = &shop_sy; break;
        case Tilemap::V_RPD: ox = &shop_cx; oy = &shop_cy; break;
        case Tilemap::ROULETTE:
          if (shop_rn < MAX_PADS && shop_rx[shop_rn] < 0) {
            ox = &shop_rx[shop_rn];
            oy = &shop_ry[shop_rn];
          }
          break;
        default: break;
      }
      if (ox != nullptr && *ox < 0) {
        *ox = (int16_t)(((uint16_t)c + 1u) * Tilemap::TILE);
        *oy = (int16_t)(((uint16_t)r + 1u) * Tilemap::TILE);
        if (t == Tilemap::ROULETTE) {
          ++shop_rn;
        }
      }
    }
  }
}

bool Game::near_inactive_roulette(uint8_t p) {
  const Sim::state& v = Sim::view();
  if (p >= Sim::NUM_PLAYERS || !v.players[p].active || shop_rn == 0) {
    return false;
  }
  const uint8_t act = roulette_active();
  const float pcx = v.players[p].x + Sim::PLAYER_SIZE / 2.0f;
  const float pcy = v.players[p].y + Sim::PLAYER_SIZE / 2.0f;
  const float r2 = (float)(shop_r * shop_r);
  for (uint8_t i = 0; i < shop_rn; ++i) {
    if (i == act || shop_rx[i] < 0) {
      continue;
    }
    const float dx = pcx - (float)shop_rx[i];
    const float dy = pcy - (float)shop_ry[i];
    if (dx * dx + dy * dy <= r2) {
      return true;
    }
  }
  return false;
}

uint8_t Game::shop_at(uint8_t p) {
  const Sim::state& v = Sim::view();
  if (p >= Sim::NUM_PLAYERS || !v.players[p].active) {
    return 0;
  }
  const float pcx = v.players[p].x + Sim::PLAYER_SIZE / 2.0f;
  const float pcy = v.players[p].y + Sim::PLAYER_SIZE / 2.0f;
  const float r2 = (float)(shop_r * shop_r);
  auto near = [&](int16_t sx, int16_t sy) -> bool {
    if (sx < 0) {
      return false;
    }
    const float dx = pcx - (float)sx;
    const float dy = pcy - (float)sy;
    return dx * dx + dy * dy <= r2;
  };
  if (near(shop_hx, shop_hy)) {
    return 1;
  } else if (near(shop_dx, shop_dy)) {
    return 2;
  } else if (near(shop_sx, shop_sy)) {
    return 3;
  } else if (near(shop_cx, shop_cy)) {
    return 5;
  } else if (shop_rn > 0) {
    const uint8_t act = roulette_active();
    if (act < shop_rn && near(shop_rx[act], shop_ry[act])) {
      return 4;
    }
  }
  return 0;
}

bool Game::revive_near(uint8_t p) {
  const Sim::state& v = Sim::view();
  if (p >= Sim::NUM_PLAYERS || !v.players[p].active || v.players[p].hp == 0) {
    return false;
  }
  const uint8_t q = (p == 0) ? 1 : 0;
  if (!v.players[q].active || !v.players[q].downed) {
    return false;
  }
  const float dx = (v.players[p].x - v.players[q].x);
  const float dy = (v.players[p].y - v.players[q].y);
  return dx * dx + dy * dy <= (float)(revive_r * revive_r);
}

bool Game::revive_update(uint32_t now) {
  const Sim::state& v = Sim::view();
  const bool d0 = v.players[0].active && v.players[0].downed;
  const bool d1 = net_multi && v.players[1].active && v.players[1].downed;
  const bool p0_died = was_down0 && !d0 && v.players[0].hp == 0;
  const bool p1_died = was_down1 && !d1 && v.players[1].hp == 0;
  if (p0_died || p1_died) {
    snprintf(hint_buf, sizeof(hint_buf), p0_died ? "P1: I'M OUT!" : "P2: I'M OUT!");
    hint_col = p0_died ? Colour::green : mate_p2col;
    hint_until = now + 2000;
  }
  if (!was_down0 && d0) {
    shouted0 = false;
  }
  if (!was_down1 && d1) {
    shouted1 = false;
  }
  was_down0 = d0;
  was_down1 = d1;
  const bool edge[2] = {Input::interact_pressed(), p2_interact};
  for (uint8_t p = 0; p < Sim::NUM_PLAYERS; ++p) {
    if (!edge[p] || !revive_near(p)) {
      continue;
    }
    const uint8_t q = (p == 0) ? 1 : 0;
    if (Sim::revive(q)) {
      snprintf(hint_buf, sizeof(hint_buf), p == 0 ? "P2: THX!" : "P1: THX!");
      hint_col = (p == 0) ? mate_p2col : Colour::green;
      hint_until = now + 1500;
      if (p == 1) {
        p2_interact = false;
      }
      return true;
    }
  }
  return false;
}

void Game::shop_update(uint32_t now) {
  const Sim::state& v = Sim::view();
  const uint8_t shop = shop_at(0);
  const bool p2_out = v.players[1].active && v.players[1].hp > 0;
  const uint8_t shop2 = p2_out ? shop_at(1) : 0;

  if (Input::interact_pressed() && shop != 0) {
    bool ok = false;
    switch (shop) {
      case 1:
        ok = Sim::buy_heal(now);
        if (ok) {
          snprintf(hint_buf, sizeof(hint_buf), "HEALED +2HP");
          hint_col = Colour::green;
        } else {
          snprintf(hint_buf, sizeof(hint_buf), v.players[0].hp >= Sim::PLAYER_HP_MAX ? "HP FULL" : "NEED %lu PTS", (unsigned long)Sim::PRICE_HEAL);
          hint_col = Colour::white;
        }
        break;
      case 2:
        if (v.dmg_lvl[0] >= Sim::MAX_LVL) {
          snprintf(hint_buf, sizeof(hint_buf), "DMG MAX");
          hint_col = Colour::white;
        } else {
          ok = Sim::buy_damage(now);
          if (ok) {
            snprintf(hint_buf, sizeof(hint_buf), "DMG +%u%%!", Sim::dmg_bonus(Sim::view().dmg_lvl[0]));
            hint_col = Colour::green;
          } else {
            snprintf(hint_buf, sizeof(hint_buf), "NEED %lu PTS", (unsigned long)Sim::price_for(Sim::PRICE_DMG, v.dmg_lvl[0]));
            hint_col = Colour::white;
          }
        }
        break;
      case 3:
        if (v.spd_lvl[0] >= Sim::MAX_LVL) {
          snprintf(hint_buf, sizeof(hint_buf), "SPD MAX");
          hint_col = Colour::white;
        } else {
          ok = Sim::buy_speed(now);
          if (ok) {
            snprintf(hint_buf, sizeof(hint_buf), "SPD +%u%%!", Sim::spd_bonus(Sim::view().spd_lvl[0]));
            hint_col = Colour::green;
          } else {
            snprintf(hint_buf, sizeof(hint_buf), "NEED %lu PTS", (unsigned long)Sim::price_for(Sim::PRICE_SPD, v.spd_lvl[0]));
            hint_col = Colour::white;
          }
        }
        break;
      case 5:
        if (v.rpd_lvl[0] >= Sim::MAX_LVL) {
          snprintf(hint_buf, sizeof(hint_buf), "ROF MAX");
          hint_col = Colour::white;
        } else {
          ok = Sim::buy_rapid(now);
          if (ok) {
            snprintf(hint_buf, sizeof(hint_buf), "ROF -%u%%!", Sim::rpd_cut(Sim::view().rpd_lvl[0]));
            hint_col = Colour::green;
          } else {
            snprintf(hint_buf, sizeof(hint_buf), "NEED %lu PTS", (unsigned long)Sim::price_for(Sim::PRICE_RPD, v.rpd_lvl[0]));
            hint_col = Colour::white;
          }
        }
        break;
      default:
        ok = Sim::roll_roulette(now);
        if (ok) {
          snprintf(hint_buf, sizeof(hint_buf), "NEW GUN: %s", Sim::gun_name_p(0));
          hint_col = Colour::green;
        } else {
          snprintf(hint_buf, sizeof(hint_buf), "NEED %lu PTS", (unsigned long)Sim::PRICE_ROLL);
          hint_col = Colour::white;
        }
        break;
    }
    hint_until = now + 1500;
  }

  if (p2_interact && shop2 != 0) {
    switch (shop2) {
      case 1: Sim::buy_heal(now, 1); break;
      case 2: Sim::buy_damage(now, 1); break;
      case 3: Sim::buy_speed(now, 1); break;
      case 5: Sim::buy_rapid(now, 1); break;
      default: Sim::roll_roulette(now, 1); break;
    }
  }
  p2_interact = false;

  if (v.players[0].downed && v.players[1].hp > 0 && Input::interact_pressed() &&
      !revive_near(1) && shop == 0) {
    send_chat(0);
  }

  if (now < hint_until && hint_buf[0] != '\0') {
    Render::prompt(hint_buf, hint_col);
    return;
  }
  if (revive_near(0)) {
    Render::prompt("PRESS INT TO REVIVE");
    return;
  }
  if (urgent_callout(0)) {
    return;
  }
  if (help_hint(0)) {
    return;
  }
  if (reload_prompt(0)) {
    return;
  }
  if (shop == 0) {
    if (near_inactive_roulette(0)) {
      Render::prompt("NO LUCK HERE", Colour::white);
      return;
    }
  }
  shop_prompt(shop, "", 0);
}

void Game::shop_prompt(uint8_t shop, const char* who, uint8_t p) {
  const Sim::state& v = Sim::view();
  switch (shop) {
    case 1:
      snprintf(hint_buf, sizeof(hint_buf), "%sGET HEAL +2HP", who);
      Render::prompt(hint_buf);
      break;
    case 2:
      if (v.dmg_lvl[p] >= Sim::MAX_LVL) {
        Render::prompt("DMG MAX", Colour::white);
      } else {
        snprintf(hint_buf, sizeof(hint_buf), "%sGET DMG +%u%%", who, Sim::dmg_bonus((uint8_t)(v.dmg_lvl[p] + 1u)));
        Render::prompt(hint_buf);
      }
      break;
    case 3:
      if (v.spd_lvl[p] >= Sim::MAX_LVL) {
        Render::prompt("SPD MAX", Colour::white);
      } else {
        snprintf(hint_buf, sizeof(hint_buf), "%sGET SPD +%u%%", who, Sim::spd_bonus((uint8_t)(v.spd_lvl[p] + 1u)));
        Render::prompt(hint_buf);
      }
      break;
    case 4:
      snprintf(hint_buf, sizeof(hint_buf), "%sGET ROLL 100", who);
      Render::prompt(hint_buf);
      break;
    case 5:
      if (v.rpd_lvl[p] >= Sim::MAX_LVL) {
        Render::prompt("ROF MAX", Colour::white);
      } else {
        snprintf(hint_buf, sizeof(hint_buf), "%sGET ROF -%u%%", who, Sim::rpd_cut((uint8_t)(v.rpd_lvl[p] + 1u)));
        Render::prompt(hint_buf);
      }
      break;
    default: Render::prompt(nullptr); break;
  }
}
