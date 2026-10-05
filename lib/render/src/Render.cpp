#include <Arduino.h>
#include <cstdio>
#include <pgmspace.h>

#include <Display.h>
#include <map.h>
#include <Sim.h>
#include <sprites.h>

#include "Render.h"

// Opaque bbox per art array, scanned once from PROGMEM: erase only repaints what draw
// could have painted (transparent margins are never touched). The 32px boss is mostly
// air, so this cuts its erase to the body. Cached by pointer (19 arts, one scan each).
struct _bbox {
  const uint16_t* img;
  uint8_t art;
  uint8_t x0, y0, w, h;
};
static _bbox _bbox_cache[24];
static uint8_t _bbox_n = 0;

// bbox of img (mirrored on x when flip, matching the composite blit below). Full rect
// cache overflows (never: 19 arts). Empty art (never) yields w=h=0: caller skips.
static void _art_bbox(const uint16_t* img, uint8_t art, bool flip, uint8_t& x0, uint8_t& y0,
                      uint8_t& w, uint8_t& h) {
  const _bbox* b = nullptr;
  for (uint8_t i = 0; i < _bbox_n; ++i) {
    if (_bbox_cache[i].img == img && _bbox_cache[i].art == art) {
      b = &_bbox_cache[i];
      break;
    }
  }
  if (b == nullptr) {
    uint8_t ox0 = art, oy0 = art, ox1 = 0, oy1 = 0;
    bool any = false;
    for (uint8_t r = 0; r < art; ++r) {
      for (uint8_t c = 0; c < art; ++c) {
        if (pgm_read_word(img + (uint16_t)r * art + c) != 0x0000) {
          if (!any || c < ox0) {
            ox0 = c;
          }
          if (!any || c > ox1) {
            ox1 = c;
          }
          if (!any || r < oy0) {
            oy0 = r;
          }
          if (!any || r > oy1) {
            oy1 = r;
          }
          any = true;
        }
      }
    }
    uint8_t bw = 0, bh = 0;
    if (any) {
      bw = (uint8_t)(ox1 - ox0 + 1);
      bh = (uint8_t)(oy1 - oy0 + 1);
    } else {
      ox0 = oy0 = 0;
    }
    if (_bbox_n < 24) {
      _bbox_cache[_bbox_n] = {img, art, ox0, oy0, bw, bh};
      b = &_bbox_cache[_bbox_n++];
    } else {
      x0 = y0 = 0; // overflow fallback (never): full rect, always correct
      w = h = art;
      return;
    }
  }
  x0 = b->x0;
  y0 = b->y0;
  w = b->w;
  h = b->h;
  if (flip && w > 0) {
    x0 = (uint8_t)(art - x0 - w); // mirror of [x0,x0+w) under hflip
  }
}

// One-frame blit buffer (32x32 max actor): terrain + sprite composite in RAM, then a
// single SPI burst. Replaces hundreds of tiny fillRect runs (each a setup + a scan race
// = partial "cut" sprite) with one atomic push. Transparency resolves against the
// tilemap, exactly like the old run path (0x0000 = see-through).
static uint16_t _blit_buf[32 * 32];

// terrain-only push of a world rect (actor erase). Same pixels as _erase_world_area,
// one burst instead of per-run fills.
static void _erase_box(int16_t wx, int16_t wy, int16_t w, int16_t h) {
  if (w <= 0 || h <= 0) {
    return;
  }
  const int16_t sw = (int16_t)display::width();
  int16_t sy0 = wy - render::cam_y() + render::HUD_H;
  int16_t x0 = wx, y0 = wy, bw = w, bh = h;
  if (sy0 < render::HUD_H) { // clip to the arena, like _fill_world_run
    const int16_t cut = render::HUD_H - sy0;
    y0 += cut;
    bh -= cut;
    sy0 = render::HUD_H;
  }
  if (sy0 + bh > render::ARENA_BOTTOM) {
    bh = render::ARENA_BOTTOM - sy0;
  }
  if (bh <= 0) {
    return;
  }
  int16_t sx0 = x0 - render::cam_x();
  if (sx0 < 0) {
    x0 -= sx0;
    bw += sx0;
    sx0 = 0;
  }
  if (sx0 + bw > sw) {
    bw = sw - sx0;
  }
  if (bw <= 0) {
    return;
  }
  for (int16_t r = 0; r < bh; ++r) {
    for (int16_t c = 0; c < bw; ++c) {
      _blit_buf[(uint16_t)r * bw + c] = tilemap::color_at(x0 + c, y0 + r);
    }
  }
  display::push_image(sx0, sy0, bw, bh, _blit_buf);
}

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

