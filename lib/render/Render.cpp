// Render core: camera, terrain and frame draw

#include <cstdio>
#include <cstring>

#include <Display.h>
#include <Map.h>
#include <Sim.h>
#include <Sprites.h>

#include "Render.h"

int16_t Render::camera_x = 0;
int16_t Render::camera_y = 0;
uint8_t Render::focus_player = 0;
int16_t Render::paint_y = Render::ARENA_H;
const char* Render::prompt_text = nullptr;
uint16_t Render::prompt_col = Colour::yellow;
static char prompt_erased[28] = "";
static int16_t prompt_ex = -1, prompt_ey = -1;

void Render::prompt(const char* msg, uint16_t color) {
  prompt_text = msg;
  prompt_col = color;
}

int16_t Render::cam_x() {
  return camera_x;
}

int16_t Render::cam_y() {
  return camera_y;
}

void Render::repaint() {
  paint_y = 0;
}

void Render::repaint_step() {
  if (paint_y >= ARENA_H) {
    return;
  }

  const int16_t sw = (int16_t)Display::width();
  const int16_t end = (paint_y + PAINT_CHUNK < ARENA_H) ? paint_y + PAINT_CHUNK : ARENA_H;

  for (int16_t ry = paint_y; ry < end; ++ry) {
    const int16_t wy = camera_y + ry;
    const int16_t sy = HUD_H + ry;
    int16_t run_x = 0;
    uint16_t run_col = Tilemap::color_at(camera_x, wy);

    for (int16_t rx = 1; rx < sw; ++rx) {
      const uint16_t col = Tilemap::color_at(camera_x + rx, wy);
      if (col != run_col) {
        Display::fill_rect(run_x, sy, rx - run_x, 1, run_col);
        run_x = rx;
        run_col = col;
      }
    }
    Display::fill_rect(run_x, sy, sw - run_x, 1, run_col);
  }

  paint_y = end;
}

int16_t Render::cell_cam(int16_t p, int16_t step, int16_t max_cam) {
  int16_t cam = (int16_t)((p / step) * step);
  if (cam < 0) {
    cam = 0;
  } else if (cam > max_cam) {
    cam = max_cam;
  }
  return cam;
}

void Render::set_focus(uint8_t p) {
  focus_player = (p < Sim::NUM_PLAYERS) ? p : 0;
  camera_x = -1;
  camera_y = -1;
}

uint8_t Render::focus() {
  return focus_player;
}

void Render::update_camera() {
  const int16_t aw = (int16_t)Display::width();
  const Sim::state& v = Sim::view();
  uint8_t f = focus_player;
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
  const int16_t pcx = (int16_t)(v.players[f].x + Sim::PLAYER_SIZE / 2);
  const int16_t pcy = (int16_t)(v.players[f].y + Sim::PLAYER_SIZE / 2);

  const int16_t cx = cell_cam(pcx, aw, (int16_t)(Tilemap::WORLD_W - aw));
  const int16_t cy = cell_cam(pcy, ARENA_H, (int16_t)(Tilemap::WORLD_H - ARENA_H));

  if (cx == camera_x && cy == camera_y) {
    return;
  }
  camera_x = cx;
  camera_y = cy;
  repaint();
}

void Render::fill_world_run(int16_t wx, int16_t sy, int16_t w, uint16_t col) {
  if (w <= 0 || sy < HUD_H || sy >= ARENA_BOTTOM) {
    return;
  }
  const int16_t sw = (int16_t)Display::width();
  int16_t x0 = wx - camera_x;
  int16_t x1 = x0 + w;
  if (x1 <= 0 || x0 >= sw) {
    return;
  }
  if (x0 < 0) {
    x0 = 0;
  }
  if (x1 > sw) {
    x1 = sw;
  }
  Display::fill_rect(x0, sy, x1 - x0, 1, col);
}

void Render::erase_world_rect(int16_t wx, int16_t wy, uint8_t size) {
  erase_world_area(wx, wy, size, size);
}

void Render::erase_world_area(int16_t wx, int16_t wy, int16_t w, int16_t h) {
  for (int16_t dy = 0; dy < h; ++dy) {
    const int16_t wyy = wy + dy;
    const int16_t sy = wyy - camera_y + HUD_H;
    if (sy < HUD_H || sy >= ARENA_BOTTOM) {
      continue;
    }

    int16_t run_x = wx;
    uint16_t run_col = Tilemap::color_at(wx, wyy);
    for (int16_t dx = 1; dx < w; ++dx) {
      const uint16_t col = Tilemap::color_at(wx + dx, wyy);
      if (col != run_col) {
        fill_world_run(run_x, sy, (int16_t)(wx + dx - run_x), run_col);
        run_x = wx + dx;
        run_col = col;
      }
    }
    fill_world_run(run_x, sy, (int16_t)(wx + w - run_x), run_col);
  }
}

void Render::fill_world_box(int16_t wx, int16_t wy, uint8_t size, uint16_t col) {
  const int x0 = wx - camera_x;
  const int y0 = wy - camera_y + HUD_H;
  const int cx0 = x0 > 0 ? x0 : 0;
  const int cy0 = y0 > HUD_H ? y0 : HUD_H;
  const int cx1 = x0 + size < (int)Display::width() ? x0 + size : (int)Display::width();
  const int cy1 = y0 + size < (int)ARENA_BOTTOM ? y0 + size : (int)ARENA_BOTTOM;

  if (cx0 < cx1 && cy0 < cy1) {
    Display::fill_rect(cx0, cy0, cx1 - cx0, cy1 - cy0, col);
  }
}

