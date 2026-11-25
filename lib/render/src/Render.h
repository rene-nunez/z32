#pragma once

#include <cstdint>

#include <Display.h>
#include <Sim.h>
#include <Sprites.h>

// The arena view: camera, the incremental terrain repaint and the entity boxes. It reads the
// simulation and nothing else, so what it paints is a function of sim state plus geometry
// The panel sits next to it and needs the arena origin and the camera, hence those are public
class Render {
  public:
    // screen layout: hud strip (W/K + gun + role badge, painted by game), arena, bottom panel
    static constexpr uint8_t HUD_H = 10;
    static constexpr uint8_t ARENA_H = 160;
    static constexpr int16_t ARENA_BOTTOM = HUD_H + ARENA_H; // panel starts here
    static constexpr uint8_t SPRITE = 16; // actor art is 16x16, centred on the hitbox
    static constexpr uint8_t BOSS_ART = 32; // the boss is double presence, same 6px core

    static void repaint(); // schedule a full arena repaint from the tilemap
    static void repaint_step(); // paint up to PAINT_CHUNK pending arena rows
    static void update_camera();
    static void set_focus(uint8_t p); // co-op: each board frames its own player
    static uint8_t focus(); // that player: panel/HUD/tags show its build
    static void clear(); // erase the entities through the tilemap colours
    static void draw(); // terrain, entities, tags and the prompt
    static void prompt(const char* msg, uint16_t color = Colour::yellow); // transient strip text

    static int16_t cam_x();
    static int16_t cam_y();

  private:
    static constexpr uint8_t PAINT_CHUNK = 80; // arena rows repainted per frame
    static constexpr int16_t prompt_h = 10; // prompt strip height at the arena bottom
    static constexpr int16_t tag_max_w = 8 * 6; // widest price tag ("DMG 2850"), for erasing

    static int16_t camera_x, camera_y;
    static uint8_t focus_player; // player the camera follows (host/solo 0, client 1)
    static int16_t paint_y; // next arena row to repaint, ARENA_H when idle
    static const char* prompt_text; // strip text for this frame, null = none (set by game)
    static uint16_t prompt_col; // its colour (default yellow, green buys/THX, white info, red danger...)

    static int16_t cell_cam(int16_t p, int16_t step, int16_t max_cam);
    static void art_bbox(const uint16_t* img, uint8_t art, bool flip, uint8_t& x0, uint8_t& y0, uint8_t& w, uint8_t& h); // opaque art bbox, cached
    static void erase_box(int16_t wx, int16_t wy, int16_t w, int16_t h); // one-burst terrain push
    static int16_t sprite_tl(int16_t e, uint8_t hitbox, uint8_t art); // art centred on hitbox
    static void draw_actor(int16_t ex, int16_t ey, uint8_t hitbox, uint8_t art, const uint16_t* img, bool flip); // centred sprite, opt. mirror
    // facing (sim 8-wind) -> stored art + mirror: players N,S,E,NE,SE; zombies N,S,E
    static const uint16_t* player_img(uint8_t p, uint8_t d, bool& flip);
    static const uint16_t* zombie_img(Sim::actor_kind k, uint8_t d, bool& flip);
    static void frame_box(int16_t sx, int16_t sy, uint16_t col); // 1px frame, downed rescue
    static void fill_world_run(int16_t wx, int16_t sy, int16_t w, uint16_t col);
    static void erase_world_rect(int16_t wx, int16_t wy, uint8_t size);
    static void erase_world_area(int16_t wx, int16_t wy, int16_t w, int16_t h);
    static void fill_world_box(int16_t wx, int16_t wy, uint8_t size, uint16_t col);
    static void shop_labels(bool erase); // world-anchored price tags over the machines
    static bool tags_stale(); // clear(): erase tags only on cut, moved cam, new price, rebuild
};