int16_t render::_cam_x = 0;
int16_t render::_cam_y = 0;
uint8_t render::_focus = 0;
int16_t render::_paint_y = render::ARENA_H;
const char* render::_prompt = nullptr;
uint16_t render::_prompt_col = colour::yellow;
// conditional strip erase: text the strip was last erased for ("" = clean terrain)
// plus the camera of that erase. Steady text + steady camera skips the 320x10
// terrain repaint every frame (that cost used to stretch frames and tear sprites).
static char _prompt_erased[28] = "";
static int16_t _prompt_ex = -1, _prompt_ey = -1;

void render::prompt(const char* msg, uint16_t color) {
  _prompt = msg;
  _prompt_col = color;
}

int16_t render::cam_x() {
  return _cam_x;
}

int16_t render::cam_y() {
  return _cam_y;
}

void render::repaint() {
  // NOTE: HUD strip (0..HUD_H) belongs to game::_draw_hud (cached): a camera cut must
  // not wipe it, or the cache would skip and leave it black. Menu->playing transitions
  // force a full HUD repaint via game (which wipes + repaints all fields itself).
  _paint_y = 0; // the arena repaint runs from here, PAINT_CHUNK rows per frame
}

void render::repaint_step() {
  if (_paint_y >= ARENA_H) {
    return; // nothing pending
  }

  const int16_t sw = (int16_t)display::width();
  const int16_t end = (_paint_y + PAINT_CHUNK < ARENA_H) ? _paint_y + PAINT_CHUNK : ARENA_H;

  for (int16_t ry = _paint_y; ry < end; ++ry) { // merge same-colour runs per row
    const int16_t wy = _cam_y + ry;
    const int16_t sy = HUD_H + ry;
    int16_t run_x = 0;
    uint16_t run_col = tilemap::color_at(_cam_x, wy);

    for (int16_t rx = 1; rx < sw; ++rx) {
      const uint16_t col = tilemap::color_at(_cam_x + rx, wy);
      if (col != run_col) {
        display::fill_rect(run_x, sy, rx - run_x, 1, run_col);
        run_x = rx;
        run_col = col;
      }
    }
    display::fill_rect(run_x, sy, sw - run_x, 1, run_col);
  }

  _paint_y = end;
}

int16_t render::_cell_cam(int16_t p, int16_t step, int16_t max_cam) {
  int16_t cam = (int16_t)((p / step) * step);
  if (cam < 0) {
    cam = 0;
  } else if (cam > max_cam) {
    cam = max_cam;
  }
  return cam;
}

void render::set_focus(uint8_t p) {
  _focus = (p < sim::NUM_PLAYERS) ? p : 0;
  _cam_x = -1; // force the camera to snap on the next update
  _cam_y = -1;
}

uint8_t render::focus() {
  return _focus;
}

