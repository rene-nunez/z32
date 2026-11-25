#include <Arduino.h>
#include <cmath>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <Buzz.h>
#include <Display.h>
#include <Map.h>
#include <Panel.h>
#include <Render.h>
#include <Screens.h>
#include <Scores.h>
#include <Sim.h>

#include "Game.h"
#include "pins.h"

// game net: ESP-NOW handlers, snapshot broadcast, client input shipping and the
// per-board playing frames. Host simulates, client mirrors; solo stays silent

void Game::on_heartbeat(const uint8_t* data, size_t len) {
  if (len < sizeof(heartbeat_msg)) {
    return;
  }
  (void)data; // presence is the signal: the host counts heartbeats, not their values
  // the host starts a co-op run off heartbeats; the client waits for a snapshot
  // (live map) instead, so heartbeats never flip a waiting client into playing
  if (handler.role() == ROLE_HOST) {
    peer_seen = true; // any peer heartbeat counts while waiting (consumed there)
  }
}

void Game::on_state(const uint8_t* data, size_t len) {
  if (handler.role() != ROLE_CLIENT) {
    return; // host never applies snapshots
  }
  if (len < sizeof(Net::game_state_msg)) {
    return;
  }
  memcpy(&rx_state, data, sizeof(Net::game_state_msg));
  rx_ready = true;
  peer_seen = true; // snapshots also join a waiting client, enabling late join
}

void Game::on_input(const uint8_t* data, size_t len) {
  if (handler.role() != ROLE_HOST) {
    return; // client never applies inputs
  }
  if (len < sizeof(Net::player_input_msg)) {
    return;
  }
  Net::player_input_msg in;
  memcpy(&in, data, sizeof(in));
  in_jx = Net::uqaxis(in.jx);
  in_jy = Net::uqaxis(in.jy);
  in_buttons = in.buttons;
  in_last_ms = millis();
}

void Game::on_chat(const uint8_t* data, size_t len) {
  if (len < sizeof(Net::chat_msg)) {
    return;
  }
  Net::chat_msg m;
  memcpy(&m, data, sizeof(m));
  if (m.from > 1 || m.id != (uint8_t)Net::chat_id::come) {
    return; // SAVE ME rides the come wire id; THX derives locally, nothing else travels
  }
  snprintf(chat_buf, sizeof(chat_buf), "P%u: SAVE ME!", (unsigned)(m.from + 1u));
  chat_col = (m.from == 0) ? Colour::green : mate_p2col;
  chat_until = millis() + 2000;
  chat_pip = true; // buzzed from the frame loop, never from the rx task
}

// downed INTERACT shout: still the come wire id (no protocol change), now reads SAVE ME
// The sender also sees its own shout as a local echo (visual only, never buzzed);
// only the peer's rx path sets chat_pip, so the hurt pip stays remote
void Game::send_chat(uint8_t from) {
  if (!net_multi) {
    return; // solo: no peer, and send() with no peer sprays delivery-failed
  }
  Net::chat_msg m;
  m.from = from;
  m.id = (uint8_t)Net::chat_id::come;
  m.seq = chat_seq++;
  handler.send(&m, sizeof(m)); // triple: 4B each, one usually lands
  handler.send(&m, sizeof(m));
  handler.send(&m, sizeof(m));
  snprintf(chat_buf, sizeof(chat_buf), "P%u: SAVE ME!", (unsigned)(from + 1u));
  chat_col = (from == 0) ? Colour::green : mate_p2col;
  chat_until = millis() + 2000;
  // first shout this down: the HELP hint below retires (each board reads its own
  // pair, so set both; the unread one is harmless)
  if (from == 0) {
    shouted0 = true;
    cli_shouted0 = true;
  } else {
    shouted1 = true;
    cli_shouted1 = true;
  }
}

