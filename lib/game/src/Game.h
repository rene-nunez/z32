#pragma once

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
    static constexpr uint32_t frame_ms = 33; // ~30 fps
    static constexpr int16_t shop_r = 28;    // INTERACT reach, px from a machine centre
    static constexpr uint32_t intro_ms = 2500; // logo and team Screens duration, FIRE skips
    static constexpr uint32_t wait_ms = 10000; // multiplayer peer wait, then back to mode
    static constexpr uint32_t wait_hb_ms = 200; // heartbeat pace while waiting
    static constexpr uint32_t in_stale_ms = 300; // peer input older than this goes neutral
    static constexpr uint32_t cli_quiet_ms = 3000; // no snapshot for this long: back to menu
    static constexpr int16_t revive_r = 28; // partner-lift reach, px centre to centre
    static constexpr uint16_t mate_p2col = 0x54DA; // P2 steel-blue, brightened to read at 8px

    static uint32_t last_frame_ms;
    static uint32_t intro_ms0; // millis() at logo/team entry, anchors the intro timers

    static uint8_t sel;
    static int8_t nav_dir;

    static Handler handler;
    static uint32_t tick;

    // F6 co-op net state. Host simulates both players and broadcasts snapshots;
    // the client sends inputs and applies snapshots. Solo never touches the air
    static bool net_multi;        // this run is co-op
    static uint32_t wait_since;   // waiting entry, millis()
    static uint32_t wait_last_hb; // last waiting heartbeat, millis()
    static bool peer_seen;        // peer showed up while waiting
    static uint16_t seq_out;      // snapshot/Input sequence, host and client each own theirs
    static uint8_t in_buttons;    // latest peer button levels (host)
    static uint8_t in_prev;       // previous peer buttons, for edges (host)
    static float in_jx, in_jy;   // latest peer axes (host)
    static uint32_t in_last_ms;   // last peer input rx time (host, stale check)
    static volatile bool rx_ready; // snapshot waiting to apply (client)
    static bool cli_mirror; // client is painting the host-owned pause (arena chrome is covered)
    static bool cli_pause_logged; // pause-from-peer line already printed for this pause
    static bool cli_was_down0, cli_was_down1; // downed flags last client frame, for the rise edge
    static bool cli_was_dead0, cli_was_dead1; // hp==0 && !downed last client frame, BACK edge
    static bool cli_shouted0, cli_shouted1; // SAVE ME sent this down (client), HELP hides after
    static uint16_t cli_wave; // wave last client frame, restarts resync silently
    static Net::game_state_msg rx_state; // snapshot buffer (client)
    static Net::game_state_msg tx_state; // snapshot scratch (host)
    static uint32_t cli_last_rx; // last snapshot applied (client, quiet check)

    static int16_t shop_hx, shop_hy; // heal vending centre, world px (-1 = missing)
    static int16_t shop_dx, shop_dy; // damage vending centre
    static int16_t shop_sx, shop_sy; // speed vending centre
    static int16_t shop_cx, shop_cy; // rapid vending centre
    static constexpr uint8_t MAX_PADS = 6; // roulette pad slots, row-major scan order
    static int16_t shop_rx[MAX_PADS], shop_ry[MAX_PADS];
    static uint8_t shop_rn; // pads found (4 on the shipped map)
    static uint8_t roulette_active(); // hash(wave/3) % shop_rn, synced via sim wave
    static uint8_t roulette_active_at(uint16_t w, uint8_t n); // active pad at wave w
    static void push_roll_marker(); // active pad tiles to the panel minimap
    static bool roulette_moved(); // the pad really relocated this wave (banner gate)
    static bool near_inactive_roulette(uint8_t p); // standing on a dead pad
    static char hint_buf[28];         // transient result text ("NEED 100 PTS", "NEW GUN: MP9")
    static uint16_t hint_col;       // its strip colour (green buys, white info...)
    static uint32_t hint_until;       // result visible while millis() < this
    static char chat_buf[28];       // last SAVE ME text (rx + local echo, "P2: SAVE ME!")
    static uint16_t chat_col;       // speaker colour (P1 green / P2 steel-blue)
    static uint32_t chat_until;     // SAVE ME visible while millis() < this (2s)
    static uint8_t chat_seq;        // SAVE ME edge counter
    static bool chat_pip;           // SAVE ME rx flag, buzzed from the frame loop
    static bool was_down0, was_down1; // downed flags last host frame, for the bleed-out edge
    static bool was_dead0, was_dead1; // hp==0 && !downed last host frame, for the BACK edge
    static bool shouted0, shouted1; // SAVE ME sent this down (host), HELP hides after
    static bool p2_interact;          // player 2 INTERACT edge, set from net, consumed in shop
    static bool p2_pause_edge;        // player 2 PAUSE edge, set from net, consumed in game

    static void on_heartbeat(const uint8_t* data, size_t len);
    static void on_state(const uint8_t* data, size_t len); // snapshot rx (client)
    static void on_input(const uint8_t* data, size_t len); // input rx (host)
    static void on_chat(const uint8_t* data, size_t len); // SAVE ME rx (both)
    static void send_chat(uint8_t from); // SAVE ME tx, edge only (multi only)
    static bool urgent_callout(uint8_t me); // voluntary SAVE ME shout, above reloading
    static bool help_hint(uint8_t me); // PRESS INT FOR HELP until first shout, above reloading
    static bool reload_prompt(uint8_t me); // own mag swap FYI ("RELOADING..."), above shop

    static int8_t nav_edge();
    static void nav_step(); // nav edge + wrap, using the screen's own item count
    static void sleep();
    static Screens::id scr;

    static void start_game(bool multi);
    static void enter_menu();
    static void enter_game_over();
    static void broadcast(); // snapshot the sim and send it (host, multi only)

    static void scan_shops();       // cache the 2x2 machine centres, once per run
    static uint8_t shop_at(uint8_t p); // nearest machine to player p (0 = none)
    static void shop_prompt(uint8_t shop, const char* who, uint8_t p); // prompt strip text only
    static bool revive_near(uint8_t p); // downed partner within lift reach of p
    static bool revive_update(uint32_t now); // INTERACT lifts, true = edge consumed
    static void shop_update(uint32_t now); // INTERACT buys + Panel prompt, after Sim::step
    static void fire_buzz(); // one jingle per Sim::last_event, after step()+shop

    static void update_menu();
    static void update_mode();
    static void update_scores();
    static void update_playing();
    static void update_playing_host();   // host sim + broadcast (solo: sim only)
    static void update_playing_client(); // input tx + snapshot apply + draw
    static void send_input();            // ship sticks/buttons (client, every frame)
    static void mirror_pause(); // host-owned pause chrome on the client (screen-driven)
    static void draw_hud();     // 10px strip: W/K left, gun centre, role badge right
    static uint16_t hud_wave, hud_kills; // last painted HUD (cache: skip if same)
    static uint8_t hud_role; // role badge cache (stays one byte)
    static Sim::weapon hud_gun;
    static uint8_t hud_ammo; // mag count cache (repaints on every shot)
    static uint8_t hud_shown; // displayed player: focus, or the partner while spectating
    static bool hud_first; // force full wipe+repaint (menu chrome covered the strip)
    static bool boss_alive();   // any active boss in the sim view (both boards mirror it)
    static void update_pause();
    static void update_game_over();
    static void update_logo();
    static void update_team();
    static void update_waiting();
};
