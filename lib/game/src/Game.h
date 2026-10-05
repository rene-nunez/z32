#pragma once

#include <cstdint>

#include <Handler.h>
#include <Input.h>
#include <NetState.h>
#include <Screens.h>

#include <Sim.h>

class game {
  public:
    static bool begin(uint8_t role);
    static void update();

  private:
    static constexpr uint32_t _frame_ms = 33; // ~30 fps
    static constexpr int16_t _shop_r = 28;    // INTERACT reach, px from a machine centre
    static constexpr uint32_t _intro_ms = 2500; // logo and team screens duration, FIRE skips
    static constexpr uint32_t _wait_ms = 10000; // multiplayer peer wait, then back to mode
    static constexpr uint32_t _wait_hb_ms = 200; // heartbeat pace while waiting
    static constexpr uint32_t _in_stale_ms = 300; // peer input older than this goes neutral
    static constexpr uint32_t _cli_quiet_ms = 3000; // no snapshot for this long: back to menu
    static constexpr int16_t _revive_r = 28; // partner-lift reach, px centre to centre

    static uint32_t _last_frame_ms;
    static uint32_t _intro_ms0; // millis() at logo/team entry, anchors the intro timers

    static uint8_t _sel;
    static int8_t _nav_dir;

    static handler _handler;
    static uint32_t _tick;
    static uint32_t _peer_tick;

    // F6 co-op net state. Host simulates both players and broadcasts snapshots;
    // the client sends inputs and applies snapshots. Solo never touches the air.
    static bool _net_multi;        // this run is co-op
    static uint32_t _wait_since;   // waiting entry, millis()
    static uint32_t _wait_last_hb; // last waiting heartbeat, millis()
    static bool _peer_seen;        // peer showed up while waiting
    static uint16_t _seq_out;      // snapshot/input sequence, host and client each own theirs
    static uint8_t _in_seq;        // last peer input seq applied (host)
    static uint8_t _in_buttons;    // latest peer button levels (host)
    static uint8_t _in_prev;       // previous peer buttons, for edges (host)
    static float _in_jx, _in_jy;   // latest peer axes (host)
    static uint32_t _in_last_ms;   // last peer input rx time (host, stale check)
    static volatile bool _rx_ready; // snapshot waiting to apply (client)
    static bool _cli_mirror; // client is painting the host-owned pause (arena chrome is covered)
    static bool _cli_was_down0, _cli_was_down1; // downed flags last client frame, for the rise edge
    static bool _cli_was_dead0, _cli_was_dead1; // hp==0 && !downed last client frame, BACK edge
    static bool _cli_shouted0, _cli_shouted1; // SAVE ME sent this down (client), HELP hides after
    static uint16_t _cli_wave; // wave last client frame, restarts resync silently
    static net::game_state_msg _rx_state; // snapshot buffer (client)
    static net::game_state_msg _tx_state; // snapshot scratch (host)
    static uint32_t _cli_last_rx; // last snapshot applied (client, quiet check)

