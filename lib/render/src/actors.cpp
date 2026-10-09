#include <Arduino.h>
#include <cstdio>
#include <pgmspace.h>

#include <display.h>
#include <map.h>
#include <sim.h>
#include <sprites.h>

#include "render.h"

// render actors: sprite composites, facing art selection and the opaque-bbox
// erase. One RAM blit per actor, terrain under transparent pixels

// Opaque bbox per art array, scanned once from PROGMEM: erase only repaints what draw
// could have painted (transparent margins are never touched). The 32px boss is mostly
// air, so this cuts its erase to the body. Cached by pointer (19 arts, one scan each)
struct _bbox {
  const uint16_t* img;
  uint8_t art;
  uint8_t x0, y0, w, h;
};
static _bbox _bbox_cache[24];
static uint8_t _bbox_n = 0;

// bbox of img (mirrored on x when flip, matching the composite blit below). Full rect
// cache overflows (never: 19 arts). Empty art (never) yields w=h=0: caller skips
void render::_art_bbox(const uint16_t* img, uint8_t art, bool flip, uint8_t& x0, uint8_t& y0, uint8_t& w, uint8_t& h) {
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
// tilemap, exactly like the old run path (0x0000 = see-through)
static uint16_t _blit_buf[32 * 32];

// terrain-only push of a world rect (actor erase). Same pixels as _erase_world_area,
// one burst instead of per-run fills
void render::_erase_box(int16_t wx, int16_t wy, int16_t w, int16_t h) {
  if (w <= 0 || h <= 0) {
    return;
  }
  const int16_t sw = (int16_t)display::width();
  int16_t sy0 = wy - cam_y() + HUD_H;
  int16_t x0 = wx, y0 = wy, bw = w, bh = h;
  if (sy0 < HUD_H) { // clip to the arena, like _fill_world_run
    const int16_t cut = HUD_H - sy0;
    y0 += cut;
    bh -= cut;
    sy0 = HUD_H;
  }
  if (sy0 + bh > ARENA_BOTTOM) {
    bh = ARENA_BOTTOM - sy0;
  }
  if (bh <= 0) {
    return;
  }
  int16_t sx0 = x0 - cam_x();
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

// art centred on the hitbox: draw and erase share this so no pixel ghosts
int16_t render::_sprite_tl(int16_t e, uint8_t hitbox, uint8_t art) {
  return e + ((int16_t)hitbox - (int16_t)art) / 2;
}

void render::_draw_actor(int16_t ex, int16_t ey, uint8_t hitbox, uint8_t art, const uint16_t* img, bool flip) {
  // world -> screen, like _fill_world_box: the erase path maps the same way, so a
  // missing offset here paints where the erase never cleans (10px-high ghost band)
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
  const int16_t w = c1 - c0, h = r1 - r0 + 1;
  for (int16_t row = 0; row < h; ++row) {
    const int16_t wy = wtly + r0 + row;
    const uint16_t* sprow = img + (uint16_t)(r0 + row) * art;
    for (int16_t cc = c0; cc < c1; ++cc) {
      const uint16_t c = pgm_read_word(sprow + (uint16_t)(flip ? (art - 1 - cc) : cc));
      _blit_buf[(uint16_t)row * w + (uint16_t)(cc - c0)] = (c != 0x0000) ? c : tilemap::color_at(wtlx + cc, wy);
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

// 8-wind facing (0=E 1=SE 2=S 3=SW 4=W 5=NW 6=N 7=NE) onto stored art + mirror
// Players store N,S,E,NE,SE (W side mirrored); zombies store N,S,E folded by
// halves (northbound shows N, southbound S, pure E/W the profile)
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
  // paints the same art, so straight chases never mirror-blink
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
