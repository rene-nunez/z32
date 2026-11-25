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

// game core: boot, frame dispatch, menus and shared state. Shop/net/hud live in
// their own files; every static below is defined once here and used across them

Handler Game::_handler;
uint32_t Game::_tick = 0;

bool Game::_net_multi = false;
uint32_t Game::_wait_since = 0;
uint32_t Game::_wait_last_hb = 0;
bool Game::_peer_seen = false;
uint16_t Game::_seq_out = 0;
uint8_t Game::_in_buttons = 0;
uint8_t Game::_in_prev = 0;
float Game::_in_jx = 0.0f;
float Game::_in_jy = 0.0f;
uint32_t Game::_in_last_ms = 0;
volatile bool Game::_rx_ready = false;
bool Game::_cli_mirror = false;
bool Game::_cli_pause_logged = false;
bool Game::_cli_was_down0 = false;
bool Game::_cli_was_down1 = false;
bool Game::_cli_was_dead0 = false;
bool Game::_cli_was_dead1 = false;
bool Game::_cli_shouted0 = false;
bool Game::_cli_shouted1 = false;
uint16_t Game::_cli_wave = 0;
Net::game_state_msg Game::_rx_state;
Net::game_state_msg Game::_tx_state;
uint32_t Game::_cli_last_rx = 0;

int16_t Game::_shop_hx = -1;
int16_t Game::_shop_hy = -1;
int16_t Game::_shop_dx = -1;
int16_t Game::_shop_dy = -1;
int16_t Game::_shop_sx = -1;
int16_t Game::_shop_sy = -1;
int16_t Game::_shop_cx = -1;
int16_t Game::_shop_cy = -1;
int16_t Game::_shop_rx[MAX_PADS] = {-1, -1, -1, -1, -1, -1};
int16_t Game::_shop_ry[MAX_PADS] = {-1, -1, -1, -1, -1, -1};
uint8_t Game::_shop_rn = 0;
char Game::_hint_buf[28] = {0};
uint16_t Game::_hint_col = Colour::yellow;
uint32_t Game::_hint_until = 0;
char Game::_chat_buf[28] = {0};
uint16_t Game::_chat_col = Colour::yellow;
uint32_t Game::_chat_until = 0;
uint8_t Game::_chat_seq = 0;
bool Game::_chat_pip = false;
bool Game::_was_down0 = false;
bool Game::_was_down1 = false;
bool Game::_was_dead0 = false;
bool Game::_was_dead1 = false;
bool Game::_shouted0 = false;
bool Game::_shouted1 = false;
bool Game::_p2_interact = false;
bool Game::_p2_pause_edge = false;

uint32_t Game::_last_frame_ms = 0;
uint32_t Game::_intro_ms0 = 0;

Screens::id Game::_scr = Screens::id::logo;
uint8_t Game::_sel = 0;
int8_t Game::_nav_dir = 0;
uint16_t Game::_hud_wave = 0xFFFF, Game::_hud_kills = 0xFFFF;
uint8_t Game::_hud_role = 0xFF;
Sim::weapon Game::_hud_gun = (Sim::weapon)0xFF;
uint8_t Game::_hud_ammo = 0xFF;
uint8_t Game::_hud_shown = 0xFF;
bool Game::_hud_first = true;

constexpr uint16_t Game::_mate_p2col; // class constexpr, defined once for net/shop/hud

bool Game::begin(uint8_t role) {
  Serial.begin(115200);
  delay(200);

  if (!_handler.begin(role)) {
    Serial.println("[game] network init failed, resetting...");
    delay(1000);
    ESP.restart();
    return false;
  }

  _handler.on_message(msg_type::heartbeat, _on_heartbeat);
  _handler.on_message(msg_type::game_state, _on_state);
  _handler.on_message(msg_type::player_input, _on_input);
  _handler.on_message(msg_type::chat, _on_chat);

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

  _scr = Screens::id::logo;
  _sel = 0;
  _nav_dir = 0;
  _intro_ms0 = millis();
  _last_frame_ms = _intro_ms0;

  Screens::paint(_scr, _sel);
  Buzz::play(Buzz::jingle::intro); // fanfare over the logo screen

  Serial.printf("[game] ready role=%s\n", role == ROLE_HOST ? "host" : "client");
  return true;
}

