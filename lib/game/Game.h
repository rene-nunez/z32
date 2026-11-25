#pragma once

// Game state machine, screens and shared static state

#include <cstdint>

#include <Handler.h>
#include <Input.h>
#include <Net_state.h>
#include <Screens.h>

#include <Sim.h>

class Game {
  public:
    static bool begin(uint8_t role);
    static void update();

  private:
    static constexpr uint32_t frame_ms = 33;
    static constexpr int16_t shop_r = 28;
    static constexpr uint32_t intro_ms = 2500;
    static constexpr uint32_t wait_ms = 10000;
    static constexpr uint32_t wait_hb_ms = 200;
    static constexpr uint32_t in_stale_ms = 300;
    static constexpr uint32_t cli_quiet_ms = 3000;
    static constexpr int16_t revive_r = 28;
    static constexpr uint16_t mate_p2col = 0x54DA;

    static uint32_t last_frame_ms;
    static uint32_t intro_ms0;

    static uint8_t sel;
    static int8_t nav_dir;

    static Handler handler;
    static uint32_t tick;

    static bool net_multi;
    static uint32_t wait_since;
    static uint32_t wait_last_hb;
    static bool peer_seen;
    static uint16_t seq_out;
    static uint8_t in_buttons;
    static uint8_t in_prev;
    static float in_jx, in_jy;
    static uint32_t in_last_ms;
    static volatile bool rx_ready;
    static bool cli_mirror;
    static bool cli_pause_logged;
    static bool cli_was_down0, cli_was_down1;
    static bool cli_was_dead0, cli_was_dead1;
    static bool cli_shouted0, cli_shouted1;
    static uint16_t cli_wave;
    static Net::game_state_msg rx_state;
    static Net::game_state_msg tx_state;
    static uint32_t cli_last_rx;

    static int16_t shop_hx, shop_hy;
    static int16_t shop_dx, shop_dy;
    static int16_t shop_sx, shop_sy;
    static int16_t shop_cx, shop_cy;
    static constexpr uint8_t MAX_PADS = 6;
    static int16_t shop_rx[MAX_PADS], shop_ry[MAX_PADS];
    static uint8_t shop_rn;
    static uint8_t roulette_active();
    static uint8_t roulette_active_at(uint16_t w, uint8_t n);
    static void push_roll_marker();
    static bool roulette_moved();
    static bool near_inactive_roulette(uint8_t p);
    static char hint_buf[28];
    static uint16_t hint_col;
    static uint32_t hint_until;
    static char chat_buf[28];
    static uint16_t chat_col;
    static uint32_t chat_until;
    static uint8_t chat_seq;
    static bool chat_pip;
    static bool was_down0, was_down1;
    static bool was_dead0, was_dead1;
    static bool shouted0, shouted1;
    static bool p2_interact;
    static bool p2_pause_edge;

    static void on_heartbeat(const uint8_t* data, size_t len);
    static void on_state(const uint8_t* data, size_t len);
    static void on_input(const uint8_t* data, size_t len);
    static void on_chat(const uint8_t* data, size_t len);
    static void send_chat(uint8_t from);
    static bool urgent_callout(uint8_t me);
    static bool help_hint(uint8_t me);
    static bool reload_prompt(uint8_t me);

    static int8_t nav_edge();
    static void nav_step();
    static void sleep();
    static Screens::id scr;

    static void start_game(bool multi);
    static void enter_menu();
    static void enter_game_over();
    static void broadcast();

    static void scan_shops();
    static uint8_t shop_at(uint8_t p);
    static void shop_prompt(uint8_t shop, const char* who, uint8_t p);
    static bool revive_near(uint8_t p);
    static bool revive_update(uint32_t now);
    static void shop_update(uint32_t now);
    static void fire_buzz();

    static void update_menu();
    static void update_mode();
    static void update_scores();
    static void update_playing();
    static void update_playing_host();
    static void update_playing_client();
    static void send_input();
    static void mirror_pause();
    static void draw_hud();
    static uint16_t hud_wave, hud_kills;
    static uint8_t hud_role;
    static Sim::weapon hud_gun;
    static uint8_t hud_ammo;
    static uint8_t hud_shown;
    static bool hud_first;
    static bool boss_alive();
    static void update_pause();
    static void update_game_over();
    static void update_logo();
    static void update_team();
    static void update_waiting();
};