void Render::clear() {
  const Sim::state& v = Sim::view();
  for (uint8_t p = 0; p < Sim::NUM_PLAYERS; ++p) {
    if (v.players[p].active) {
      const int16_t tlx = sprite_tl((int16_t)v.players[p].x, Sim::PLAYER_SIZE, SPRITE);
      const int16_t tly = sprite_tl((int16_t)v.players[p].y, Sim::PLAYER_SIZE, SPRITE);
      if (v.players[p].downed) {
        erase_box(tlx, tly, SPRITE, SPRITE);
        continue;
      }
      bool flip = false;
      const uint16_t* img = player_img(p, v.players[p].facing & 7, flip);
      uint8_t bx0, by0, bw, bh;
      art_bbox(img, SPRITE, flip, bx0, by0, bw, bh);
      if (bw > 0 && bh > 0) {
        erase_box(tlx + bx0, tly + by0, bw, bh);
      }
    }
  }
  for (uint8_t i = 0; i < Sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active) {
      const uint8_t art = (v.zombies[i].kind == Sim::actor_kind::boss) ? BOSS_ART : SPRITE;
      const int16_t tlx = sprite_tl((int16_t)v.zombies[i].x, Sim::ZOMBIE_SIZE, art);
      const int16_t tly = sprite_tl((int16_t)v.zombies[i].y, Sim::ZOMBIE_SIZE, art);
      bool flip = false;
      const uint16_t* img = zombie_img(v.zombies[i].kind, v.zombies[i].facing & 7, flip);
      uint8_t bx0, by0, bw, bh;
      art_bbox(img, art, flip, bx0, by0, bw, bh);
      if (bw > 0 && bh > 0) {
        erase_box(tlx + bx0, tly + by0, bw, bh);
      }
    }
  }
  for (uint8_t i = 0; i < Sim::MAX_BULLETS; ++i) {
    if (v.bullets[i].active) {
      erase_world_rect((int16_t)v.bullets[i].x, (int16_t)v.bullets[i].y, Sim::BULLET_SIZE);
    }
  }
  if (tags_stale()) {
    shop_labels(true);
  }
  const char* cur_prompt = (prompt_text != nullptr) ? prompt_text : "";
  if ((cur_prompt[0] != '\0' || prompt_erased[0] != '\0') &&
      (strcmp(cur_prompt, prompt_erased) != 0 || camera_x != prompt_ex ||
       camera_y != prompt_ey || paint_y < ARENA_H)) {
    erase_world_area(camera_x, camera_y + ARENA_H - prompt_h, (int16_t)Display::width(), prompt_h);
    snprintf(prompt_erased, sizeof(prompt_erased), "%s", cur_prompt);
    prompt_ex = camera_x;
    prompt_ey = camera_y;
  }
}

void Render::draw() {
  repaint_step();

  const Sim::state& v = Sim::view();
  for (uint8_t i = 0; i < Sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active) {
      bool flip = false;
      const uint16_t* img = zombie_img(v.zombies[i].kind, v.zombies[i].facing & 7, flip);
      const uint8_t art = (v.zombies[i].kind == Sim::actor_kind::boss) ? BOSS_ART : SPRITE;
      draw_actor((int16_t)v.zombies[i].x, (int16_t)v.zombies[i].y, Sim::ZOMBIE_SIZE, art, img, flip);
    }
  }
  for (uint8_t p = 0; p < Sim::NUM_PLAYERS; ++p) {
    if (!v.players[p].active) {
      continue;
    }
    bool flip = false;
    const uint16_t* img = player_img(p, v.players[p].facing & 7, flip);
    if (v.players[p].downed) {
      const int16_t sx = sprite_tl((int16_t)v.players[p].x, Sim::PLAYER_SIZE, SPRITE);
      const int16_t sy = sprite_tl((int16_t)v.players[p].y, Sim::PLAYER_SIZE, SPRITE);
      draw_actor((int16_t)v.players[p].x, (int16_t)v.players[p].y, Sim::PLAYER_SIZE, SPRITE, img, flip);
      frame_box(sx, sy, Colour::yellow);
    } else if (v.players[p].hp > 0) {
      draw_actor((int16_t)v.players[p].x, (int16_t)v.players[p].y, Sim::PLAYER_SIZE, SPRITE, img, flip);
    }
  }
  for (uint8_t i = 0; i < Sim::MAX_BULLETS; ++i) {
    if (v.bullets[i].active) {
      fill_world_box((int16_t)v.bullets[i].x, (int16_t)v.bullets[i].y, Sim::BULLET_SIZE, Colour::white);
    }
  }
  shop_labels(false);
  if (prompt_text != nullptr && prompt_text[0] != '\0') {
    uint8_t len = 0;
    while (prompt_text[len] != '\0') {
      ++len;
    }
    const int16_t tw = (int16_t)len * 6;
    const int16_t sw = (int16_t)Display::width();
    const int16_t sx = (sw - tw) / 2;
    Display::text(prompt_text, sx < 0 ? 0 : sx, ARENA_BOTTOM - prompt_h + 1, prompt_col, 1);
  }
}
