#include <Arduino.h>

#include <Display.h>
#include <map.h>
#include <Render.h>
#include <Sim.h>

#include "Panel.h"

int16_t panel::_mm_px[2 + sim::MAX_ZOMBIES] = {0};
int16_t panel::_mm_py[2 + sim::MAX_ZOMBIES] = {0};
uint8_t panel::_mm_n = 0;
int16_t panel::_mm_ctx = -1; // camera cell tile whose frame is on the minimap, -1 = none yet
int16_t panel::_mm_cty = -1;
int16_t panel::_roll_tx = -1, panel::_roll_ty = -1; // set by game, painted green in blips
int16_t panel::_roll_dx = -1, panel::_roll_dy = -1; // last painted marker, restored next frame
// stats cache: draw() only repaints when a value changes, blips() stays per-frame
static uint32_t _cache_points = 0xFFFFFFFF;
static uint8_t _cache_hp0 = 0xFF, _cache_hp1 = 0xFF, _cache_bleed0 = 0xFF, _cache_bleed1 = 0xFF;
static uint8_t _cache_dmg = 0xFF, _cache_spd = 0xFF, _cache_rpd = 0xFF;
static uint8_t _cache_p2 = 0xFF, _cache_down0 = 0xFF, _cache_down1 = 0xFF;
static uint8_t _cache_shown = 0xFF; // displayed player: focus, or partner while spectating

int16_t panel::_mm_x() {
  return (int16_t)display::width() - _mm_w - _mm_gap;
}

void panel::init() {
  const int16_t mx = _mm_x();
  _mm_ctx = -1; // the base repaint wipes the frame and the blips
  _roll_tx = _roll_ty = -1; // game re-pushes the active pad on the next playing frame
  _roll_dx = _roll_dy = -1;
  _mm_n = 0;
  _cache_points = 0xFFFFFFFF; // force the next draw() to repaint every row
  _cache_hp0 = _cache_hp1 = _cache_bleed0 = _cache_bleed1 = 0xFF;
  _cache_dmg = _cache_spd = _cache_rpd = _cache_p2 = _cache_down0 = _cache_down1 = 0xFF;
  _cache_shown = 0xFF;
  display::fill_rect(0, render::ARENA_BOTTOM, (int16_t)display::width(), _panel_h, colour::black);

  for (uint8_t r = 0; r < tilemap::ROWS; ++r) { // minimap terrain, same-colour runs
    // flat base colours for the minimap; only the machines have sprite detail
    const int16_t sy = _mm_y + (int16_t)r * _mm_scale;
    const int16_t wy = (int16_t)r * tilemap::TILE;
    int16_t run_x = 0;
    uint16_t run_col = tilemap::color(tilemap::tile_at(0, wy));

    for (uint8_t c = 1; c < tilemap::COLS; ++c) {
      const uint16_t col = tilemap::color(tilemap::tile_at((int16_t)c * tilemap::TILE, wy));
      if (col != run_col) {
        display::fill_rect(mx + run_x * _mm_scale, sy, (int16_t)(c - run_x) * _mm_scale, _mm_scale,
                           run_col);
        run_x = c;
        run_col = col;
      }
    }
    display::fill_rect(mx + run_x * _mm_scale, sy, (int16_t)(tilemap::COLS - run_x) * _mm_scale,
                       _mm_scale, run_col);
  }
  _mm_n = 0;
}

void panel::set_roll(int16_t tx, int16_t ty) {
  _roll_tx = tx;
  _roll_ty = ty;
}

void panel::_pip_row(int16_t y, const char* label, uint8_t lvl, uint8_t max, uint16_t col) {
  display::text(label, 4, y, colour::white, 1);
  const uint16_t spent = display::rgb565(40, 40, 40);
  for (uint8_t i = 0; i < max; ++i) {
    display::fill_rect(28 + (int16_t)i * 9, y, 8, 8, (i < lvl) ? col : spent);
  }
}

