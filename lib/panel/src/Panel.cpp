#include <Arduino.h>

#include <Display.h>
#include <Map.h>
#include <Render.h>
#include <Sim.h>

#include "Panel.h"

int16_t Panel::mm_px[2 + Sim::MAX_ZOMBIES] = {0};
int16_t Panel::mm_py[2 + Sim::MAX_ZOMBIES] = {0};
uint8_t Panel::mm_n = 0;
int16_t Panel::mm_ctx = -1;
int16_t Panel::mm_cty = -1;
int16_t Panel::roll_tx = -1, Panel::roll_ty = -1; // set by game, painted lime in blips
int16_t Panel::roll_dx = -1, Panel::roll_dy = -1;

// stats cache: draw() only repaints when a value changes, blips() stays per-frame
static uint32_t cache_points = 0xFFFFFFFF;
static uint8_t cache_hp0 = 0xFF, cache_hp1 = 0xFF, cache_bleed0 = 0xFF, cache_bleed1 = 0xFF;
static uint8_t cache_dmg = 0xFF, cache_spd = 0xFF, cache_rpd = 0xFF;
static uint8_t cache_p2 = 0xFF, cache_down0 = 0xFF, cache_down1 = 0xFF;
static uint8_t cache_shown = 0xFF;

// P2 steel blue, brightened to read at 2px (same as the HP2 row and P2 prompts)
static constexpr uint16_t p2_blue = 0x54DA; // rgb565(80,152,208)

int16_t Panel::mm_x() {
  return (int16_t)Display::width() - mm_w - mm_gap;
}

void Panel::init() {
  const int16_t mx = mm_x();
  mm_ctx = -1; // the base repaint wipes the frame and the blips
  roll_tx = roll_ty = -1; // game re-pushes the active pad on the next playing frame
  roll_dx = roll_dy = -1;
  mm_n = 0;
  cache_points = 0xFFFFFFFF; // force the next draw() to repaint every row
  cache_hp0 = cache_hp1 = cache_bleed0 = cache_bleed1 = 0xFF;
  cache_dmg = cache_spd = cache_rpd = cache_p2 = cache_down0 = cache_down1 = 0xFF;
  cache_shown = 0xFF;
  Display::fill_rect(0, Render::ARENA_BOTTOM, (int16_t)Display::width(), panel_h, Colour::black);

  for (uint8_t r = 0; r < Tilemap::ROWS; ++r) { // minimap terrain, same-colour runs
    // flat base colours for the minimap; only the machines have sprite detail
    const int16_t sy = mm_y + (int16_t)r * mm_scale;
    const int16_t wy = (int16_t)r * Tilemap::TILE;
    int16_t run_x = 0;
    uint16_t run_col = Tilemap::color(Tilemap::tile_at(0, wy));

    for (uint8_t c = 1; c < Tilemap::COLS; ++c) {
      const uint16_t col = Tilemap::color(Tilemap::tile_at((int16_t)c * Tilemap::TILE, wy));
      if (col != run_col) {
        Display::fill_rect(mx + run_x * mm_scale, sy, (int16_t)(c - run_x) * mm_scale, mm_scale, run_col);
        run_x = c;
        run_col = col;
      }
    }
    Display::fill_rect(mx + run_x * mm_scale, sy, (int16_t)(Tilemap::COLS - run_x) * mm_scale, mm_scale, run_col);
  }
}

void Panel::set_roll(int16_t tx, int16_t ty) {
  roll_tx = tx;
  roll_ty = ty;
}

void Panel::pip_row(int16_t y, const char* label, uint8_t lvl, uint8_t max, uint16_t col) {
  Display::text(label, 4, y, Colour::white, 1);
  const uint16_t spent = Display::rgb565(40, 40, 40);
  for (uint8_t i = 0; i < max; ++i) {
    Display::fill_rect(28 + (int16_t)i * 9, y, 8, 8, (i < lvl) ? col : spent);
  }
}