void Game::update() {
  Input::update();
  Buzz::update(millis()); // the note sequencer runs on every screen

  switch (_scr) {
    case Screens::id::logo: _update_logo(); break;
    case Screens::id::team: _update_team(); break;
    case Screens::id::menu: _update_menu(); break;
    case Screens::id::mode: _update_mode(); break;
    case Screens::id::scores: _update_scores(); break;
    case Screens::id::playing: _update_playing(); break;
    case Screens::id::pause: _update_pause(); break;
    case Screens::id::game_over: _update_game_over(); break;
    case Screens::id::waiting: _update_waiting(); break;
  }

  // target-paced: a heavy frame pushes the next one out, it never catches up
  const uint32_t spent = millis() - _last_frame_ms;
  if (spent < _frame_ms) {
    delay(_frame_ms - spent);
  }
  _last_frame_ms = millis();
}

int8_t Game::_nav_edge() {
  const int8_t cur = Input::jy() > 0.5f ? 1 : (Input::jy() < -0.5f ? -1 : 0);
  const int8_t edge = (cur != 0 && _nav_dir == 0) ? cur : 0;
  _nav_dir = cur;
  return edge;
}

void Game::_start_game(bool multi) {
  _net_multi = multi;
  // solo always frames player 1; multi frames the local board (host P1, client P2)
  Render::set_focus((multi && _handler.role() == ROLE_CLIENT) ? 1 : 0);
  _seq_out = 0;
  _in_prev = _in_buttons; // hold levels so a held PAUSE/FIRE does not phantom-edge on entry
  _in_jx = _in_jy = 0.0f;
  _rx_ready = false;
  _cli_mirror = false; // a fresh run owns the arena again, never the pause chrome
  _cli_pause_logged = false; // a fresh run re-arms the pause-from-peer line
  _cli_was_down0 = _cli_was_down1 = false; // no rise edge on the join frame
  _cli_was_dead0 = _cli_was_dead1 = false; // no BACK edge on the join frame
  _cli_shouted0 = _cli_shouted1 = false; // HELP shows on the next down
  _was_down0 = _was_down1 = false; // no fall/death edge on the join frame
  _was_dead0 = _was_dead1 = false; // no BACK edge on the join frame
  _shouted0 = _shouted1 = false; // HELP shows on the next down
  _cli_wave = 0;
  _cli_last_rx = millis(); // grace window so a fresh client is not instantly "quiet"
  _p2_interact = _p2_pause_edge = false;
  Sim::reset();
  if (multi && _handler.role() == ROLE_HOST) {
    Sim::set_p2_active(true); // the client never steps, it follows snapshots
  }
  _scan_shops();
  _hint_until = 0;
  _hint_buf[0] = '\0';
  _hint_col = Colour::yellow;
  _chat_until = 0;
  _chat_buf[0] = '\0';
  _chat_pip = false;
  Panel::init();
  _hud_first = true; // the menu chrome covered the HUD strip: wipe+repaint it fully
  Render::update_camera();
  Render::repaint(); // forced: the game over screen cleared the arena and the camera may not move
  _scr = Screens::id::playing;
  Screens::invalidate();
}

void Game::_enter_menu() {
  _scr = Screens::id::menu;
  _sel = 0;
  _nav_dir = 0;
}

void Game::_enter_game_over() {
  const Sim::state& v = Sim::view();
  // each board stores the run on its own card off the same shared state
  Scores::add_run(v.kills, v.points, v.wave); // fold the run: wallet at death, kills, wave
  _scr = Screens::id::game_over;
  _sel = 0;
}