void render::update_camera() {
  const int16_t aw = (int16_t)display::width();
  const sim::state& v = sim::view();
  // each board frames its own player, so co-op splits across districts freely. A downed
  // focus still frames its body (spectate the rescue); only an inactive focus falls back.
  // A bled-out body (active, hp 0, not downed) spectates the living partner until the
  // next wave respawns it: the rule is per frame, so the camera returns on its own.
  uint8_t f = _focus;
  const uint8_t o = (f == 0) ? 1 : 0;
  const bool f_dead = v.players[f].active && !v.players[f].downed && v.players[f].hp == 0;
  const bool o_out = v.players[o].active && !v.players[o].downed && v.players[o].hp > 0;
  if (f_dead && o_out) {
    f = o;
  }
  if (!v.players[f].active) {
    f = (f == 0) ? 1 : 0;
  }
  if (!v.players[f].active) {
    f = 0;
  }
  const int16_t pcx = (int16_t)(v.players[f].x + sim::PLAYER_SIZE / 2);
  const int16_t pcy = (int16_t)(v.players[f].y + sim::PLAYER_SIZE / 2);

  const int16_t cx = _cell_cam(pcx, aw, (int16_t)(tilemap::WORLD_W - aw));
  const int16_t cy = _cell_cam(pcy, ARENA_H, (int16_t)(tilemap::WORLD_H - ARENA_H));

  if (cx == _cam_x && cy == _cam_y) {
    return;
  }
  _cam_x = cx;
  _cam_y = cy;
  repaint(); // the new screen must be repainted from the tilemap
}

void render::_fill_world_run(int16_t wx, int16_t sy, int16_t w, uint16_t col) {
  if (w <= 0 || sy < HUD_H || sy >= ARENA_BOTTOM) {
    return;
  }
  const int16_t sw = (int16_t)display::width();
  int16_t x0 = wx - _cam_x;
  int16_t x1 = x0 + w;
  if (x1 <= 0 || x0 >= sw) {
    return; // fully off-viewport
  }
  if (x0 < 0) {
    x0 = 0;
  }
  if (x1 > sw) {
    x1 = sw;
  }
  display::fill_rect(x0, sy, x1 - x0, 1, col);
}

void render::_erase_world_rect(int16_t wx, int16_t wy, uint8_t size) {
  _erase_world_area(wx, wy, size, size);
}

void render::_erase_world_area(int16_t wx, int16_t wy, int16_t w, int16_t h) {
  for (int16_t dy = 0; dy < h; ++dy) {
    const int16_t wyy = wy + dy;
    const int16_t sy = wyy - _cam_y + HUD_H;
    if (sy < HUD_H || sy >= ARENA_BOTTOM) {
      continue;
    }

    int16_t run_x = wx;
    uint16_t run_col = tilemap::color_at(wx, wyy);
    for (int16_t dx = 1; dx < w; ++dx) {
      const uint16_t col = tilemap::color_at(wx + dx, wyy);
      if (col != run_col) {
        _fill_world_run(run_x, sy, (int16_t)(wx + dx - run_x), run_col);
        run_x = wx + dx;
        run_col = col;
      }
    }
    _fill_world_run(run_x, sy, (int16_t)(wx + w - run_x), run_col);
  }
}

void render::_fill_world_box(int16_t wx, int16_t wy, uint8_t size, uint16_t col) {
  const int x0 = wx - _cam_x;
  const int y0 = wy - _cam_y + HUD_H;
  // clip to the arena so a half-sprite never bleeds into the hud strip nor the panel
  const int cx0 = x0 > 0 ? x0 : 0;
  const int cy0 = y0 > HUD_H ? y0 : HUD_H;
  const int cx1 = x0 + size < (int)display::width() ? x0 + size : (int)display::width();
  const int cy1 = y0 + size < (int)ARENA_BOTTOM ? y0 + size : (int)ARENA_BOTTOM;

  if (cx0 < cx1 && cy0 < cy1) {
    display::fill_rect(cx0, cy0, cx1 - cx0, cy1 - cy0, col);
  }
}

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

