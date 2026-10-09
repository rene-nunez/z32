#include <Arduino.h>
#include <cstdio>
#include <pgmspace.h>

#include <display.h>
#include <map.h>
#include <sim.h>
#include <sprites.h>

#include "render.h"

// render core: camera, terrain repaint, entity erase and the frame draw
// Actors live in actors.cpp, price tags and the prompt strip in tags.cpp;
// every class static below is defined once here and used across them

int16_t render::_cam_x = 0;
int16_t render::_cam_y = 0;
uint8_t render::_focus = 0;
int16_t render::_paint_y = render::ARENA_H;
const char* render::_prompt = nullptr;
uint16_t render::_prompt_col = colour::yellow;
// conditional strip erase: text the strip was last erased for ("" = clean terrain)
// plus the camera of that erase. Steady text + steady camera skips the 320x10
// terrain repaint every frame
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
  // force a full HUD repaint via game (which wipes + repaints all fields itself)
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
  // focus still frames its body (spectate the rescue); only an inactive focus falls back
  // A bled-out body (active, hp 0, not downed) spectates the living partner until the
  // next wave respawns it: the rule is per frame, so the camera returns on its own
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
      const uint8_t art = (v.zombies[i].kind == sim::actor_kind::boss) ? BOSS_ART : SPRITE;
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
  // rect moves with the camera and a camera cut would strand it otherwise
  // conditional: _prompt still holds last frame's text here (game sets the new one
  // after the step). Steady text + steady camera = nothing to clean, so the
  // 320x10 terrain repaint (3200 color_at/frame) runs only on text change,
  // camera move or progressive repaint. draw() repaints the text every frame
  // with an opaque bg, so it self-covers; the erase is always full-width, so
  // shrinking text never strands pixels
  const char* cur_prompt = (_prompt != nullptr) ? _prompt : "";
  if ((cur_prompt[0] != '\0' || _prompt_erased[0] != '\0') &&
      (strcmp(cur_prompt, _prompt_erased) != 0 || _cam_x != _prompt_ex ||
       _cam_y != _prompt_ey || _paint_y < ARENA_H)) {
    _erase_world_area(_cam_x, _cam_y + ARENA_H - _prompt_h, (int16_t)display::width(), _prompt_h);
    snprintf(_prompt_erased, sizeof(_prompt_erased), "%s", cur_prompt);
    _prompt_ex = _cam_x;
    _prompt_ey = _cam_y;
  }
}

// flat bullets: one box per shot, nothing to ghost on erase
// The HUD carries no render text: game draws W/K, gun and role badge up there,
// stats live in the panel
void render::draw() {
  repaint_step(); // terrain first, so a cut never paints over a live sprite

  const sim::state& v = sim::view();
  // zombies first, players on top (the boss is 32px over your 16px). Erase is untouched:
  // clear() wipes everything against the terrain before this runs
  for (uint8_t i = 0; i < sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active) {
      bool flip = false;
      const uint16_t* img = _zombie_img(v.zombies[i].kind, v.zombies[i].facing & 7, flip);
      const uint8_t art = (v.zombies[i].kind == sim::actor_kind::boss) ? BOSS_ART : SPRITE;
      _draw_actor((int16_t)v.zombies[i].x, (int16_t)v.zombies[i].y, sim::ZOMBIE_SIZE, art, img, flip);
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
      _draw_actor((int16_t)v.players[p].x, (int16_t)v.players[p].y, sim::PLAYER_SIZE, SPRITE, img, flip);
      _frame_box(sx, sy, colour::yellow); // body to rescue
    } else if (v.players[p].hp > 0) {
      _draw_actor((int16_t)v.players[p].x, (int16_t)v.players[p].y, sim::PLAYER_SIZE, SPRITE, img, flip);
    }
  }
  for (uint8_t i = 0; i < sim::MAX_BULLETS; ++i) {
    if (v.bullets[i].active) {
      _fill_world_box((int16_t)v.bullets[i].x, (int16_t)v.bullets[i].y, sim::BULLET_SIZE, colour::white);
    }
  }
  // NOTE: the paint runs every frame on purpose (the erase above is the conditional
  // one): actor/prompt erases can clobber tag pixels without staling them, and
  // repainting identical text self-heals. Text only shrinks on a buy, which always
  // stales the erase first, so no ghosts. On a camera cut the old tags linger until
  // the progressive repaint covers them (2 frames), while the new ones paint at once
  _shop_labels(false);
  // prompt last, on top by design: a 32px boss walking behind the bottom strip is
  // covered there (reads as "cut"). Reordering would let the sprite bleed over UI text
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