void Game::_sleep() {
  Buzz::stop();
  Display::backlight(false);

  rtc_gpio_pullup_en((gpio_num_t)BTN_PAUSE);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PAUSE, 0); // LOW = pressed

  Serial.println("[game] sleeping...");
  Serial.flush();
  esp_deep_sleep_start();
}

// the wrap reads the count from the screen itself, so adding an item cannot leave a
// hardcoded modulus behind
void Game::_nav_step() {
  const int8_t e = _nav_edge();
  const uint8_t n = Screens::count(_scr);
  if (e && n > 0) {
    _sel = (uint8_t)((_sel + n + e) % n);
    Buzz::play(Buzz::jingle::menu); // cursor tick on every menu move
  }
}

void Game::_update_logo() {
  if (Input::fire_pressed() || millis() - _intro_ms0 >= _intro_ms) {
    _scr = Screens::id::team; // timed or skippable, then the team screen
    _sel = 0;
    _intro_ms0 = millis();
    Buzz::play(Buzz::jingle::intro); // re-fire: a skip still hears it from the top
  }
  Screens::paint(_scr, _sel);
}

void Game::_update_team() {
  if (Input::fire_pressed() || Input::pause_pressed() || millis() - _intro_ms0 >= _intro_ms) {
    _enter_menu(); // timed like the logo, FIRE/PAUSE skip it
  }
  Screens::paint(_scr, _sel);
}

void Game::_update_menu() {
  _nav_step();
  if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu); // confirm blip (inaudible on Exit, it sleeps)
    switch (_sel) {
      case 0:
        _scr = Screens::id::mode;
        _sel = 0;
        break;
      case 1:
        _scr = Screens::id::scores;
        _sel = 0;
        break;
      default: // Exit
        _sleep();
        break;
    }
  }
  Screens::paint(_scr, _sel);
}

void Game::_update_mode() {
  _nav_step();
  if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    if (_sel == 2) { // Back
      _enter_menu();
    } else if (_sel == 0) {
      _start_game(false); // Solo: local run, the radio stays silent
    } else {
      _scr = Screens::id::waiting;
      _sel = 0;
      _wait_since = millis();
      _wait_last_hb = 0;
      _peer_seen = false;
      _rx_ready = false;
    }
  }
  Screens::paint(_scr, _sel);
}

void Game::_update_waiting() {
  const uint32_t now = millis();
  if (now - _wait_last_hb >= _wait_hb_ms) {
    heartbeat_msg hb;
    hb.tick = _tick++;
    hb.role = _handler.role();
    _handler.send(&hb, sizeof(hb));
    _wait_last_hb = now;
  }
  if (Input::pause_pressed()) {
    _enter_menu(); // bail out
    return;
  }
  if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    _start_game(false); // tired of waiting: play solo
    return;
  }
  if (_handler.role() == ROLE_CLIENT) {
    // the client joins off a live snapshot (host map), never off a bare heartbeat,
    // so it lands straight into the running frame instead of an empty reset
    if (_rx_ready) {
      const Net::game_state_msg snap = _rx_state; // copy: _start_game clears the flag
      _rx_ready = false;
      _peer_seen = false;
      Buzz::play(Buzz::jingle::wave);
      Serial.println("[net] joined multi role=client");
      _start_game(true);
      Sim::apply_snapshot(snap); // start from the live frame, not the reset one
      _cli_last_rx = millis();
      return;
    }
  } else if (_peer_seen) {
    _peer_seen = false;
    Buzz::play(Buzz::jingle::wave);
    Serial.println("[net] joined multi role=host");
    _start_game(true); // peer showed up (a client also joins mid-run off snapshots)
    return;
  }
  if (now - _wait_since >= _wait_ms) {
    Buzz::play(Buzz::jingle::denied); // nobody out there
    Serial.println("[net] waiting timeout, solo");
    _enter_menu();
    return;
  }
  Screens::paint(_scr, _sel);
}