void panel::draw() {
  // stats live left of the minimap; wave/kills and the gun moved to the 10px HUD, so this
  // keeps POINTS big plus the pip rows with room to breathe. Every row is cleared first:
  // numbers shrink and overpainting alone would leave ghost digits behind.
  // cached: POINTS/HP/buffs only change on kills/buys/hits, so most frames skip here
  // entirely (blips() still runs per frame). init() invalidates the cache.
  const sim::state& v = sim::view();
  const uint8_t p2 = v.players[1].active ? 1 : 0;
  const uint8_t f = render::focus(); // this board's build: host/solo P1, client P2
  // spectator: bled-out (dead till the wave, not downed) shows the living partner's
  // build till the respawn; downed keeps its own (it rises with it)
  const uint8_t q = (uint8_t)(1 - f);
  const bool f_dead = v.players[f].active && v.players[f].hp == 0 && !v.players[f].downed;
  const bool q_alive = v.players[q].active && (v.players[q].hp > 0 || v.players[q].downed);
  const uint8_t s = (f_dead && q_alive) ? q : f;
  const uint8_t down0 = v.players[0].downed ? 1 : 0;
  const uint8_t down1 = v.players[1].downed ? 1 : 0;
  if (v.points == _cache_points && v.players[0].hp == _cache_hp0 &&
      v.players[1].hp == _cache_hp1 && v.players[0].bleed == _cache_bleed0 &&
      v.players[1].bleed == _cache_bleed1 && v.dmg_lvl[s] == _cache_dmg &&
      v.spd_lvl[s] == _cache_spd && v.rpd_lvl[s] == _cache_rpd && p2 == _cache_p2 &&
      down0 == _cache_down0 && down1 == _cache_down1 && s == _cache_shown) {
    return;
  }
  _cache_points = v.points;
  _cache_hp0 = v.players[0].hp;
  _cache_hp1 = v.players[1].hp;
  _cache_bleed0 = v.players[0].bleed;
  _cache_bleed1 = v.players[1].bleed;
  _cache_dmg = v.dmg_lvl[s];
  _cache_spd = v.spd_lvl[s];
  _cache_rpd = v.rpd_lvl[s];
  _cache_p2 = p2;
  _cache_down0 = down0;
  _cache_down1 = down1;
  _cache_shown = s;
  const int16_t mx = _mm_x();
  char buf[32];

  display::fill_rect(0, render::ARENA_BOTTOM + 4, mx, 16, colour::black);
  snprintf(buf, sizeof(buf), "POINTS %lu", v.points);
  display::text(buf, 4, render::ARENA_BOTTOM + 4, colour::yellow, 2);

  // co-op spreads the rows (10px pitch, 2px gutters) to fit HP2 + the 3 buff
  // lines: HP1 192, HP2 202, DMG 212, SPD 222, ROF 232..240 (panel edge, exact);
  // solo keeps 12px
  const int16_t hp_y = render::ARENA_BOTTOM + (p2 ? 22 : 24);
  const int16_t pitch = p2 ? 10 : 12;
  const int16_t tail_y = hp_y + (p2 ? 20 : 12); // DMG row (HP1, [+HP2,] then DMG/SPD/ROF)

  auto hp_row = [&](uint8_t p, int16_t y, const char* label, uint16_t ok_col) {
    display::fill_rect(0, y, mx, 8, colour::black);
    if (v.players[p].downed) {
      // countdown starts where the pips start: same column, same row
      display::text(label, 4, y, colour::white, 1);
      snprintf(buf, sizeof(buf), "DOWN %u", v.players[p].bleed);
      display::text(buf, 28, y, colour::yellow, 1); // seconds shrink, cleared above
      return;
    }
    const uint16_t col = (v.players[p].hp * 5 <= sim::PLAYER_HP_MAX * 2) ? colour::red
                                                                                   : ok_col; // low hp reads red
    _pip_row(y, label, v.players[p].hp, sim::PLAYER_HP_MAX, col);
  };
  hp_row(0, hp_y, p2 ? "HP1" : "HP", colour::green); // P1 suit reads green already
  if (p2) {
    // P2 suit is dark steel blue (0x3310): same hue, brightened to read at 8px
    constexpr uint16_t hp2_col = 0x54DA; // rgb565(80,152,208)
    hp_row(1, hp_y + pitch, "HP2", hp2_col);
  }
  // buffs are % text only (no pips): this board's build, or the living partner's
  // while spectating bled-out (focus: host/solo P1, client P2), left-aligned at
  // x=28 hugging the label. Bold via a double draw; every row is cleared first
  // so shrinking text leaves no ghosts.
  auto buff_row = [&](int16_t y, const char* label, uint8_t b, char sign, uint16_t col) {
    display::fill_rect(0, y, mx, 8, colour::black);
    display::text(label, 4, y, colour::white, 1);
    snprintf(buf, sizeof(buf), "%c%u%%", sign, b);
    display::text(buf, 28, y, col, 1);
    display::text(buf, 29, y, col, 1);
  };
  buff_row(tail_y, "DMG", sim::dmg_bonus(v.dmg_lvl[s]), '+', colour::red);
  buff_row(tail_y + pitch, "SPD", sim::spd_bonus(v.spd_lvl[s]), '+',
           display::rgb565(60, 130, 230));
  buff_row(tail_y + 2 * pitch, "ROF", sim::rpd_cut(v.rpd_lvl[s]), '-', colour::orange);
}

