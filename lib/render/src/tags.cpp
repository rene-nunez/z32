#include <Arduino.h>
#include <cstdio>
#include <pgmspace.h>

#include <display.h>
#include <map.h>
#include <sim.h>
#include <sprites.h>

#include "render.h"

// render tags: world-anchored shop price tags plus the centred prompt strip.
// Tags erase only when stale (cut, moved cam, price change, rebuild) but paint
// every frame, so actor/prompt erases self-heal; the prompt paints last, on top.

// Price tags erase only when stale: a camera cut (via repaint), a moved camera, or a
// price-level change (buys). The paint runs every frame instead: actor/prompt erases
// can clobber tag pixels without staling them, and repainting identical text is a
// no-op (text only ever shrinks on a buy, which always stales the erase first).
static int16_t _tags_cx = -1, _tags_cy = -1; // camera the tags were last erased for
static uint8_t _tags_dmg = 0xFF, _tags_spd = 0xFF; // levels the tags were last erased for
static uint8_t _tags_rpd = 0xFF;
static uint16_t _tags_roll = 0xFFFF; // active roulette pad the tags were last erased for
// active wheel pad: row-major scan order, hash(wave/3) % pads (3-wave epochs from
// wave 3: waves 1-2, 3-5, 6-8, ...). Mirrors game::_roulette_active off the synced
// wave, so tags follow the wheel with no extra net bytes.
static uint8_t _roll_active(uint8_t n) {
  if (n == 0) {
    return 0;
  }
  const uint16_t w = sim::view().wave;
  if (w == 0) {
    return 0;
  }
  const uint32_t e = (uint32_t)w / 3u;
  const uint32_t h = (e * 1103515245u + 12345u) & 0x7FFFFFFFu;
  return (uint8_t)((h >> 16u) % n);
}
// shop tag anchors are world-fixed (machines parse once from constexpr _art), so the
// 60x30 scan runs once and every frame reuses the cached centres. 0=H 1=D 2=S 3=C,
// 4..=roulette pads in row-major scan order (same order game::_scan_shops uses,
// so the active index agrees on both sides).
struct _tag_anchor {
  int16_t cx; // block centre, world px
  int16_t wy; // tag top, world px (8px glyph + 2px gap above the block)
  bool found;
};
static constexpr uint8_t TAG_PADS = 6; // roulette pad anchor slots (4 on the shipped map)
static _tag_anchor _tag_anchors[4 + TAG_PADS];
static bool _tag_anchors_done = false;

// stale when the camera moved since the last erase, a price level changed, or terrain
// is still rebuilding under the tags (progressive repaint paints over them, so the
// erase must keep up). Runs in clear(), at the old camera, so the erase lands on last
// frame's pixels. Caches update only on erase: steady frames keep comparing against
// the last erase position.
bool render::_tags_stale() {
  const sim::state& v = sim::view();
  // roulette pads found so far (anchors fill on first paint); the active pad rotates
  // with the wave, so a rotation must erase the old tag even if the camera sat still.
  uint8_t rn = 0;
  for (uint8_t i = 4; i < 4 + TAG_PADS; ++i) {
    if (_tag_anchors_done && _tag_anchors[i].found) {
      ++rn;
    }
  }
  // before the first scan rn reads 0: fall back to comparing the wave itself
  const uint16_t roll = (rn > 0) ? _roll_active(rn) : v.wave;
  // tags price the focus build: each board sees its own next-level cost
  const uint8_t f = (_focus < sim::NUM_PLAYERS) ? _focus : 0;
  if (_paint_y < ARENA_H || _cam_x != _tags_cx || _cam_y != _tags_cy ||
      v.dmg_lvl[f] != _tags_dmg || v.spd_lvl[f] != _tags_spd || v.rpd_lvl[f] != _tags_rpd ||
      roll != _tags_roll) {
    _tags_cx = _cam_x;
    _tags_cy = _cam_y;
    _tags_dmg = v.dmg_lvl[f];
    _tags_spd = v.spd_lvl[f];
    _tags_rpd = v.rpd_lvl[f];
    _tags_roll = roll;
    return true;
  }
  return false;
}