void render::clear() {
  const sim::state& v = sim::view();
  for (uint8_t p = 0; p < sim::NUM_PLAYERS; ++p) {
    if (v.players[p].active) {
      const int16_t tlx = _sprite_tl((int16_t)v.players[p].x, sim::PLAYER_SIZE, SPRITE);
      const int16_t tly = _sprite_tl((int16_t)v.players[p].y, sim::PLAYER_SIZE, SPRITE);
      if (v.players[p].downed) {
        // full rect: the yellow rescue frame sits at the sprite edges, outside the bbox
        _erase_box(tlx, tly, SPRITE, SPRITE);
        continue;
      }
      // erase the opaque bbox, not the full art: draw() never paints the margins
      bool flip = false;
      const uint16_t* img = _player_img(p, v.players[p].facing & 7, flip);
      uint8_t bx0, by0, bw, bh;
      _art_bbox(img, SPRITE, flip, bx0, by0, bw, bh);
      if (bw > 0 && bh > 0) {
        _erase_box(tlx + bx0, tly + by0, bw, bh);
      }
    }
  }
  for (uint8_t i = 0; i < sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active) {
      const uint8_t art =
          (v.zombies[i].kind == sim::actor_kind::boss) ? BOSS_ART : SPRITE;
      const int16_t tlx = _sprite_tl((int16_t)v.zombies[i].x, sim::ZOMBIE_SIZE, art);
      const int16_t tly = _sprite_tl((int16_t)v.zombies[i].y, sim::ZOMBIE_SIZE, art);
      bool flip = false;
      const uint16_t* img = _zombie_img(v.zombies[i].kind, v.zombies[i].facing & 7, flip);
      uint8_t bx0, by0, bw, bh;
      _art_bbox(img, art, flip, bx0, by0, bw, bh);
      if (bw > 0 && bh > 0) {
        _erase_box(tlx + bx0, tly + by0, bw, bh);
      }
    }
  }
  for (uint8_t i = 0; i < sim::MAX_BULLETS; ++i) {
    if (v.bullets[i].active) {
      _erase_world_rect((int16_t)v.bullets[i].x, (int16_t)v.bullets[i].y, sim::BULLET_SIZE);
    }
  }
  if (_tags_stale()) {
    _shop_labels(true); // erase last frame's price tags, but only when stale
  }
  // erase the prompt strip at the old camera: it is screen-fixed, so its world
  // rect moves with the camera and a camera cut would strand it otherwise.
  // conditional: _prompt still holds last frame's text here (game sets the new one
  // after the step). Steady text + steady camera = nothing to clean, so the
  // 320x10 terrain repaint (3200 color_at/frame) runs only on text change,
  // camera move or progressive repaint. draw() repaints the text every frame
  // with an opaque bg, so it self-covers; the erase is always full-width, so
  // shrinking text never strands pixels.
  const char* cur_prompt = (_prompt != nullptr) ? _prompt : "";
  if ((cur_prompt[0] != '\0' || _prompt_erased[0] != '\0') &&
      (strcmp(cur_prompt, _prompt_erased) != 0 || _cam_x != _prompt_ex ||
       _cam_y != _prompt_ey || _paint_y < ARENA_H)) {
    _erase_world_area(_cam_x, _cam_y + ARENA_H - _prompt_h, (int16_t)display::width(),
                      _prompt_h);
    snprintf(_prompt_erased, sizeof(_prompt_erased), "%s", cur_prompt);
    _prompt_ex = _cam_x;
    _prompt_ey = _cam_y;
  }
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
    const char* t = (ti < 4) ? text[ti] : "ROLL 100";
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

// art centred on the hitbox: draw and erase share this so no pixel ghosts.
int16_t render::_sprite_tl(int16_t e, uint8_t hitbox, uint8_t art) {
  return e + ((int16_t)hitbox - (int16_t)art) / 2;
}