void Panel::draw() {
  // stats live left of the minimap; every row is cleared first (shrinking numbers would leave ghost digits)
  // Cached on value change; init() invalidates
  const Sim::state& v = Sim::view();
  const uint8_t p2 = v.players[1].active ? 1 : 0;
  const uint8_t f = Render::focus();
  // spectator: bled-out (dead till the wave, not downed) shows the living partner's
  // build till the respawn; downed keeps its own (it rises with it)
  const uint8_t q = (uint8_t)(1 - f);
  const bool f_dead = v.players[f].active && v.players[f].hp == 0 && !v.players[f].downed;
  const bool q_alive = v.players[q].active && (v.players[q].hp > 0 || v.players[q].downed);
  const uint8_t s = (f_dead && q_alive) ? q : f;
  const uint8_t down0 = v.players[0].downed ? 1 : 0;
  const uint8_t down1 = v.players[1].downed ? 1 : 0;
  if (v.points == cache_points && v.players[0].hp == cache_hp0 &&
      v.players[1].hp == cache_hp1 && v.players[0].bleed == cache_bleed0 &&
      v.players[1].bleed == cache_bleed1 && v.dmg_lvl[s] == cache_dmg &&
      v.spd_lvl[s] == cache_spd && v.rpd_lvl[s] == cache_rpd && p2 == cache_p2 &&
      down0 == cache_down0 && down1 == cache_down1 && s == cache_shown) {
    return;
  }
  cache_points = v.points;
  cache_hp0 = v.players[0].hp;
  cache_hp1 = v.players[1].hp;
  cache_bleed0 = v.players[0].bleed;
  cache_bleed1 = v.players[1].bleed;
  cache_dmg = v.dmg_lvl[s];
  cache_spd = v.spd_lvl[s];
  cache_rpd = v.rpd_lvl[s];
  cache_p2 = p2;
  cache_down0 = down0;
  cache_down1 = down1;
  cache_shown = s;
  const int16_t mx = mm_x();
  char buf[32];

  Display::fill_rect(0, Render::ARENA_BOTTOM + 4, mx, 16, Colour::black);
  snprintf(buf, sizeof(buf), "POINTS %lu", v.points);
  Display::text(buf, 4, Render::ARENA_BOTTOM + 4, Colour::yellow, 2);

  // co-op spreads the rows (10px pitch, 2px gutters) to fit HP2 + the 3 buff
  // lines: HP1 192, HP2 202, DMG 212, SPD 222, ROF 232..240 (Panel edge, exact);
  // solo keeps 12px
  const int16_t hp_y = Render::ARENA_BOTTOM + (p2 ? 22 : 24);
  const int16_t pitch = p2 ? 10 : 12;
  const int16_t tail_y = hp_y + (p2 ? 20 : 12); // DMG row (HP1, [+HP2,] then DMG/SPD/ROF)

  auto hp_row = [&](uint8_t p, int16_t y, const char* label, uint16_t ok_col) {
    Display::fill_rect(0, y, mx, 8, Colour::black);
    if (v.players[p].downed) {
      // countdown starts where the pips start: same column, same row
      Display::text(label, 4, y, Colour::white, 1);
      snprintf(buf, sizeof(buf), "DOWN %u", v.players[p].bleed);
      Display::text(buf, 28, y, Colour::yellow, 1);
      return;
    }
    const uint16_t col = (v.players[p].hp * 5 <= Sim::PLAYER_HP_MAX * 2) ? Colour::red : ok_col; // low hp reads red
    pip_row(y, label, v.players[p].hp, Sim::PLAYER_HP_MAX, col);
  };
  hp_row(0, hp_y, p2 ? "HP1" : "HP", Colour::green);
  if (p2) {
    hp_row(1, hp_y + pitch, "HP2", p2_blue);
  }
  // buffs are % text only: this board's build (or the living partner's while
  // spectating bled-out), left-aligned at x=28. Bold via a double draw
  auto buff_row = [&](int16_t y, const char* label, uint8_t b, char sign, uint16_t col) {
    Display::fill_rect(0, y, mx, 8, Colour::black);
    Display::text(label, 4, y, Colour::white, 1);
    snprintf(buf, sizeof(buf), "%c%u%%", sign, b);
    Display::text(buf, 28, y, col, 1);
    Display::text(buf, 29, y, col, 1);
  };
  buff_row(tail_y, "DMG", Sim::dmg_bonus(v.dmg_lvl[s]), '+', Colour::red);
  buff_row(tail_y + pitch, "SPD", Sim::spd_bonus(v.spd_lvl[s]), '+', Display::rgb565(60, 130, 230));
  buff_row(tail_y + 2 * pitch, "ROF", Sim::rpd_cut(v.rpd_lvl[s]), '-', Colour::orange);
}

void Panel::mm_restore_row(int16_t tx0, int16_t tx1, int16_t ty) {
  const int16_t mx = mm_x();
  const int16_t sy = mm_y + ty * mm_scale;
  int16_t run_x = tx0;
  uint16_t run_col = Tilemap::color(Tilemap::tile_at(tx0 * Tilemap::TILE, ty * Tilemap::TILE));

  for (int16_t tx = tx0 + 1; tx <= tx1; ++tx) {
    const uint16_t col = Tilemap::color(Tilemap::tile_at(tx * Tilemap::TILE, ty * Tilemap::TILE));
    if (col != run_col) {
      Display::fill_rect(mx + run_x * mm_scale, sy, (tx - run_x) * mm_scale, mm_scale, run_col);
      run_x = tx;
      run_col = col;
    }
  }
  Display::fill_rect(mx + run_x * mm_scale, sy, (tx1 - run_x + 1) * mm_scale, mm_scale, run_col);
}

void Panel::mm_restore_col(int16_t tx, int16_t ty0, int16_t ty1) {
  const int16_t mx = mm_x() + tx * mm_scale;
  int16_t run_y = ty0;
  uint16_t run_col = Tilemap::color(Tilemap::tile_at(tx * Tilemap::TILE, ty0 * Tilemap::TILE));

  for (int16_t ty = ty0 + 1; ty <= ty1; ++ty) {
    const uint16_t col = Tilemap::color(Tilemap::tile_at(tx * Tilemap::TILE, ty * Tilemap::TILE));
    if (col != run_col) {
      Display::fill_rect(mx, mm_y + run_y * mm_scale, mm_scale, (ty - run_y) * mm_scale, run_col);
      run_y = ty;
      run_col = col;
    }
  }
  Display::fill_rect(mx, mm_y + run_y * mm_scale, mm_scale, (ty1 - run_y + 1) * mm_scale, run_col);
}

