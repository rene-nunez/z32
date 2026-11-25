// Game net: ESP-NOW handlers and per-board playing frames

#include <Arduino.h>

#include <Buzz.h>
#include <Display.h>
#include <Panel.h>
#include <Render.h>
#include <Screens.h>
#include <Sim.h>

#include "Game.h"

void Game::on_heartbeat(const uint8_t* data, size_t len) {
  if (len < sizeof(heartbeat_msg)) {
    return;
  }
  (void)data;
  if (handler.role() == ROLE_HOST) {
    peer_seen = true;
  }
}

void Game::on_state(const uint8_t* data, size_t len) {
  if (handler.role() != ROLE_CLIENT) {
    return;
  }
  if (len < sizeof(Net::game_state_msg)) {
    return;
  }
  memcpy(&rx_state, data, sizeof(Net::game_state_msg));
  rx_ready = true;
  peer_seen = true;
}

void Game::on_input(const uint8_t* data, size_t len) {
  if (handler.role() != ROLE_HOST) {
    return;
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
    return;
  }
  snprintf(chat_buf, sizeof(chat_buf), "P%u: SAVE ME!", (unsigned)(m.from + 1u));
  chat_col = (m.from == 0) ? Colour::green : mate_p2col;
  chat_until = millis() + 2000;
  chat_pip = true;
}

void Game::send_chat(uint8_t from) {
  if (!net_multi) {
    return;
  }
  Net::chat_msg m;
  m.from = from;
  m.id = (uint8_t)Net::chat_id::come;
  m.seq = chat_seq++;
  handler.send(&m, sizeof(m));
  handler.send(&m, sizeof(m));
  handler.send(&m, sizeof(m));
  snprintf(chat_buf, sizeof(chat_buf), "P%u: SAVE ME!", (unsigned)(from + 1u));
  chat_col = (from == 0) ? Colour::green : mate_p2col;
  chat_until = millis() + 2000;
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
  const uint32_t now = millis();
  Sim::ctl c = {};
  if (net_multi && now - in_last_ms < in_stale_ms) {
    c.jx = in_jx;
    c.jy = in_jy;
    const bool p2_lvl = (in_buttons & Net::fire_bit) != 0;
    c.fire = Sim::auto_fire(Sim::view().guns[1]) ? p2_lvl : (p2_lvl && !(in_prev & Net::fire_bit));
    p2_interact = (in_buttons & Net::interact_bit) && !(in_prev & Net::interact_bit);
    p2_pause_edge = (in_buttons & Net::pause_bit) && !(in_prev & Net::pause_bit);
    c.reload = (in_buttons & Net::reload_bit) && !(in_prev & Net::reload_bit);
    in_prev = in_buttons;
  } else {
    in_prev = in_buttons;
  }
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

  Render::clear();
  if (!Sim::step(now)) {
    Buzz::play(Buzz::jingle::over);
    enter_game_over();
    if (net_multi) {
      broadcast();
    }
    return;
  }
  const bool was_d0 = was_down0;
  const bool was_d1 = was_down1;
  if (revive_update(now)) {
    Render::prompt(hint_buf, hint_col);
  } else {
    shop_update(now);
  }
  if (Sim::view().last_event == Sim::event::wave) {
    if (boss_alive()) {
      snprintf(hint_buf, sizeof(hint_buf), "BOSS WAVE!");
      hint_col = Colour::purple;
      hint_until = now + 2000;
      Render::prompt(hint_buf, hint_col);
    } else if (roulette_moved() && now >= hint_until) {
      snprintf(hint_buf, sizeof(hint_buf), "ROLL RELOCATED!");
      hint_col = Colour::yellow;
      hint_until = now + 2000;
      Render::prompt(hint_buf);
    } else if (now >= hint_until) {
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
        hint_col = p1 ? Colour::green : mate_p2col;
        hint_until = now + 1500;
        Render::prompt(hint_buf, hint_col);
      }
    }
  }
  {
    const Sim::state& wv = Sim::view();
    const bool d0 = wv.players[0].active && wv.players[0].downed;
    const bool d1 = net_multi && wv.players[1].active && wv.players[1].downed;
    was_dead0 = wv.players[0].active && wv.players[0].hp == 0 && !d0;
    was_dead1 = net_multi && wv.players[1].active && wv.players[1].hp == 0 && !d1;
  }
  fire_buzz();
  push_roll_marker();
  Render::update_camera();
  Render::draw();
  Panel::draw();
  Panel::blips();
  draw_hud();
  if (net_multi) {
    broadcast();
  }
}