void panel::_mm_restore_row(int16_t tx0, int16_t tx1, int16_t ty) {
  const int16_t mx = _mm_x();
  const int16_t sy = _mm_y + ty * _mm_scale;
  int16_t run_x = tx0;
  uint16_t run_col = tilemap::color(tilemap::tile_at(tx0 * tilemap::TILE, ty * tilemap::TILE));

  for (int16_t tx = tx0 + 1; tx <= tx1; ++tx) {
    const uint16_t col = tilemap::color(tilemap::tile_at(tx * tilemap::TILE, ty * tilemap::TILE));
    if (col != run_col) {
      display::fill_rect(mx + run_x * _mm_scale, sy, (tx - run_x) * _mm_scale, _mm_scale, run_col);
      run_x = tx;
      run_col = col;
    }
  }
  display::fill_rect(mx + run_x * _mm_scale, sy, (tx1 - run_x + 1) * _mm_scale, _mm_scale, run_col);
}

void panel::_mm_restore_col(int16_t tx, int16_t ty0, int16_t ty1) {
  const int16_t mx = _mm_x() + tx * _mm_scale;
  int16_t run_y = ty0;
  uint16_t run_col = tilemap::color(tilemap::tile_at(tx * tilemap::TILE, ty0 * tilemap::TILE));

  for (int16_t ty = ty0 + 1; ty <= ty1; ++ty) {
    const uint16_t col = tilemap::color(tilemap::tile_at(tx * tilemap::TILE, ty * tilemap::TILE));
    if (col != run_col) {
      display::fill_rect(mx, _mm_y + run_y * _mm_scale, _mm_scale, (ty - run_y) * _mm_scale, run_col);
      run_y = ty;
      run_col = col;
    }
  }
  display::fill_rect(mx, _mm_y + run_y * _mm_scale, _mm_scale, (ty1 - run_y + 1) * _mm_scale, run_col);
}

void panel::_mm_dot(int16_t wx, int16_t wy, uint16_t col) {
  if (_mm_n >= (uint8_t)(2 + sim::MAX_ZOMBIES)) {
    return;
  }
  display::fill_rect(_mm_x() + (wx / tilemap::TILE) * _mm_scale, _mm_y + (wy / tilemap::TILE) * _mm_scale,
                     _mm_scale, _mm_scale, col);
  _mm_px[_mm_n] = wx;
  _mm_py[_mm_n] = wy;
  ++_mm_n;
}