// price tags over the shop machines, anchored to the world so they pan with the
// camera. HEAL/ROLL are fixed; DMG/SPD show the live next-level price (or MAX).
// Drawn every frame after the terrain, erased via the tilemap like sprites. The
// erase always covers the widest tag (8 chars): a buy can shrink the text and a
// tight erase would strand the old pixels for a frame. Only the wave-active
// roulette pad paints its tag; dead pads stay silent (game prompts NO LUCK HERE).
// Anchors are defined above (0=H 1=D 2=S 3=C, 4..=roulette pads).
void render::_shop_labels(bool erase) {
  char dmg_buf[12], spd_buf[12], rpd_buf[12];
  const sim::state& v = sim::view();
  const uint8_t f = (_focus < sim::NUM_PLAYERS) ? _focus : 0;
  if (v.dmg_lvl[f] >= sim::MAX_LVL) {
    snprintf(dmg_buf, sizeof(dmg_buf), "DMG MAX");
  } else {
    snprintf(dmg_buf, sizeof(dmg_buf), "DMG %lu",
             (unsigned long)sim::price_for(sim::PRICE_DMG, v.dmg_lvl[f]));
  }
  if (v.spd_lvl[f] >= sim::MAX_LVL) {
    snprintf(spd_buf, sizeof(spd_buf), "SPD MAX");
  } else {
    snprintf(spd_buf, sizeof(spd_buf), "SPD %lu",
             (unsigned long)sim::price_for(sim::PRICE_SPD, v.spd_lvl[f]));
  }
  if (v.rpd_lvl[f] >= sim::MAX_LVL) {
    snprintf(rpd_buf, sizeof(rpd_buf), "ROF MAX");
  } else {
    snprintf(rpd_buf, sizeof(rpd_buf), "ROF %lu",
             (unsigned long)sim::price_for(sim::PRICE_RPD, v.rpd_lvl[f]));
  }
  const uint8_t want[4] = {tilemap::VENDING, tilemap::V_DMG, tilemap::V_SPD, tilemap::V_RPD};
  const char* text[4] = {"HEAL 150", dmg_buf, spd_buf, rpd_buf};
  const uint16_t col[4] = {colour::green, colour::red, display::rgb565(60, 130, 230),
                           colour::orange};
  if (!_tag_anchors_done) {
    _tag_anchors_done = true;
    for (uint8_t i = 0; i < 4 + TAG_PADS; ++i) {
      _tag_anchors[i].found = false;
    }
    for (uint8_t ti = 0; ti < 4; ++ti) {
      for (uint8_t r = 0; r < tilemap::ROWS && !_tag_anchors[ti].found; ++r) {
        for (uint8_t c = 0; c < tilemap::COLS; ++c) {
          if (tilemap::tiles[r][c] != want[ti]) {
            continue;
          }
          // top-left tile of the 2x2 block only, so the tag paints once per machine
          if (c > 0 && tilemap::tiles[r][c - 1] == want[ti]) {
            continue;
          }
          if (r > 0 && tilemap::tiles[r - 1][c] == want[ti]) {
            continue;
          }
          _tag_anchors[ti].cx = (int16_t)c * tilemap::TILE + tilemap::TILE;
          _tag_anchors[ti].wy = (int16_t)r * tilemap::TILE - 10; // 8px glyph + 2px gap
          _tag_anchors[ti].found = true;
          break;
        }
      }
    }
    // roulette pads in row-major scan order, same order game::_scan_shops uses
    uint8_t rn = 0;
    for (uint8_t r = 0; r < tilemap::ROWS && rn < TAG_PADS; ++r) {
      for (uint8_t c = 0; c < tilemap::COLS && rn < TAG_PADS; ++c) {
        if (tilemap::tiles[r][c] != tilemap::ROULETTE) {
          continue;
        }
        if (c > 0 && tilemap::tiles[r][c - 1] == tilemap::ROULETTE) {
          continue;
        }
        if (r > 0 && tilemap::tiles[r - 1][c] == tilemap::ROULETTE) {
          continue;
        }
        _tag_anchors[4 + rn].cx = (int16_t)c * tilemap::TILE + tilemap::TILE;
        _tag_anchors[4 + rn].wy = (int16_t)r * tilemap::TILE - 10;
        _tag_anchors[4 + rn].found = true;
        ++rn;
      }
    }
  }
  uint8_t roll_n = 0;
  for (uint8_t i = 4; i < 4 + TAG_PADS; ++i) {
    if (_tag_anchors[i].found) {
      ++roll_n;
    }
  }
  const uint8_t act = (uint8_t)(4 + _roll_active(roll_n));
  for (uint8_t ti = 0; ti < 4 + TAG_PADS; ++ti) {
    if (!_tag_anchors[ti].found) {
      continue;
    }
    const bool is_roll = ti >= 4;
    if (erase) {
      // erase every pad (active moved => old tag must clear even off-camera logic aside)
      _erase_world_area(_tag_anchors[ti].cx - _tag_max_w / 2, _tag_anchors[ti].wy,
                        _tag_max_w, 8);
      continue;
    }
    if (is_roll && ti != act) {
      continue; // dead pads stay silent
    }
    const char* t = (ti < 4) ? text[ti] : "ROLL 200";
    const uint16_t cc = (ti < 4) ? col[ti] : colour::yellow;
    uint8_t len = 0;
    while (t[len] != '\0') {
      ++len;
    }
    // the block centre never moves, so erase and draw share it; only the
    // width differs (erase always covers the widest tag, see above)
    const int16_t cx = _tag_anchors[ti].cx;
    const int16_t wy = _tag_anchors[ti].wy;
    const int16_t tw = (int16_t)len * 6; // size-1 glyphs are 6px wide
    const int16_t wx = cx - tw / 2;
    const int16_t sx = wx - _cam_x;
    const int16_t sy = wy - _cam_y + HUD_H;
    if (sx < 0 || sy < HUD_H || sx + tw > (int16_t)display::width() ||
        sy + 8 > ARENA_BOTTOM) {
      continue; // partially off-arena: skip rather than bleed into hud/panel
    }
    display::text(t, sx, sy, cc, 1);
  }
}
