// Game core: boot, frame pacing and menu dispatch

#include <Arduino.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>

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

Handler Game::handler;
uint32_t Game::tick = 0;

bool Game::net_multi = false;
uint32_t Game::wait_since = 0;
uint32_t Game::wait_last_hb = 0;
bool Game::peer_seen = false;
uint16_t Game::seq_out = 0;
uint8_t Game::in_buttons = 0;
uint8_t Game::in_prev = 0;
float Game::in_jx = 0.0f;
float Game::in_jy = 0.0f;
uint32_t Game::in_last_ms = 0;
volatile bool Game::rx_ready = false;
bool Game::cli_mirror = false;
bool Game::cli_pause_logged = false;
bool Game::cli_was_down0 = false;
bool Game::cli_was_down1 = false;
bool Game::cli_was_dead0 = false;
bool Game::cli_was_dead1 = false;
bool Game::cli_shouted0 = false;
bool Game::cli_shouted1 = false;
uint16_t Game::cli_wave = 0;
Net::game_state_msg Game::rx_state;
Net::game_state_msg Game::tx_state;
uint32_t Game::cli_last_rx = 0;

int16_t Game::shop_hx = -1;
int16_t Game::shop_hy = -1;
int16_t Game::shop_dx = -1;
int16_t Game::shop_dy = -1;
int16_t Game::shop_sx = -1;
int16_t Game::shop_sy = -1;
int16_t Game::shop_cx = -1;
int16_t Game::shop_cy = -1;
int16_t Game::shop_rx[MAX_PADS] = {-1, -1, -1, -1, -1, -1};
int16_t Game::shop_ry[MAX_PADS] = {-1, -1, -1, -1, -1, -1};
uint8_t Game::shop_rn = 0;
char Game::hint_buf[28] = {0};
uint16_t Game::hint_col = Colour::yellow;
uint32_t Game::hint_until = 0;
char Game::chat_buf[28] = {0};
uint16_t Game::chat_col = Colour::yellow;
uint32_t Game::chat_until = 0;
uint8_t Game::chat_seq = 0;
bool Game::chat_pip = false;
bool Game::was_down0 = false;
bool Game::was_down1 = false;
bool Game::was_dead0 = false;
bool Game::was_dead1 = false;
bool Game::shouted0 = false;
bool Game::shouted1 = false;
bool Game::p2_interact = false;
bool Game::p2_pause_edge = false;

uint32_t Game::last_frame_ms = 0;
uint32_t Game::intro_ms0 = 0;

Screens::id Game::scr = Screens::id::logo;
uint8_t Game::sel = 0;
int8_t Game::nav_dir = 0;
uint16_t Game::hud_wave = 0xFFFF, Game::hud_kills = 0xFFFF;
uint8_t Game::hud_role = 0xFF;
Sim::weapon Game::hud_gun = (Sim::weapon)0xFF;
uint8_t Game::hud_ammo = 0xFF;
uint8_t Game::hud_shown = 0xFF;
bool Game::hud_first = true;

constexpr uint16_t Game::mate_p2col;

bool Game::begin(uint8_t role) {
  Serial.begin(115200);
  delay(200);

  if (!handler.begin(role)) {
    Serial.println("[game] network init failed, resetting...");
    delay(1000);
    ESP.restart();
    return false;
  }

  handler.on_message(msg_type::heartbeat, on_heartbeat);
  handler.on_message(msg_type::game_state, on_state);
  handler.on_message(msg_type::player_input, on_input);
  handler.on_message(msg_type::chat, on_chat);

  if (!Display::begin()) {
    Serial.println("[game] display init failed, resetting...");
    delay(1000);
    ESP.restart();
    return false;
  }

  if (!Input::begin()) {
    Serial.println("[game] input init failed, resetting...");
    delay(1000);
    ESP.restart();
    return false;
  }

  Tilemap::init();

  Scores::load();

  if (!Buzz::begin()) {
    Serial.println("[game] buzz init failed, silent mode");
  }

  scr = Screens::id::logo;
  sel = 0;
  nav_dir = 0;
  intro_ms0 = millis();
  last_frame_ms = intro_ms0;

  Screens::paint(scr, sel);
  Buzz::play(Buzz::jingle::intro);

  Serial.printf("[game] ready role=%s\n", role == ROLE_HOST ? "host" : "client");
  return true;
}