void panel::_mm_frame() {
  // the camera snaps to a whole cell, so the frame always lands on even minimap pixels
  const int16_t cell_w = (int16_t)(display::width() / tilemap::TILE);
  const int16_t cell_h = (int16_t)(render::ARENA_H / tilemap::TILE);
  const int16_t ctx = (int16_t)(render::cam_x() / tilemap::TILE);
  const int16_t cty = (int16_t)(render::cam_y() / tilemap::TILE);

  if (_mm_ctx >= 0) { // put the terrain back under the frame drawn last frame
    _mm_restore_row(_mm_ctx, _mm_ctx + cell_w - 1, _mm_cty);
    _mm_restore_row(_mm_ctx, _mm_ctx + cell_w - 1, _mm_cty + cell_h - 1);
    _mm_restore_col(_mm_ctx, _mm_cty, _mm_cty + cell_h - 1);
    _mm_restore_col(_mm_ctx + cell_w - 1, _mm_cty, _mm_cty + cell_h - 1);
  }

  const int16_t fx = _mm_x() + ctx * _mm_scale;
  const int16_t fy = _mm_y + cty * _mm_scale;
  const int16_t fw = cell_w * _mm_scale;
  const int16_t fh = cell_h * _mm_scale;

  display::fill_rect(fx, fy, fw, 1, colour::yellow);
  display::fill_rect(fx, fy + fh - 1, fw, 1, colour::yellow);
  display::fill_rect(fx, fy, 1, fh, colour::yellow);
  display::fill_rect(fx + fw - 1, fy, 1, fh, colour::yellow);

  _mm_ctx = ctx;
  _mm_cty = cty;
}

void panel::blips() {
  for (uint8_t i = 0; i < _mm_n; ++i) { // restore the terrain under last frame's dots
    const int16_t tx = _mm_px[i] / tilemap::TILE;
    const int16_t ty = _mm_py[i] / tilemap::TILE;
    display::fill_rect(_mm_x() + tx * _mm_scale, _mm_y + ty * _mm_scale, _mm_scale, _mm_scale,
                       tilemap::color(tilemap::tile_at(_mm_px[i], _mm_py[i])));
  }
  _mm_n = 0;

  if (_roll_dx >= 0) { // put the terrain back under last frame's pad marker
    _mm_restore_row(_roll_dx, _roll_dx + 1, _roll_dy);
    _mm_restore_row(_roll_dx, _roll_dx + 1, _roll_dy + 1);
    _roll_dx = _roll_dy = -1;
  }
  if (_roll_tx >= 0 && _roll_ty >= 0 && _roll_tx + 1 < tilemap::COLS &&
      _roll_ty + 1 < tilemap::ROWS) {
    // active roulette pad: solid green 2x2 under the dots (green is free on the
    // minimap: players white/cyan/yellow, zombies red/orange/purple)
    display::fill_rect(_mm_x() + _roll_tx * _mm_scale, _mm_y + _roll_ty * _mm_scale,
                       (int16_t)(2 * _mm_scale), (int16_t)(2 * _mm_scale), colour::green);
    _roll_dx = _roll_tx;
    _roll_dy = _roll_ty;
  }

  _mm_frame();

  const sim::state& v = sim::view();
  for (uint8_t p = 0; p < sim::NUM_PLAYERS; ++p) {
    if (!v.players[p].active || (v.players[p].hp == 0 && !v.players[p].downed)) {
      continue; // inactive, or bled out (dead till the wave): no dot. Downed keeps
    }           // its yellow dot (hp reads 0 there too, so the downed check matters).
    uint16_t col = (p == 0) ? colour::white : colour::cyan;
    if (v.players[p].downed) {
      col = colour::yellow; // body to rescue
    }
    _mm_dot((int16_t)v.players[p].x, (int16_t)v.players[p].y, col);
  }
  for (uint8_t i = 0; i < sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active) {
      uint16_t col = colour::red;
      if (v.zombies[i].kind == sim::actor_kind::runner) {
        col = colour::orange;
      } else if (v.zombies[i].kind == sim::actor_kind::boss) {
        col = colour::purple;
      }
      _mm_dot((int16_t)v.zombies[i].x, (int16_t)v.zombies[i].y, col);
    }
  }
}