void Game::broadcast() {
  tx_state.type = Net::TYPE_STATE;
  tx_state.seq = seq_out++;
  Sim::snapshot(tx_state);
  // the menu mirror rides the snapshot: the host owns pause/over chrome, the client paints it
  if (scr == Screens::id::pause) {
    tx_state.screen = Net::SCREEN_PAUSE;
  } else if (scr == Screens::id::game_over) {
    tx_state.screen = Net::SCREEN_OVER;
  } else {
    tx_state.screen = Net::SCREEN_PLAYING;
  }
  tx_state.sel = sel;
  handler.send(&tx_state, sizeof(tx_state));
}

void Game::update_playing_host() {
  // fold the latest peer input into player 2 (edges from levels, neutral when stale)
  const uint32_t now = millis();
  Sim::ctl c = {};
  if (net_multi && now - in_last_ms < in_stale_ms) {
    c.jx = in_jx;
    c.jy = in_jy;
    // P2 fire mirrors the local rule: level for autos (MP9/AR-15), edge for the rest
    const bool p2_lvl = (in_buttons & Net::fire_bit) != 0;
    c.fire = Sim::auto_fire(Sim::view().guns[1]) ? p2_lvl : (p2_lvl && !(in_prev & Net::fire_bit));
    p2_interact = (in_buttons & Net::interact_bit) && !(in_prev & Net::interact_bit);
    p2_pause_edge = (in_buttons & Net::pause_bit) && !(in_prev & Net::pause_bit);
    c.reload = (in_buttons & Net::reload_bit) && !(in_prev & Net::reload_bit);
    in_prev = in_buttons;
  } else {
    in_prev = in_buttons; // stale: hold levels so the next packet re-edges cleanly
  }
  c.pause = false; // pause travels via p2_pause_edge, sim never sees it
  Sim::set_p2(c);

  if (Input::pause_pressed() || p2_pause_edge) {
    if (p2_pause_edge && !Input::pause_pressed()) {
      Serial.println("[net] pause from peer");
    }
    p2_pause_edge = false;
    scr = Screens::id::pause;
    sel = 0;
    return;
  }
  p2_pause_edge = false;

  // the order matters: the clear-before-sim is what erases entities that die mid-frame
  Render::clear();
  if (!Sim::step(now)) {
    Buzz::play(Buzz::jingle::over); // death jingle, then the screen change below
    enter_game_over(); // sim reports the death, the screen change belongs here
    if (net_multi) {
      broadcast(); // the client mirrors game over off the over event
    }
    return;
  }
  // pre-revive downed edges: the wave banner below needs them (held-on rises),
  // but revive_update advances was_down first
  const bool was_d0 = was_down0;
  const bool was_d1 = was_down1;
  if (revive_update(now)) {
    Render::prompt(hint_buf, hint_col); // lift result now, shop waits a frame
  } else {
    shop_update(now); // INTERACT buys + prompt, before the panel paints it
  }
  if (Sim::view().last_event == Sim::event::wave) {
    if (boss_alive()) {
      // boss waves announce over the proximity prompt: buys still win, proximity waits
      snprintf(hint_buf, sizeof(hint_buf), "BOSS WAVE!");
      hint_col = Colour::purple;
      hint_until = now + 2000;
      Render::prompt(hint_buf, hint_col);
    } else if (roulette_moved() && now >= hint_until) {
      // the wheel really relocated: announce it over the proximity prompt once
      snprintf(hint_buf, sizeof(hint_buf), "ROLL RELOCATED!");
      hint_col = Colour::yellow; // explicit: shop_update repaints the buf with this
      hint_until = now + 2000;
      Render::prompt(hint_buf);
    } else if (now >= hint_until) {
      // wave rejoins, least important of the three: only when the strip is
      // free (fresher hints already won above). Bled-out rejoins and downed
      // players that held on till the break both announce; a same-frame lift
      // already overwrote the wave event (THX wins), so this only sees rises
      // Ties name the bled-out first: the bigger news wins the single strip
      const Sim::state& wv = Sim::view();
      const bool d0 = wv.players[0].active && wv.players[0].downed;
      const bool d1 = net_multi && wv.players[1].active && wv.players[1].downed;
      const bool p0_back = was_dead0 && wv.players[0].hp > 0;
      const bool p1_back = was_dead1 && wv.players[1].hp > 0;
      const bool p0_held = was_d0 && !d0 && wv.players[0].hp > 0;
      const bool p1_held = was_d1 && !d1 && wv.players[1].hp > 0;
      if (p0_back || p1_back || p0_held || p1_held) {
        const bool p1 = p0_back || (!p1_back && p0_held);
        snprintf(hint_buf, sizeof(hint_buf), p1 ? "P1: I'M BACK!" : "P2: I'M BACK!");
        hint_col = p1 ? Colour::green : mate_p2col; // the returner's Colour
        hint_until = now + 1500;
        Render::prompt(hint_buf, hint_col);
      }
    }
  }
  {
    // was_dead advances after the banners so they still see the previous frame
    const Sim::state& wv = Sim::view();
    const bool d0 = wv.players[0].active && wv.players[0].downed;
    const bool d1 = net_multi && wv.players[1].active && wv.players[1].downed;
    was_dead0 = wv.players[0].active && wv.players[0].hp == 0 && !d0;
    was_dead1 = net_multi && wv.players[1].active && wv.players[1].hp == 0 && !d1;
  }
  fire_buzz(); // jingle for the frame's event (revive/buys already overwrote shots)
  push_roll_marker(); // active pad to the minimap, moves on epoch change
  Render::update_camera();
  Render::draw();
  Panel::draw();
  Panel::blips();
  draw_hud(); // wave/kills + gun + role, cached (repaints only on change)
  if (net_multi) {
    broadcast(); // ~30Hz state to the client, right after the frame simmed
  }
}

