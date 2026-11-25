#pragma once

// Arena view: camera, terrain repaint and entity boxes

#include <cstdint>

#include <Display.h>
#include <Sim.h>
#include <Sprites.h>

class Render {
  public:
    static constexpr uint8_t HUD_H = 10;
    static constexpr uint8_t ARENA_H = 160;
    static constexpr int16_t ARENA_BOTTOM = HUD_H + ARENA_H;
    static constexpr uint8_t SPRITE = 16;
    static constexpr uint8_t BOSS_ART = 32;

    static void repaint();
    static void repaint_step();
    static void update_camera();
    static void set_focus(uint8_t p);
    static uint8_t focus();
    static void clear();
    static void draw();
    static void prompt(const char* msg, uint16_t color = Colour::yellow);

    static int16_t cam_x();
    static int16_t cam_y();

  private:
    static constexpr uint8_t PAINT_CHUNK = 80;
    static constexpr int16_t prompt_h = 10;
    static constexpr int16_t tag_max_w = 8 * 6;

    static int16_t camera_x, camera_y;
    static uint8_t focus_player;
    static int16_t paint_y;
    static const char* prompt_text;
    static uint16_t prompt_col;

    static int16_t cell_cam(int16_t p, int16_t step, int16_t max_cam);
    static void art_bbox(const uint16_t* img, uint8_t art, bool flip, uint8_t& x0, uint8_t& y0, uint8_t& w, uint8_t& h);
    static void erase_box(int16_t wx, int16_t wy, int16_t w, int16_t h);
    static int16_t sprite_tl(int16_t e, uint8_t hitbox, uint8_t art);
    static void draw_actor(int16_t ex, int16_t ey, uint8_t hitbox, uint8_t art, const uint16_t* img, bool flip);
    static const uint16_t* player_img(uint8_t p, uint8_t d, bool& flip);
    static const uint16_t* zombie_img(Sim::actor_kind k, uint8_t d, bool& flip);
    static void frame_box(int16_t sx, int16_t sy, uint16_t col);
    static void fill_world_run(int16_t wx, int16_t sy, int16_t w, uint16_t col);
    static void erase_world_rect(int16_t wx, int16_t wy, uint8_t size);
    static void erase_world_area(int16_t wx, int16_t wy, int16_t w, int16_t h);
    static void fill_world_box(int16_t wx, int16_t wy, uint8_t size, uint16_t col);
    static void shop_labels(bool erase);
    static bool tags_stale();
};