void Game::_update_scores() {
  if (Input::fire_pressed() || Input::pause_pressed()) {
    if (Input::fire_pressed()) {
      Buzz::play(Buzz::jingle::menu);
    }
    _enter_menu();
  }
  Screens::paint(_scr, _sel);
}

void Game::_update_playing() {
  // solo is always a local sim on both boards (silent radio); only multi + client mirrors
  if (!_net_multi) {
    _update_playing_host();
    return;
  }
  if (_handler.role() == ROLE_CLIENT) {
    _update_playing_client();
    return;
  }
  _update_playing_host();
}

void Game::_update_pause() {
  _nav_step();
  // the client keeps shipping inputs while frozen, so derive its PAUSE edge here too:
  // otherwise nobody converts levels to an edge while the host sits in pause and the
  // client can never resume
  if (_net_multi && _handler.role() == ROLE_HOST) {
    const bool lvl = (_in_buttons & Net::pause_bit) != 0;
    _p2_pause_edge = lvl && ((_in_prev & Net::pause_bit) == 0);
    _in_prev = _in_buttons;
  }
  if (Input::pause_pressed() || _p2_pause_edge) {
    _p2_pause_edge = false;
    _scr = Screens::id::playing;
    Screens::invalidate();
    Panel::init(); _hud_first = true; // the pause menu covered the panel and the minimap
    Render::repaint(); // clear leftover pause menu
  } else if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    switch (_sel) {
      case 0: // Continue
        _scr = Screens::id::playing;
        Screens::invalidate();
        Panel::init(); _hud_first = true; // the pause menu covered the panel and the minimap
        Render::repaint(); // clear leftover pause menu
        break;
      case 1: // Restart
        _start_game(_net_multi); // a co-op pause restarts co-op
        break;
      default: // Exit to Menu
        _enter_menu();
        break;
    }
  }
  Screens::paint(_scr, _sel);
  if (_net_multi && _handler.role() == ROLE_HOST) {
    _broadcast(); // frozen sim keeps flowing so the client holds the frame
  }
}

void Game::_update_game_over() {
  // solo on either board is a local Game over; only a multi client mirrors the host
  if (_net_multi && _handler.role() == ROLE_CLIENT) {
    // full mirror like the pause chrome: the host owns the cursor, we only paint it
    if (_rx_ready) {
      Sim::apply_snapshot(_rx_state);
      _rx_ready = false;
      _cli_last_rx = millis();
      bool anyone = false;
      for (uint8_t p = 0; p < Sim::NUM_PLAYERS; ++p) {
        anyone |= Sim::view().players[p].active && Sim::view().players[p].hp > 0;
      }
      if (anyone) {
        const Net::game_state_msg snap = _rx_state; // copy: _start_game clears the flag
        _start_game(true); // host restarted: re-init chrome, snapshots fill the sim
        Sim::apply_snapshot(snap); // paint live values on frame 1, never the reset Sim
        return;
      }
    }
    if (Input::fire_pressed()) {
      Buzz::play(Buzz::jingle::menu); // feedback only, the host drives restart
    }
    if (millis() - _cli_last_rx >= _cli_quiet_ms) {
      _net_multi = false;
      Serial.println("[net] peer quiet 3000ms, back to menu");
      _enter_menu(); // host left: drop to menu
      return;
    }
    uint8_t s = _rx_state.sel;
    if (s >= Screens::count(Screens::id::game_over)) {
      s = 0; // corrupt cursor never blanks the chrome (paint guards sel too)
    }
    _sel = s;
    Screens::paint(_scr, _sel);
    return;
  }
  _nav_step();
  if (Input::fire_pressed()) {
    Buzz::play(Buzz::jingle::menu);
    if (_sel == 0) {
      _start_game(_net_multi);
    } else {
      _enter_menu();
    }
  }
  Screens::paint(_scr, _sel);
  if (_net_multi) {
    _broadcast(); // the over event keeps flowing until the host restarts or exits
  }
}