void render::_draw_actor(int16_t ex, int16_t ey, uint8_t hitbox, uint8_t art,
                         const uint16_t* img, bool flip) {
  // world -> screen, like _fill_world_box: the erase path maps the same way, so a
  // missing offset here paints where the erase never cleans (10px-high ghost band).
  const int16_t wtlx = _sprite_tl(ex, hitbox, art);
  const int16_t wtly = _sprite_tl(ey, hitbox, art);
  const int16_t sx = wtlx - _cam_x;
  const int16_t sy = wtly - _cam_y + HUD_H;
  int16_t r0 = 0;
  int16_t r1 = (int16_t)art - 1;
  if (sy < HUD_H) {
    r0 = HUD_H - sy;
  }
  if (sy + r1 >= ARENA_BOTTOM) {
    r1 = ARENA_BOTTOM - 1 - sy;
  }
  if (r0 > r1) {
    return;
  }
  const int16_t sw = (int16_t)display::width();
  int16_t c0 = 0, c1 = (int16_t)art;
  if (sx < 0) {
    c0 = -sx;
  }
  if (sx + c1 > sw) {
    c1 = sw - sx;
  }
  if (c0 >= c1) {
    return;
  }
  // composite in RAM (terrain under transparent pixels), one atomic SPI burst.
  const int16_t w = c1 - c0, h = r1 - r0 + 1;
  for (int16_t row = 0; row < h; ++row) {
    const int16_t wy = wtly + r0 + row;
    const uint16_t* sprow = img + (uint16_t)(r0 + row) * art;
    for (int16_t cc = c0; cc < c1; ++cc) {
      const uint16_t c =
          pgm_read_word(sprow + (uint16_t)(flip ? (art - 1 - cc) : cc));
      _blit_buf[(uint16_t)row * w + (uint16_t)(cc - c0)] =
          (c != 0x0000) ? c : tilemap::color_at(wtlx + cc, wy);
    }
  }
  display::push_image(sx + c0, sy + r0, w, h, _blit_buf);
}

void render::_frame_box(int16_t sx, int16_t sy, uint16_t col) {
  // sx/sy arrive in world coords (sprite top-left): map to screen like the actor above
  const int16_t x0 = sx - _cam_x;
  const int16_t y0 = sy - _cam_y + HUD_H;
  // the downed frame only paints fully inside the arena, for the same erase symmetry
  if (x0 < 0 || y0 < HUD_H || x0 + SPRITE > (int16_t)display::width() ||
      y0 + SPRITE > ARENA_BOTTOM) {
    return;
  }
  display::fill_rect(x0, y0, SPRITE, 1, col);
  display::fill_rect(x0, y0 + SPRITE - 1, SPRITE, 1, col);
  display::fill_rect(x0, y0, 1, SPRITE, col);
  display::fill_rect(x0 + SPRITE - 1, y0, 1, SPRITE, col);
}

// 8-wind facing (0=E 1=SE 2=S 3=SW 4=W 5=NW 6=N 7=NE) onto stored art + mirror.
// Players store N,S,E,NE,SE (W side mirrored); zombies store N,S,E folded by
// halves (northbound shows N, southbound S, pure E/W the profile).
const uint16_t* render::_player_img(uint8_t p, uint8_t d, bool& flip) {
  flip = (d == 3 || d == 4 || d == 5); // SW/W/NW mirror SE/E/NE
  const uint8_t base = (d == 3) ? 1 : (d == 4) ? 0 : (d == 5) ? 7 : d;
  if (p == 0) {
    switch (base) {
      case 6: return player1_n;
      case 2: return player1_s;
      case 1: return player1_se;
      case 7: return player1_ne;
      default: return player1_e;
    }
  }
  switch (base) {
    case 6: return player2_n;
    case 2: return player2_s;
    case 1: return player2_se;
    case 7: return player2_ne;
    default: return player2_e;
  }
}