    static int16_t _shop_hx, _shop_hy; // heal vending centre, world px (-1 = missing)
    static int16_t _shop_dx, _shop_dy; // damage vending centre
    static int16_t _shop_sx, _shop_sy; // speed vending centre
    static int16_t _shop_cx, _shop_cy; // rapid vending centre
    static constexpr uint8_t MAX_PADS = 6; // roulette pad slots, row-major scan order
    static int16_t _shop_rx[MAX_PADS], _shop_ry[MAX_PADS];
    static uint8_t _shop_rn; // pads found (4 on the shipped map)
    static uint8_t _roulette_active(); // hash(wave/3) % _shop_rn, synced via sim wave
    static uint8_t _roulette_active_at(uint16_t w, uint8_t n); // active pad at wave w
    static void _push_roll_marker(); // active pad tiles to the panel minimap
    static bool _roulette_moved(); // the pad really relocated this wave (banner gate)
    static bool _near_inactive_roulette(uint8_t p); // standing on a dead pad
    static char _hint_buf[28];         // transient result text ("NEED 100 PTS", "NEW GUN: MP9")
    static uint16_t _hint_col;       // its strip colour (green buys, white info...)
    static uint32_t _hint_until;       // result visible while millis() < this
    static char _chat_buf[28];       // last SAVE ME text (rx + local echo, "P2: SAVE ME!")
    static uint16_t _chat_col;       // speaker colour (P1 green / P2 steel-blue)
    static uint32_t _chat_until;     // SAVE ME visible while millis() < this (2s)
    static uint8_t _chat_seq;        // SAVE ME edge counter
    static bool _chat_pip;           // SAVE ME rx flag, buzzed from the frame loop
    static bool _was_down0, _was_down1; // downed flags last host frame, for the bleed-out edge
    static bool _was_dead0, _was_dead1; // hp==0 && !downed last host frame, for the BACK edge
    static bool _shouted0, _shouted1; // SAVE ME sent this down (host), HELP hides after
    static bool _p2_interact;          // player 2 INTERACT edge, set from net, consumed in shop
    static bool _p2_pause_edge;        // player 2 PAUSE edge, set from net, consumed in game

    static void _on_heartbeat(const uint8_t* data, size_t len);
    static void _on_state(const uint8_t* data, size_t len); // snapshot rx (client)
    static void _on_input(const uint8_t* data, size_t len); // input rx (host)
    static void _on_chat(const uint8_t* data, size_t len); // SAVE ME rx (both)
    static void _send_chat(uint8_t from); // SAVE ME tx, edge only (multi only)
    static bool _urgent_callout(uint8_t me); // voluntary SAVE ME shout, above reloading
    static bool _help_hint(uint8_t me); // PRESS INT FOR HELP until first shout, above reloading
    static bool _reload_prompt(uint8_t me); // own mag swap FYI ("RELOADING..."), above shop

    static int8_t _nav_edge();
    static void _nav_step(); // nav edge + wrap, using the screen's own item count
    static void _sleep();
    static screens::id _scr;

    static void _start_game(bool multi);
    static void _enter_menu();
    static void _enter_game_over();
    static void _broadcast(); // snapshot the sim and send it (host, multi only)

    static void _scan_shops();       // cache the 2x2 machine centres, once per run
    static uint8_t _shop_at(uint8_t p); // nearest machine to player p (0 = none)
    static void _shop_prompt(uint8_t shop, const char* who, uint8_t p); // prompt strip text only
    static bool _revive_near(uint8_t p); // downed partner within lift reach of p
    static bool _revive_update(uint32_t now); // INTERACT lifts, true = edge consumed
    static void _shop_update(uint32_t now); // INTERACT buys + panel prompt, after sim::step
    static void _fire_buzz(); // one jingle per sim::last_event, after step()+shop

    static void _update_menu();
    static void _update_mode();
    static void _update_points();
    static void _update_playing();
    static void _update_playing_host();   // host sim + broadcast (solo: sim only)
    static void _update_playing_client(); // input tx + snapshot apply + draw
    static void _send_input();            // ship sticks/buttons (client, every frame)
    static void _mirror_pause(); // host-owned pause chrome on the client (screen-driven)
    static void _draw_hud();     // 10px strip: W/K left, gun centre, role badge right
    static uint16_t _hud_wave, _hud_kills; // last painted HUD (cache: skip if same)
    static uint8_t _hud_role; // role badge cache (stays one byte)
    static sim::weapon _hud_gun;
    static uint8_t _hud_ammo; // mag count cache (repaints on every shot)
    static uint8_t _hud_shown; // displayed player: focus, or the partner while spectating
    static bool _hud_first; // force full wipe+repaint (menu chrome covered the strip)
    static bool _boss_alive();   // any active boss in the sim view (both boards mirror it)
    static void _update_pause();
    static void _update_game_over();
    static void _update_logo();
    static void _update_team();
    static void _update_waiting();
};