void Panel::mm_dot(int16_t wx, int16_t wy, uint16_t col) {
  if (mm_n >= (uint8_t)(2 + Sim::MAX_ZOMBIES)) {
    return;
  }
  Display::fill_rect(mm_x() + (wx / Tilemap::TILE) * mm_scale, mm_y + (wy / Tilemap::TILE) * mm_scale, mm_scale, mm_scale, col);
  mm_px[mm_n] = wx;
  mm_py[mm_n] = wy;
  ++mm_n;
}

void Panel::mm_frame() {
  // the camera snaps to a whole cell, so the frame always lands on even minimap pixels
  const int16_t cell_w = (int16_t)(Display::width() / Tilemap::TILE);
  const int16_t cell_h = (int16_t)(Render::ARENA_H / Tilemap::TILE);
  const int16_t ctx = (int16_t)(Render::cam_x() / Tilemap::TILE);
  const int16_t cty = (int16_t)(Render::cam_y() / Tilemap::TILE);

  if (mm_ctx >= 0) { // put the terrain back under the frame drawn last frame
    mm_restore_row(mm_ctx, mm_ctx + cell_w - 1, mm_cty);
    mm_restore_row(mm_ctx, mm_ctx + cell_w - 1, mm_cty + cell_h - 1);
    mm_restore_col(mm_ctx, mm_cty, mm_cty + cell_h - 1);
    mm_restore_col(mm_ctx + cell_w - 1, mm_cty, mm_cty + cell_h - 1);
  }

  const int16_t fx = mm_x() + ctx * mm_scale;
  const int16_t fy = mm_y + cty * mm_scale;
  const int16_t fw = cell_w * mm_scale;
  const int16_t fh = cell_h * mm_scale;

  Display::fill_rect(fx, fy, fw, 1, Colour::yellow);
  Display::fill_rect(fx, fy + fh - 1, fw, 1, Colour::yellow);
  Display::fill_rect(fx, fy, 1, fh, Colour::yellow);
  Display::fill_rect(fx + fw - 1, fy, 1, fh, Colour::yellow);

  mm_ctx = ctx;
  mm_cty = cty;
}

void Panel::blips() {
  for (uint8_t i = 0; i < mm_n; ++i) { // restore the terrain under last frame's dots
    const int16_t tx = mm_px[i] / Tilemap::TILE;
    const int16_t ty = mm_py[i] / Tilemap::TILE;
    Display::fill_rect(mm_x() + tx * mm_scale, mm_y + ty * mm_scale, mm_scale, mm_scale, Tilemap::color(Tilemap::tile_at(mm_px[i], mm_py[i])));
  }
  mm_n = 0;

  if (roll_dx >= 0) { // put the terrain back under last frame's pad marker
    mm_restore_row(roll_dx, roll_dx + 1, roll_dy);
    mm_restore_row(roll_dx, roll_dx + 1, roll_dy + 1);
    roll_dx = roll_dy = -1;
  }
  if (roll_tx >= 0 && roll_ty >= 0 && roll_tx + 1 < Tilemap::COLS &&
      roll_ty + 1 < Tilemap::ROWS) {
    // active roulette pad: solid lime 2x2 under the dots (lime is free on the
    // minimap: players green/steel-blue/yellow, zombies red/orange/purple)
    Display::fill_rect(mm_x() + roll_tx * mm_scale, mm_y + roll_ty * mm_scale, (int16_t)(2 * mm_scale), (int16_t)(2 * mm_scale), Colour::lime);
    roll_dx = roll_tx;
    roll_dy = roll_ty;
  }

  mm_frame();

  const Sim::state& v = Sim::view();
  for (uint8_t p = 0; p < Sim::NUM_PLAYERS; ++p) {
    if (!v.players[p].active || (v.players[p].hp == 0 && !v.players[p].downed)) {
      continue; // inactive, or bled out (dead till the wave): no dot. Downed keeps
    }           // its yellow dot (hp reads 0 there too, so the downed check matters)
    uint16_t col = (p == 0) ? Colour::green : p2_blue;
    if (v.players[p].downed) {
      col = Colour::yellow; // body to rescue
    } else if (v.players[p].hp * 5 <= Sim::PLAYER_HP_MAX * 2) {
      col = Colour::red; // critical, same gate as the pip rows
    }
    mm_dot((int16_t)v.players[p].x, (int16_t)v.players[p].y, col);
  }
  for (uint8_t i = 0; i < Sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active) {
      uint16_t col = Colour::red;
      if (v.zombies[i].kind == Sim::actor_kind::runner) {
        col = Colour::orange;
      } else if (v.zombies[i].kind == Sim::actor_kind::boss) {
        col = Colour::purple;
      }
      mm_dot((int16_t)v.zombies[i].x, (int16_t)v.zombies[i].y, col);
    }
  }
}
