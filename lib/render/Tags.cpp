// World-anchored shop tags and the prompt strip

#include <cstdio>

#include <Display.h>
#include <Map.h>
#include <Sim.h>
#include <Sprites.h>

#include "Render.h"

static int16_t tags_cx = -1, tags_cy = -1;
static uint8_t tags_dmg = 0xFF, tags_spd = 0xFF;
static uint8_t tags_rpd = 0xFF;
static uint16_t tags_roll = 0xFFFF;
static uint8_t roll_active(uint8_t n) {
  if (n == 0) {
    return 0;
  }
  const uint16_t w = Sim::view().wave;
  if (w == 0) {
    return 0;
  }
  const uint32_t e = (uint32_t)w / 3u;
  const uint32_t h = (e * 1103515245u + 12345u) & 0x7FFFFFFFu;
  return (uint8_t)((h >> 16u) % n);
}
struct tag_anchor {
  int16_t cx;
  int16_t wy;
  bool found;
};
static constexpr uint8_t TAG_PADS = 6;
static tag_anchor tag_anchors[4 + TAG_PADS];
static bool tag_anchors_done = false;

bool Render::tags_stale() {
  const Sim::state& v = Sim::view();
  uint8_t rn = 0;
  for (uint8_t i = 4; i < 4 + TAG_PADS; ++i) {
    if (tag_anchors_done && tag_anchors[i].found) {
      ++rn;
    }
  }
  const uint16_t roll = (rn > 0) ? roll_active(rn) : v.wave;
  const uint8_t f = (focus_player < Sim::NUM_PLAYERS) ? focus_player : 0;
  if (paint_y < ARENA_H || camera_x != tags_cx || camera_y != tags_cy ||
      v.dmg_lvl[f] != tags_dmg || v.spd_lvl[f] != tags_spd || v.rpd_lvl[f] != tags_rpd ||
      roll != tags_roll) {
    tags_cx = camera_x;
    tags_cy = camera_y;
    tags_dmg = v.dmg_lvl[f];
    tags_spd = v.spd_lvl[f];
    tags_rpd = v.rpd_lvl[f];
    tags_roll = roll;
    return true;
  }
  return false;
}

void Render::shop_labels(bool erase) {
  char dmg_buf[12], spd_buf[12], rpd_buf[12];
  const Sim::state& v = Sim::view();
  const uint8_t f = (focus_player < Sim::NUM_PLAYERS) ? focus_player : 0;
  if (v.dmg_lvl[f] >= Sim::MAX_LVL) {
    snprintf(dmg_buf, sizeof(dmg_buf), "DMG MAX");
  } else {
    snprintf(dmg_buf, sizeof(dmg_buf), "DMG %lu", (unsigned long)Sim::price_for(Sim::PRICE_DMG, v.dmg_lvl[f]));
  }
  if (v.spd_lvl[f] >= Sim::MAX_LVL) {
    snprintf(spd_buf, sizeof(spd_buf), "SPD MAX");
  } else {
    snprintf(spd_buf, sizeof(spd_buf), "SPD %lu", (unsigned long)Sim::price_for(Sim::PRICE_SPD, v.spd_lvl[f]));
  }
  if (v.rpd_lvl[f] >= Sim::MAX_LVL) {
    snprintf(rpd_buf, sizeof(rpd_buf), "ROF MAX");
  } else {
    snprintf(rpd_buf, sizeof(rpd_buf), "ROF %lu", (unsigned long)Sim::price_for(Sim::PRICE_RPD, v.rpd_lvl[f]));
  }
  const uint8_t want[4] = {Tilemap::VENDING, Tilemap::V_DMG, Tilemap::V_SPD, Tilemap::V_RPD};
  const char* text[4] = {"HEAL 100", dmg_buf, spd_buf, rpd_buf};
  const uint16_t col[4] = {Colour::green, Colour::red, Display::rgb565(60, 130, 230), Colour::orange};
  if (!tag_anchors_done) {
    tag_anchors_done = true;
    for (uint8_t i = 0; i < 4 + TAG_PADS; ++i) {
      tag_anchors[i].found = false;
    }
    for (uint8_t ti = 0; ti < 4; ++ti) {
      for (uint8_t r = 0; r < Tilemap::ROWS && !tag_anchors[ti].found; ++r) {
        for (uint8_t c = 0; c < Tilemap::COLS; ++c) {
          if (Tilemap::tiles[r][c] != want[ti]) {
            continue;
          }
          if (c > 0 && Tilemap::tiles[r][c - 1] == want[ti]) {
            continue;
          }
          if (r > 0 && Tilemap::tiles[r - 1][c] == want[ti]) {
            continue;
          }
          tag_anchors[ti].cx = (int16_t)c * Tilemap::TILE + Tilemap::TILE;
          tag_anchors[ti].wy = (int16_t)r * Tilemap::TILE - 10;
          tag_anchors[ti].found = true;
          break;
        }
      }
    }
    uint8_t rn = 0;
    for (uint8_t r = 0; r < Tilemap::ROWS && rn < TAG_PADS; ++r) {
      for (uint8_t c = 0; c < Tilemap::COLS && rn < TAG_PADS; ++c) {
        if (Tilemap::tiles[r][c] != Tilemap::ROULETTE) {
          continue;
        }
        if (c > 0 && Tilemap::tiles[r][c - 1] == Tilemap::ROULETTE) {
          continue;
        }
        if (r > 0 && Tilemap::tiles[r - 1][c] == Tilemap::ROULETTE) {
          continue;
        }
        tag_anchors[4 + rn].cx = (int16_t)c * Tilemap::TILE + Tilemap::TILE;
        tag_anchors[4 + rn].wy = (int16_t)r * Tilemap::TILE - 10;
        tag_anchors[4 + rn].found = true;
        ++rn;
      }
    }
  }
  uint8_t roll_n = 0;
  for (uint8_t i = 4; i < 4 + TAG_PADS; ++i) {
    if (tag_anchors[i].found) {
      ++roll_n;
    }
  }
  const uint8_t act = (uint8_t)(4 + roll_active(roll_n));
  for (uint8_t ti = 0; ti < 4 + TAG_PADS; ++ti) {
    if (!tag_anchors[ti].found) {
      continue;
    }
    const bool is_roll = ti >= 4;
    if (erase) {
      erase_world_area(tag_anchors[ti].cx - tag_max_w / 2, tag_anchors[ti].wy, tag_max_w, 8);
      continue;
    }
    if (is_roll && ti != act) {
      continue;
    }
    const char* t = (ti < 4) ? text[ti] : "ROLL 100";
    const uint16_t cc = (ti < 4) ? col[ti] : Colour::yellow;
    uint8_t len = 0;
    while (t[len] != '\0') {
      ++len;
    }
    const int16_t cx = tag_anchors[ti].cx;
    const int16_t wy = tag_anchors[ti].wy;
    const int16_t tw = (int16_t)len * 6;
    const int16_t wx = cx - tw / 2;
    const int16_t sx = wx - camera_x;
    const int16_t sy = wy - camera_y + HUD_H;
    if (sx < 0 || sy < HUD_H || sx + tw > (int16_t)Display::width() ||
        sy + 8 > ARENA_BOTTOM) {
      continue;
    }
    Display::text(t, sx, sy, cc, 1);
  }
}