void Game::update_playing_client() {
  send_input();

  if (rx_state.screen == Net::SCREEN_PAUSE) {
    if (!cli_pause_logged) {
      cli_pause_logged = true;
      Serial.println("[net] pause from peer");
    }
    mirror_pause();
    return;
  }
  cli_pause_logged = false;
  if (cli_mirror) {
    cli_mirror = false;
    Screens::invalidate();
    Panel::init(); hud_first = true;
    Render::repaint();
  }

  Render::clear();
  if (rx_ready) {
    Sim::apply_snapshot(rx_state);
    rx_ready = false;
    cli_last_rx = millis();
  }
  if (millis() - cli_last_rx >= cli_quiet_ms) {
    Buzz::play(Buzz::jingle::denied);
    Serial.println("[net] peer quiet 3000ms, back to menu");
    net_multi = false;
    enter_menu();
    return;
  }
  if (Sim::view().last_event == Sim::event::over) {
    enter_game_over();
    return;
  }
  const Sim::state& cv = Sim::view();
  const bool c1_down = cv.players[0].active && cv.players[0].downed;
  const bool c2_down = cv.players[1].active && cv.players[1].downed;
  bool edge_hint = false;
  const bool was_c1_down = cli_was_down0;
  const bool was_c2_down = cli_was_down1;
  const bool wave_rose = (cv.wave > cli_wave);
  const bool wave_frame = (cv.last_event == Sim::event::wave) ||
                          (wave_rose && cv.last_event == Sim::event::none);
  if (!cli_was_down0 && c1_down) {
    cli_shouted0 = false;
  }
  if (!cli_was_down1 && c2_down) {
    cli_shouted1 = false;
  }
  if (cv.wave < cli_wave) {
    cli_was_down0 = c1_down;
    cli_was_down1 = c2_down;
  } else if (!wave_frame) {
    const bool p1_rose = cli_was_down0 && !c1_down && cv.players[0].hp > 0;
    const bool p2_rose = cli_was_down1 && !c2_down && cv.players[1].hp > 0;
    const bool p1_died = cli_was_down0 && !c1_down && cv.players[0].hp == 0;
    const bool p2_died = cli_was_down1 && !c2_down && cv.players[1].hp == 0;
    const uint32_t now_rx = millis();
    if (p2_rose || p1_rose) {
      snprintf(hint_buf, sizeof(hint_buf), p2_rose ? "P2: THX!" : "P1: THX!");
      hint_col = p2_rose ? mate_p2col : Colour::green;
      hint_until = now_rx + 1500;
      edge_hint = true;
    } else if (p2_died || p1_died) {
      snprintf(hint_buf, sizeof(hint_buf), p2_died ? "P2: I'M OUT!" : "P1: I'M OUT!");
      hint_col = p2_died ? mate_p2col : Colour::green;
      hint_until = now_rx + 2000;
      edge_hint = true;
    }
    cli_was_down0 = c1_down;
    cli_was_down1 = c2_down;
  } else {
    cli_was_down0 = c1_down;
    cli_was_down1 = c2_down;
  }
  cli_wave = cv.wave;
  const uint8_t cshop = shop_at(1);
  if (cv.players[1].downed && cv.players[0].hp > 0 && Input::interact_pressed() &&
      !revive_near(0) && cshop == 0) {
    send_chat(1);
  }
  if (edge_hint) {
    Render::prompt(hint_buf, hint_col);
  } else if (revive_near(1)) {
    Render::prompt("PRESS INT TO REVIVE");
  } else if (urgent_callout(1)) {
    ;
  } else if (help_hint(1)) {
    ;
  } else if (reload_prompt(1)) {
    ;
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
      snprintf(hint_buf, sizeof(hint_buf), "BOSS WAVE!");
      hint_col = Colour::purple;
      hint_until = cli_now + 2000;
    } else if (roulette_moved() && cli_now >= hint_until) {
      snprintf(hint_buf, sizeof(hint_buf), "ROLL RELOCATED!");
      hint_col = Colour::yellow;
      hint_until = cli_now + 2000;
    } else if (cli_now >= hint_until) {
      const Sim::state& wv = Sim::view();
      const bool p1_back = cli_was_dead0 && wv.players[0].hp > 0;
      const bool p2_back = cli_was_dead1 && wv.players[1].hp > 0;
      const bool p1_held = was_c1_down && !c1_down && wv.players[0].hp > 0;
      const bool p2_held = was_c2_down && !c2_down && wv.players[1].hp > 0;
      if (p1_back || p2_back || p1_held || p2_held) {
        const bool p2 = p2_back || (!p1_back && p2_held);
        snprintf(hint_buf, sizeof(hint_buf), p2 ? "P2: I'M BACK!" : "P1: I'M BACK!");
        hint_col = p2 ? mate_p2col : Colour::green;
        hint_until = cli_now + 1500;
      }
    }
  }
  {
    const Sim::state& wv = Sim::view();
    const bool d0 = wv.players[0].active && wv.players[0].downed;
    const bool d1 = wv.players[1].active && wv.players[1].downed;
    cli_was_dead0 = wv.players[0].active && wv.players[0].hp == 0 && !d0;
    cli_was_dead1 = wv.players[1].active && wv.players[1].hp == 0 && !d1;
  }
  if (cli_now < hint_until && hint_buf[0] != '\0') {
    Render::prompt(hint_buf, hint_col);
  }
  fire_buzz();
  push_roll_marker();
  Render::update_camera();
  Render::draw();
  Panel::draw();
  Panel::blips();
  draw_hud();
}

void Game::send_input() {
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
  send_input();

  if (rx_ready) {
    Sim::apply_snapshot(rx_state);
    rx_ready = false;
    cli_last_rx = millis();
  }
  if (millis() - cli_last_rx >= cli_quiet_ms) {
    cli_mirror = false;
    net_multi = false;
    enter_menu();
    return;
  }
  if (rx_state.screen == Net::SCREEN_PLAYING) {
    cli_mirror = false;
    Screens::invalidate();
    Panel::init(); hud_first = true;
    Render::repaint();
    return;
  }
  if (rx_state.screen == Net::SCREEN_OVER) {
    cli_mirror = false;
    enter_game_over();
    return;
  }
  uint8_t s = rx_state.sel;
  if (s >= Screens::count(Screens::id::pause)) {
    s = 0;
  }
  cli_mirror = true;
  Screens::paint(Screens::id::pause, s);
}