void Game::update_playing_client() {
  send_input();

  // route off the last snapshot before touching the frame: while the host-owned menu is
  // up, clear() would spray terrain erases over it, so mirror frames skip clear/draw
  if (rx_state.screen == Net::SCREEN_PAUSE) {
    if (!cli_pause_logged) {
      cli_pause_logged = true;
      Serial.println("[net] pause from peer");
    }
    mirror_pause(); // scr stays playing so update() keeps routing here
    return;
  }
  cli_pause_logged = false; // host resumed: next remote pause logs again
  if (cli_mirror) {
    // first playing frame after the menu: its chrome covered the arena, so rebuild it
    // exactly like the host resume path (progressive terrain over the next 2 frames)
    cli_mirror = false;
    Screens::invalidate();
    Panel::init(); hud_first = true;         // the pause menu covered the panel and the minimap
    Render::repaint();     // clear leftover pause menu
  }

  // erase at the old frame, then step to the new one: same order as the host
  Render::clear();
  if (rx_ready) {
    Sim::apply_snapshot(rx_state);
    rx_ready = false;
    cli_last_rx = millis();
  }
  if (millis() - cli_last_rx >= cli_quiet_ms) {
    Buzz::play(Buzz::jingle::denied); // host went away (menu/sleep): drop to menu
    Serial.println("[net] peer quiet 3000ms, back to menu");
    net_multi = false;
    enter_menu();
    return;
  }
  if (Sim::view().last_event == Sim::event::over) {
    enter_game_over(); // host declared it, we mirror and store it on our own card
    return;
  }
  const Sim::state& cv = Sim::view();
  const bool c1_down = cv.players[0].active && cv.players[0].downed;
  const bool c2_down = cv.players[1].active && cv.players[1].downed;
  // edge news: the hint text never travels in the snapshot, so the client
  // derives lift/death edges itself. A wave respawn also moves bodies, but that
  // frame carries the wave event or a wave-number rise (BACK/banner wins below),
  // and a host restart drops the wave, so only lone edges land here. Priority
  // per frame: lift result, then the bleed-out; ties name the local body. Falls
  // stay silent (except arming HELP below, which shows until the first shout)
  bool edge_hint = false;
  // pre-edge downed flags: the wave banner below needs them (held-on rises),
  // but the chain advances cli_was_down first
  const bool was_c1_down = cli_was_down0;
  const bool was_c2_down = cli_was_down1;
  // the wave event lives a single host frame: if that snapshot is skipped (two
  // arrivals between client frames), the next one still carries the higher wave
  // number, so the banner below only slips a frame instead of being lost. A rise
  // with a stamped event (lift/buy overwrote wave on the host) is not a wave
  // frame: the host skipped its banners too, and the edges below still run
  const bool wave_rose = (cv.wave > cli_wave);
  const bool wave_frame = (cv.last_event == Sim::event::wave) ||
                          (wave_rose && cv.last_event == Sim::event::none);
  if (!cli_was_down0 && c1_down) {
    cli_shouted0 = false; // fall edge: HELP shows again until the first shout
  }
  if (!cli_was_down1 && c2_down) {
    cli_shouted1 = false;
  }
  if (cv.wave < cli_wave) {
    cli_was_down0 = c1_down; // host restarted: resync, no announcement
    cli_was_down1 = c2_down;
  } else if (!wave_frame) {
    // hp gate: bleeding out also clears downed (hp stays 0, dead till the wave),
    // only a real lift comes back with hp. Without it the death reads as a revive
    // Bleed-outs announce once (2s); the strip frees after
    const bool p1_rose = cli_was_down0 && !c1_down && cv.players[0].hp > 0;
    const bool p2_rose = cli_was_down1 && !c2_down && cv.players[1].hp > 0;
    const bool p1_died = cli_was_down0 && !c1_down && cv.players[0].hp == 0;
    const bool p2_died = cli_was_down1 && !c2_down && cv.players[1].hp == 0;
    const uint32_t now_rx = millis();
    if (p2_rose || p1_rose) {
      snprintf(hint_buf, sizeof(hint_buf), p2_rose ? "P2: THX!" : "P1: THX!");
      hint_col = p2_rose ? mate_p2col : Colour::green; // the risen one's Colour
      hint_until = now_rx + 1500;
      edge_hint = true; // skip proximity below: shop_prompt reuses hint_buf as scratch
    } else if (p2_died || p1_died) {
      snprintf(hint_buf, sizeof(hint_buf), p2_died ? "P2: I'M OUT!" : "P1: I'M OUT!");
      hint_col = p2_died ? mate_p2col : Colour::green; // the dead one's Colour
      hint_until = now_rx + 2000;
      edge_hint = true;
    }
    cli_was_down0 = c1_down;
    cli_was_down1 = c2_down;
    // cli_was_dead advances after the banners below, like the host pair
  } else {
    cli_was_down0 = c1_down;
    cli_was_down1 = c2_down;
  }
  cli_wave = cv.wave;
  // downed P2 mashing INTERACT with nothing actionable nearby screams SAVE ME
  // (near a machine the host-side buy still wins, like a standing player)
  const uint8_t cshop = shop_at(1);
  if (cv.players[1].downed && cv.players[0].hp > 0 && Input::interact_pressed() &&
      !revive_near(0) && cshop == 0) {
    send_chat(1);
  }
  if (edge_hint) {
    Render::prompt(hint_buf, hint_col); // edge news wins over proximity
  } else if (revive_near(1)) {
    Render::prompt("PRESS INT TO REVIVE"); // INT verb stays yellow
  } else if (urgent_callout(1)) {
    ; // voluntary SAVE ME over the reload nag
  } else if (help_hint(1)) {
    ; // PRESS INT FOR HELP until the first shout
  } else if (reload_prompt(1)) {
    ; // own mag swap under the shout
  } else {
    if (cshop == 0 && near_inactive_roulette(1)) {
      Render::prompt("NO LUCK HERE", Colour::white);
    } else {
      shop_prompt(cshop, "", 1);
    }
  }
  const uint32_t cli_now = millis();
  if (wave_frame) {
    if (boss_alive()) {
      snprintf(hint_buf, sizeof(hint_buf), "BOSS WAVE!"); // 2s locally, like the host hint
      hint_col = Colour::purple;
      hint_until = cli_now + 2000;
    } else if (roulette_moved() && cli_now >= hint_until) {
      snprintf(hint_buf, sizeof(hint_buf), "ROLL RELOCATED!");
      hint_col = Colour::yellow; // explicit: painted below with this, no stale boss purple
      hint_until = cli_now + 2000;
    } else if (cli_now >= hint_until) {
      // wave rejoins, least important of the three: only when the strip is
      // free. Bled-out rejoins and held-on rises both announce (ties name the
      // bled-out first); mirrors the host banner
      const Sim::state& wv = Sim::view();
      const bool p1_back = cli_was_dead0 && wv.players[0].hp > 0;
      const bool p2_back = cli_was_dead1 && wv.players[1].hp > 0;
      const bool p1_held = was_c1_down && !c1_down && wv.players[0].hp > 0;
      const bool p2_held = was_c2_down && !c2_down && wv.players[1].hp > 0;
      if (p1_back || p2_back || p1_held || p2_held) {
        const bool p2 = p2_back || (!p1_back && p2_held);
        snprintf(hint_buf, sizeof(hint_buf), p2 ? "P2: I'M BACK!" : "P1: I'M BACK!");
        hint_col = p2 ? mate_p2col : Colour::green; // the returner's Colour
        hint_until = cli_now + 1500;
      }
    }
  }
  {
    // cli_was_dead advances after the banners so they still see the previous frame
    const Sim::state& wv = Sim::view();
    const bool d0 = wv.players[0].active && wv.players[0].downed;
    const bool d1 = wv.players[1].active && wv.players[1].downed;
    cli_was_dead0 = wv.players[0].active && wv.players[0].hp == 0 && !d0;
    cli_was_dead1 = wv.players[1].active && wv.players[1].hp == 0 && !d1;
  }
  if (cli_now < hint_until && hint_buf[0] != '\0') {
    Render::prompt(hint_buf, hint_col); // recent boss/roll/BACK wave wins over the proximity prompt
  }
  fire_buzz(); // the snapshot carries the event, so both buzzers sing
  push_roll_marker(); // active pad to the minimap, same epoch math as the host
  Render::update_camera();
  Render::draw();
  Panel::draw();
  Panel::blips();
  draw_hud();
}