const uint16_t* render::_zombie_img(sim::actor_kind k, uint8_t d, bool& flip) {
  // 3-art fold by halves: northbound (5,6,7) shows N, southbound (1,2,3)
  // shows S, pure E/W keep the profile (W mirrored). Jitter inside a half
  // paints the same art, so straight chases never mirror-blink.
  flip = (d == 4); // pure west only
  const bool south = (d == 1 || d == 2 || d == 3);
  const bool north = (d == 5 || d == 6 || d == 7);
  if (k == sim::actor_kind::runner) {
    if (north) {
      return zombie_runner_n;
    }
    if (south) {
      return zombie_runner_s;
    }
    return zombie_runner_e;
  }
  if (k == sim::actor_kind::boss) {
    if (north) {
      return zombie_boss_n;
    }
    if (south) {
      return zombie_boss_s;
    }
    return zombie_boss_e;
  }
  if (north) {
    return zombie_n;
  }
  if (south) {
    return zombie_s;
  }
  return zombie_e;
}

// flat bullets: one box per shot, nothing to ghost on erase.
// The HUD carries no render text: game draws W/K, gun and role badge up there,
// stats live in the panel.
void render::draw() {
  repaint_step(); // terrain first, so a cut never paints over a live sprite

  const sim::state& v = sim::view();
  // zombies first, players on top: in contact the zombie used to cover you
  // (the boss is 32px over your 16px), now you cover it. Erase is untouched:
  // clear() wipes everything against the terrain before this runs.
  for (uint8_t i = 0; i < sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active) {
      bool flip = false;
      const uint16_t* img = _zombie_img(v.zombies[i].kind, v.zombies[i].facing & 7, flip);
      const uint8_t art = (v.zombies[i].kind == sim::actor_kind::boss) ? BOSS_ART : SPRITE;
      _draw_actor((int16_t)v.zombies[i].x, (int16_t)v.zombies[i].y, sim::ZOMBIE_SIZE, art,
                  img, flip);
    }
  }
  for (uint8_t p = 0; p < sim::NUM_PLAYERS; ++p) {
    if (!v.players[p].active) {
      continue;
    }
    bool flip = false;
    const uint16_t* img = _player_img(p, v.players[p].facing & 7, flip);
    if (v.players[p].downed) {
      const int16_t sx = _sprite_tl((int16_t)v.players[p].x, sim::PLAYER_SIZE, SPRITE);
      const int16_t sy = _sprite_tl((int16_t)v.players[p].y, sim::PLAYER_SIZE, SPRITE);
      _draw_actor((int16_t)v.players[p].x, (int16_t)v.players[p].y, sim::PLAYER_SIZE, SPRITE,
                  img, flip);
      _frame_box(sx, sy, colour::yellow); // body to rescue
    } else if (v.players[p].hp > 0) {
      _draw_actor((int16_t)v.players[p].x, (int16_t)v.players[p].y, sim::PLAYER_SIZE, SPRITE,
                  img, flip);
    }
  }
  for (uint8_t i = 0; i < sim::MAX_BULLETS; ++i) {
    if (v.bullets[i].active) {
      _fill_world_box((int16_t)v.bullets[i].x, (int16_t)v.bullets[i].y, sim::BULLET_SIZE,
                      colour::white);
    }
  }
  // NOTE: the paint runs every frame on purpose (the erase above is the conditional
  // one): actor/prompt erases can clobber tag pixels without staling them, and
  // repainting identical text self-heals. Text only shrinks on a buy, which always
  // stales the erase first, so no ghosts. On a camera cut the old tags linger until
  // the progressive repaint covers them (2 frames), while the new ones paint at once.
  _shop_labels(false);
  // prompt last, on top by design: a 32px boss walking behind the bottom strip is
  // covered there (reads as "cut"). Reordering would let the sprite bleed over UI text.
  if (_prompt != nullptr && _prompt[0] != '\0') {
    uint8_t len = 0;
    while (_prompt[len] != '\0') {
      ++len;
    }
    const int16_t tw = (int16_t)len * 6; // size-1 glyphs are 6px wide
    const int16_t sw = (int16_t)display::width();
    const int16_t sx = (sw - tw) / 2; // centred; the strip is always fully on screen
    display::text(_prompt, sx < 0 ? 0 : sx, ARENA_BOTTOM - _prompt_h + 1, _prompt_col, 1);
  }
}
