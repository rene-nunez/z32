#pragma once

#include <cstdint>

#include <Render.h>
#include <Sim.h>
#include <Map.h>

// The bottom panel: minimap plus the status text
class Panel {
  public:
    static void init(); // once per run: black panel + minimap terrain
    static void draw(); // per frame: points, HP pips, DMG/SPD/ROF % (focus build)
    static void blips(); // per frame: camera cell frame and the entity dots

    // active roulette pad (top-left tile, -1 = none): game pushes it, blips() paints its minimap marker lime
    static void set_roll(int16_t tx, int16_t ty);

  private:
    static constexpr uint8_t mm_scale = 2;
    static constexpr uint8_t panel_h = 70;
    static constexpr int16_t mm_w = (int16_t)Tilemap::COLS * mm_scale;
    static constexpr int16_t mm_gap = 4;
    static constexpr int16_t mm_y = Render::ARENA_BOTTOM + 5;

    static int16_t mm_px[2 + Sim::MAX_ZOMBIES]; // minimap blips drawn last frame
    static int16_t mm_py[2 + Sim::MAX_ZOMBIES];
    static uint8_t mm_n;
    static int16_t mm_ctx; // camera cell tile whose frame is on the minimap, -1 = none yet
    static int16_t mm_cty;
    static int16_t roll_tx, roll_ty; // active roulette pad, top-left tile (-1 = none)
    static int16_t roll_dx, roll_dy; // pad tiles painted last frame (restored in blips)

    static int16_t mm_x();
    static void pip_row(int16_t y, const char* label, uint8_t lvl, uint8_t max, uint16_t col);
    static void mm_restore_row(int16_t tx0, int16_t tx1, int16_t ty);
    static void mm_restore_col(int16_t tx, int16_t ty0, int16_t ty1);
    static void mm_dot(int16_t wx, int16_t wy, uint16_t col);
    static void mm_frame();
};