void Game::update() {
  Input::update();
  Buzz::update(millis());

  switch (scr) {
    case Screens::id::logo: update_logo(); break;
    case Screens::id::team: update_team(); break;
    case Screens::id::menu: update_menu(); break;
    case Screens::id::mode: update_mode(); break;
    case Screens::id::scores: update_scores(); break;
    case Screens::id::playing: update_playing(); break;
    case Screens::id::pause: update_pause(); break;
    case Screens::id::game_over: update_game_over(); break;
    case Screens::id::waiting: update_waiting(); break;
  }

  const uint32_t spent = millis() - last_frame_ms;
  if (spent < frame_ms) {
    delay(frame_ms - spent);
  }
  last_frame_ms = millis();
}

int8_t Game::nav_edge() {
  const int8_t cur = Input::jy() > 0.5f ? 1 : (Input::jy() < -0.5f ? -1 : 0);
  const int8_t edge = (cur != 0 && nav_dir == 0) ? cur : 0;
  nav_dir = cur;
  return edge;
}

void Game::start_game(bool multi) {
  net_multi = multi;
  Render::set_focus((multi && handler.role() == ROLE_CLIENT) ? 1 : 0);
  seq_out = 0;
  in_prev = in_buttons;
  in_jx = in_jy = 0.0f;
  rx_ready = false;
  cli_mirror = false;
  cli_pause_logged = false;
  cli_was_down0 = cli_was_down1 = false;
  cli_was_dead0 = cli_was_dead1 = false;
  cli_shouted0 = cli_shouted1 = false;
  was_down0 = was_down1 = false;
  was_dead0 = was_dead1 = false;
  shouted0 = shouted1 = false;
  cli_wave = 0;
  cli_last_rx = millis();
  p2_interact = p2_pause_edge = false;
  Sim::reset();
  if (multi && handler.role() == ROLE_HOST) {
    Sim::set_p2_active(true);
  }
  scan_shops();
  hint_until = 0;
  hint_buf[0] = '\0';
  hint_col = Colour::yellow;
  chat_until = 0;
  chat_buf[0] = '\0';
  chat_pip = false;
  Panel::init();
  hud_first = true;
  Render::update_camera();
  Render::repaint();
  scr = Screens::id::playing;
  Screens::invalidate();
}

void Game::enter_menu() {
  scr = Screens::id::menu;
  sel = 0;
  nav_dir = 0;
}

void Game::enter_game_over() {
  const Sim::state& v = Sim::view();
  Scores::add_run(v.kills, v.points, v.wave);
  scr = Screens::id::game_over;
  sel = 0;
}

void Game::sleep() {
  Buzz::stop();
  Display::backlight(false);

  rtc_gpio_pullup_en((gpio_num_t)BTN_PAUSE);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PAUSE, 0);

  Serial.println("[game] sleeping...");
  Serial.flush();
  esp_deep_sleep_start();
}

void Game::nav_step() {
  const int8_t e = nav_edge();
  const uint8_t n = Screens::count(scr);
  if (e && n > 0) {
    sel = (uint8_t)((sel + n + e) % n);
    Buzz::play(Buzz::jingle::menu);
  }
}

void Game::update_logo() {
  if (Input::fire_pressed() || millis() - intro_ms0 >= intro_ms) {
    scr = Screens::id::team;
    sel = 0;
    intro_ms0 = millis();
    Buzz::play(Buzz::jingle::intro);
  }
  Screens::paint(scr, sel);
}

void Game::update_team() {
  if (Input::fire_pressed() || Input::pause_pressed() || millis() - intro_ms0 >= intro_ms) {
    enter_menu();
  }
  Screens::paint(scr, sel);
}

void Game::update_menu() {
  nav_step();
  if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    switch (sel) {
      case 0:
        scr = Screens::id::mode;
        sel = 0;
        break;
      case 1:
        scr = Screens::id::scores;
        sel = 0;
        break;
      default:
        sleep();
        break;
    }
  }
  Screens::paint(scr, sel);
}