void Game::send_input() {
  // ship our sticks and buttons every frame; the host owns the sim
  Net::player_input_msg in;
  in.jx = Net::qaxis(Input::jx());
  in.jy = Net::qaxis(Input::jy());
  in.buttons = (Input::fire_down() ? Net::fire_bit : 0) |
               (Input::reload_down() ? Net::reload_bit : 0) |
               (Input::interact_down() ? Net::interact_bit : 0) |
               (Input::pause_down() ? Net::pause_bit : 0);
  in.seq = (uint8_t)seq_out++;
  handler.send(&in, sizeof(in));
}

void Game::mirror_pause() {
  // Host-owned menu on the client: keep shipping inputs (PAUSE resumes from either
  // board), paint the host cursor, never step it locally. scr stays playing so
  // update() keeps routing here; the chrome entry is tracked by Screens::paint itself
  // No buzz here: the frozen snapshot event would otherwise jingle every frame
  send_input();

  if (rx_ready) {
    Sim::apply_snapshot(rx_state);
    rx_ready = false;
    cli_last_rx = millis();
  }
  if (millis() - cli_last_rx >= cli_quiet_ms) {
    cli_mirror = false;
    net_multi = false;
    enter_menu(); // host left: drop to menu
    return;
  }
  if (rx_state.screen == Net::SCREEN_PLAYING) {
    // host resumed or restarted: rebuild the arena chrome like the host resume path
    cli_mirror = false;
    Screens::invalidate();
    Panel::init(); hud_first = true;         // the pause menu covered the panel and the minimap
    Render::repaint();     // clear leftover pause menu
    return;
  }
  if (rx_state.screen == Net::SCREEN_OVER) {
    cli_mirror = false;
    enter_game_over(); // host declared it, we mirror and store it on our own card
    return;
  }
  uint8_t s = rx_state.sel;
  if (s >= Screens::count(Screens::id::pause)) {
    s = 0; // corrupt/clamped cursor never blanks the chrome (paint guards sel)
  }
  cli_mirror = true; // the arena chrome is covered until the one-shot exit above
  Screens::paint(Screens::id::pause, s);
}