void Game::update_mode() {
  nav_step();
  if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    if (sel == 2) {
      enter_menu();
    } else if (sel == 0) {
      start_game(false);
    } else {
      scr = Screens::id::waiting;
      sel = 0;
      wait_since = millis();
      wait_last_hb = 0;
      peer_seen = false;
      rx_ready = false;
    }
  }
  Screens::paint(scr, sel);
}

void Game::update_waiting() {
  const uint32_t now = millis();
  if (now - wait_last_hb >= wait_hb_ms) {
    heartbeat_msg hb;
    hb.tick = tick++;
    hb.role = handler.role();
    handler.send(&hb, sizeof(hb));
    wait_last_hb = now;
  }
  if (Input::pause_pressed()) {
    enter_menu();
    return;
  }
  if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    start_game(false);
    return;
  }
  if (handler.role() == ROLE_CLIENT) {
    if (rx_ready) {
      const Net::game_state_msg snap = rx_state;
      rx_ready = false;
      peer_seen = false;
      Buzz::play(Buzz::jingle::wave);
      Serial.println("[net] joined multi role=client");
      start_game(true);
      Sim::apply_snapshot(snap);
      cli_last_rx = millis();
      return;
    }
  } else if (peer_seen) {
    peer_seen = false;
    Buzz::play(Buzz::jingle::wave);
    Serial.println("[net] joined multi role=host");
    start_game(true);
    return;
  }
  if (now - wait_since >= wait_ms) {
    Buzz::play(Buzz::jingle::denied);
    Serial.println("[net] waiting timeout, solo");
    enter_menu();
    return;
  }
  Screens::paint(scr, sel);
}

void Game::update_scores() {
  if (Input::fire_pressed() || Input::pause_pressed()) {
    if (Input::fire_pressed()) {
      Buzz::play(Buzz::jingle::menu);
    }
    enter_menu();
  }
  Screens::paint(scr, sel);
}

void Game::update_playing() {
  if (!net_multi) {
    update_playing_host();
    return;
  }
  if (handler.role() == ROLE_CLIENT) {
    update_playing_client();
    return;
  }
  update_playing_host();
}

void Game::update_pause() {
  nav_step();
  if (net_multi && handler.role() == ROLE_HOST) {
    const bool lvl = (in_buttons & Net::pause_bit) != 0;
    p2_pause_edge = lvl && ((in_prev & Net::pause_bit) == 0);
    in_prev = in_buttons;
  }
  if (Input::pause_pressed() || p2_pause_edge) {
    p2_pause_edge = false;
    scr = Screens::id::playing;
    Screens::invalidate();
    Panel::init(); hud_first = true;
    Render::repaint();
  } else if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    switch (sel) {
      case 0:
        scr = Screens::id::playing;
        Screens::invalidate();
        Panel::init(); hud_first = true;
        Render::repaint();
        break;
      case 1:
        start_game(net_multi);
        break;
      default:
        enter_menu();
        break;
    }
  }
  Screens::paint(scr, sel);
  if (net_multi && handler.role() == ROLE_HOST) {
    broadcast();
  }
}

void Game::update_game_over() {
  if (net_multi && handler.role() == ROLE_CLIENT) {
    if (rx_ready) {
      Sim::apply_snapshot(rx_state);
      rx_ready = false;
      cli_last_rx = millis();
      bool anyone = false;
      for (uint8_t p = 0; p < Sim::NUM_PLAYERS; ++p) {
        anyone |= Sim::view().players[p].active && Sim::view().players[p].hp > 0;
      }
      if (anyone) {
        const Net::game_state_msg snap = rx_state;
        start_game(true);
        Sim::apply_snapshot(snap);
        return;
      }
    }
    if (Input::fire_pressed()) {
      Buzz::play(Buzz::jingle::menu);
    }
    if (millis() - cli_last_rx >= cli_quiet_ms) {
      net_multi = false;
      Serial.println("[net] peer quiet 3000ms, back to menu");
      enter_menu();
      return;
    }
    uint8_t s = rx_state.sel;
    if (s >= Screens::count(Screens::id::game_over)) {
      s = 0;
    }
    sel = s;
    Screens::paint(scr, sel);
    return;
  }
  nav_step();
  if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    if (sel == 0) {
      start_game(net_multi);
    } else {
      enter_menu();
    }
  }
  Screens::paint(scr, sel);
  if (net_multi) {
    broadcast();
  }
}
