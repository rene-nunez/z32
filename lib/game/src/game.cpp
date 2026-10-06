#include <Arduino.h>
#include <cmath>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <Buzz.h>
#include <Display.h>
#include <map.h>
#include <Panel.h>
#include <Render.h>
#include <Screens.h>
#include <Scores.h>
#include <Sim.h>

#include "Game.h"
#include "pins.h"

// game core: boot, frame dispatch, menus and shared state. Shop/net/hud live in
// their own files; every static below is defined once here and used across them.

handler game::_handler;
uint32_t game::_tick = 0;
uint32_t game::_peer_tick = 0;

bool game::_net_multi = false;
uint32_t game::_wait_since = 0;
uint32_t game::_wait_last_hb = 0;
bool game::_peer_seen = false;
uint16_t game::_seq_out = 0;
uint8_t game::_in_seq = 0;
uint8_t game::_in_buttons = 0;
uint8_t game::_in_prev = 0;
float game::_in_jx = 0.0f;
float game::_in_jy = 0.0f;
uint32_t game::_in_last_ms = 0;
volatile bool game::_rx_ready = false;
bool game::_cli_mirror = false;
bool game::_cli_was_down0 = false;
bool game::_cli_was_down1 = false;
bool game::_cli_was_dead0 = false;
bool game::_cli_was_dead1 = false;
bool game::_cli_shouted0 = false;
bool game::_cli_shouted1 = false;
uint16_t game::_cli_wave = 0;
net::game_state_msg game::_rx_state;
net::game_state_msg game::_tx_state;
uint32_t game::_cli_last_rx = 0;

int16_t game::_shop_hx = -1;
int16_t game::_shop_hy = -1;
int16_t game::_shop_dx = -1;
int16_t game::_shop_dy = -1;
int16_t game::_shop_sx = -1;
int16_t game::_shop_sy = -1;
int16_t game::_shop_cx = -1;
int16_t game::_shop_cy = -1;
int16_t game::_shop_rx[MAX_PADS] = {-1, -1, -1, -1, -1, -1};
int16_t game::_shop_ry[MAX_PADS] = {-1, -1, -1, -1, -1, -1};
uint8_t game::_shop_rn = 0;
char game::_hint_buf[28] = {0};
uint16_t game::_hint_col = colour::yellow;
uint32_t game::_hint_until = 0;
char game::_chat_buf[28] = {0};
uint16_t game::_chat_col = colour::yellow;
uint32_t game::_chat_until = 0;
uint8_t game::_chat_seq = 0;
bool game::_chat_pip = false;
bool game::_was_down0 = false;
bool game::_was_down1 = false;
bool game::_was_dead0 = false;
bool game::_was_dead1 = false;
bool game::_shouted0 = false;
bool game::_shouted1 = false;
bool game::_p2_interact = false;
bool game::_p2_pause_edge = false;

uint32_t game::_last_frame_ms = 0;
uint32_t game::_intro_ms0 = 0;

screens::id game::_scr = screens::id::logo;
uint8_t game::_sel = 0;
int8_t game::_nav_dir = 0;
uint16_t game::_hud_wave = 0xFFFF, game::_hud_kills = 0xFFFF;
uint8_t game::_hud_role = 0xFF;
sim::weapon game::_hud_gun = (sim::weapon)0xFF;
uint8_t game::_hud_ammo = 0xFF;
uint8_t game::_hud_shown = 0xFF;
bool game::_hud_first = true;

constexpr uint16_t game::_mate_p2col; // class constexpr, defined once for net/shop/hud

bool game::begin(uint8_t role) {
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

  if (!display::begin()) {
    Serial.println("[game] display init failed, resetting...");
    delay(1000);
    ESP.restart();
    return false;
  }

  if (!input::begin()) {
    Serial.println("[game] input init failed, resetting...");
    delay(1000);
    ESP.restart();
    return false;
  }

  tilemap::init();

  scores::load();

  if (!buzz::begin()) {
    Serial.println("[game] buzz init failed, silent mode");
  }

  _scr = screens::id::logo;
  _sel = 0;
  _nav_dir = 0;
  _intro_ms0 = millis();
  _last_frame_ms = _intro_ms0;

  screens::paint(_scr, _sel);
  buzz::play(buzz::jingle::intro); // fanfare over the logo screen

  Serial.println("[game] ready");
  return true;
}

void game::update() {
  input::update();
  buzz::update(millis()); // the note sequencer runs on every screen

  switch (_scr) {
    case screens::id::logo: _update_logo(); break;
    case screens::id::team: _update_team(); break;
    case screens::id::menu: _update_menu(); break;
    case screens::id::mode: _update_mode(); break;
    case screens::id::scores: _update_scores(); break;
    case screens::id::playing: _update_playing(); break;
    case screens::id::pause: _update_pause(); break;
    case screens::id::game_over: _update_game_over(); break;
    case screens::id::waiting: _update_waiting(); break;
  }

  // target-paced: a heavy frame pushes the next one out, it never catches up
  const uint32_t spent = millis() - _last_frame_ms;
  if (spent < _frame_ms) {
    delay(_frame_ms - spent);
  }
  _last_frame_ms = millis();
}

int8_t game::_nav_edge() {
  const int8_t cur = input::jy() > 0.5f ? 1 : (input::jy() < -0.5f ? -1 : 0);
  const int8_t edge = (cur != 0 && _nav_dir == 0) ? cur : 0;
  _nav_dir = cur;
  return edge;
}

void game::_start_game(bool multi) {
  _net_multi = multi;
  // solo always frames player 1; multi frames the local board (host P1, client P2)
  render::set_focus((multi && _handler.role() == ROLE_CLIENT) ? 1 : 0); // each board frames its own
  _seq_out = 0;
  _in_prev = _in_buttons; // hold levels so a held PAUSE/FIRE does not phantom-edge on entry
  _in_jx = _in_jy = 0.0f;
  _rx_ready = false;
  _cli_mirror = false; // a fresh run owns the arena again, never the pause chrome
  _cli_was_down0 = _cli_was_down1 = false; // no rise edge on the join frame
  _cli_was_dead0 = _cli_was_dead1 = false; // no BACK edge on the join frame
  _cli_shouted0 = _cli_shouted1 = false; // HELP shows on the next down
  _was_down0 = _was_down1 = false; // no fall/death edge on the join frame
  _was_dead0 = _was_dead1 = false; // no BACK edge on the join frame
  _shouted0 = _shouted1 = false; // HELP shows on the next down
  _cli_wave = 0;
  _cli_last_rx = millis(); // grace window so a fresh client is not instantly "quiet"
  _p2_interact = _p2_pause_edge = false;
  sim::reset();
  if (multi && _handler.role() == ROLE_HOST) {
    sim::set_p2_active(true); // the client never steps, it follows snapshots
  }
  _scan_shops();
  _hint_until = 0;
  _hint_buf[0] = '\0';
  _hint_col = colour::yellow;
  _chat_until = 0;
  _chat_buf[0] = '\0';
  _chat_pip = false;
  panel::init(); // static panel + minimap terrain, then blips on top
  _hud_first = true; // the menu chrome covered the HUD strip: wipe+repaint it fully
  render::update_camera();
  render::repaint(); // forced: the game over screen cleared the arena and the camera may not move
  _scr = screens::id::playing;
  screens::invalidate(); // the next pause must repaint its chrome
}

void game::_enter_menu() {
  _scr = screens::id::menu;
  _sel = 0;
  _nav_dir = 0;
}

void game::_enter_game_over() {
  const sim::state& v = sim::view();
  // the host owns the wallet and the SD card; a client only mirrors the screen
  if (!_net_multi || _handler.role() == ROLE_HOST) {
    scores::add_run(v.kills, v.points, v.wave); // fold the run: wallet at death, kills, wave
  }
  _scr = screens::id::game_over;
  _sel = 0;
}

void game::_sleep() {
  buzz::stop();
  display::backlight(false);

  rtc_gpio_pullup_en((gpio_num_t)BTN_PAUSE);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PAUSE, 0); // LOW = pressed

  Serial.println("[game] sleeping...");
  Serial.flush();
  esp_deep_sleep_start();
}

// the wrap reads the count from the screen itself, so adding an item cannot leave a
// hardcoded modulus behind
void game::_nav_step() {
  const int8_t e = _nav_edge();
  const uint8_t n = screens::count(_scr);
  if (e && n > 0) {
    _sel = (uint8_t)((_sel + n + e) % n);
    buzz::play(buzz::jingle::menu); // cursor tick on every menu move
  }
}

void game::_update_logo() {
  if (input::fire_pressed() || millis() - _intro_ms0 >= _intro_ms) {
    _scr = screens::id::team; // timed or skippable, then the team screen
    _sel = 0;
    _intro_ms0 = millis();
    buzz::play(buzz::jingle::intro); // re-fire: a skip still hears it from the top
  }
  screens::paint(_scr, _sel);
}

void game::_update_team() {
  if (input::fire_pressed() || input::pause_pressed() || millis() - _intro_ms0 >= _intro_ms) {
    _enter_menu(); // timed like the logo, FIRE/PAUSE skip it
  }
  screens::paint(_scr, _sel);
}

void game::_update_menu() {
  _nav_step();
  if (input::fire_pressed()) {
    buzz::play(buzz::jingle::menu); // confirm blip (inaudible on Exit, it sleeps)
    switch (_sel) {
      case 0:
        _scr = screens::id::mode;
        _sel = 0;
        break;
      case 1:
        _scr = screens::id::scores;
        _sel = 0;
        break;
      default: // Exit
        _sleep();
        break;
    }
  }
  screens::paint(_scr, _sel);
}

void game::_update_mode() {
  _nav_step();
  if (input::fire_pressed()) {
    buzz::play(buzz::jingle::menu);
    if (_sel == 2) { // Back
      _enter_menu();
    } else if (_sel == 0) {
      _start_game(false); // Solo: local run, the radio stays silent
    } else {
      _scr = screens::id::waiting; // Multiplayer: wait for the peer
      _sel = 0;
      _wait_since = millis();
      _wait_last_hb = 0;
      _peer_seen = false;
      _rx_ready = false;
    }
  }
  screens::paint(_scr, _sel);
}

void game::_update_waiting() {
  const uint32_t now = millis();
  if (now - _wait_last_hb >= _wait_hb_ms) {
    heartbeat_msg hb;
    hb.tick = _tick++;
    hb.role = _handler.role();
    _handler.send(&hb, sizeof(hb));
    _wait_last_hb = now;
  }
  if (input::pause_pressed()) {
    _enter_menu(); // bail out
    return;
  }
  if (input::fire_pressed()) {
    buzz::play(buzz::jingle::menu);
    _start_game(false); // tired of waiting: play solo
    return;
  }
  if (_handler.role() == ROLE_CLIENT) {
    // the client joins off a live snapshot (host map), never off a bare heartbeat,
    // so it lands straight into the running frame instead of an empty reset.
    if (_rx_ready) {
      const net::game_state_msg snap = _rx_state; // copy: _start_game clears the flag
      _rx_ready = false;
      _peer_seen = false;
      buzz::play(buzz::jingle::wave);
      _start_game(true);
      sim::apply_snapshot(snap); // start from the live frame, not the reset one
      _cli_last_rx = millis();
      return;
    }
  } else if (_peer_seen) {
    _peer_seen = false;
    buzz::play(buzz::jingle::wave);
    _start_game(true); // peer showed up (a client also joins mid-run off snapshots)
    return;
  }
  if (now - _wait_since >= _wait_ms) {
    buzz::play(buzz::jingle::denied); // nobody out there
    _enter_menu();
    return;
  }
  screens::paint(_scr, _sel);
}

void game::_update_scores() {
  if (input::fire_pressed() || input::pause_pressed()) {
    if (input::fire_pressed()) {
      buzz::play(buzz::jingle::menu);
    }
    _enter_menu();
  }
  screens::paint(_scr, _sel);
}

void game::_update_playing() {
  // solo is always a local sim on both boards (silent radio); only multi + client mirrors.
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

void game::_update_pause() {
  _nav_step();
  // the client keeps shipping inputs while frozen, so derive its PAUSE edge here too:
  // otherwise nobody converts levels to an edge while the host sits in pause and the
  // client can never resume.
  if (_net_multi && _handler.role() == ROLE_HOST) {
    const bool lvl = (_in_buttons & net::pause_bit) != 0;
    _p2_pause_edge = lvl && ((_in_prev & net::pause_bit) == 0);
    _in_prev = _in_buttons;
  }
  if (input::pause_pressed() || _p2_pause_edge) {
    _p2_pause_edge = false;
    _scr = screens::id::playing;
    screens::invalidate(); // the next pause must repaint its chrome
    panel::init(); _hud_first = true; // the pause menu covered the panel and the minimap
    render::repaint(); // clear leftover pause menu
  } else if (input::fire_pressed()) {
    buzz::play(buzz::jingle::menu);
    switch (_sel) {
      case 0: // Continue
        _scr = screens::id::playing;
        screens::invalidate(); // the next pause must repaint its chrome
        panel::init(); _hud_first = true; // the pause menu covered the panel and the minimap
        render::repaint(); // clear leftover pause menu
        break;
      case 1: // Restart
        _start_game(_net_multi); // a co-op pause restarts co-op
        break;
      default: // Exit to Menu
        _enter_menu();
        break;
    }
  }
  screens::paint(_scr, _sel);
  if (_net_multi && _handler.role() == ROLE_HOST) {
    _broadcast(); // frozen sim keeps flowing so the client holds the frame
  }
}

void game::_update_game_over() {
  // solo on either board is a local game over; only a multi client mirrors the host.
  if (_net_multi && _handler.role() == ROLE_CLIENT) {
    // full mirror like the pause chrome: the host owns the cursor, we only paint it
    if (_rx_ready) {
      sim::apply_snapshot(_rx_state);
      _rx_ready = false;
      _cli_last_rx = millis();
      bool anyone = false;
      for (uint8_t p = 0; p < sim::NUM_PLAYERS; ++p) {
        anyone |= sim::view().players[p].active && sim::view().players[p].hp > 0;
      }
      if (anyone) {
        const net::game_state_msg snap = _rx_state; // copy: _start_game clears the flag
        _start_game(true); // host restarted: re-init chrome, snapshots fill the sim
        sim::apply_snapshot(snap); // paint live values on frame 1, never the reset sim
        return;
      }
    }
    if (input::fire_pressed()) {
      buzz::play(buzz::jingle::menu); // feedback only, the host drives restart
    }
    if (millis() - _cli_last_rx >= _cli_quiet_ms) {
      _net_multi = false;
      _enter_menu(); // host left: drop to menu
      return;
    }
    uint8_t s = _rx_state.sel;
    if (s >= screens::count(screens::id::game_over)) {
      s = 0; // corrupt cursor never blanks the chrome (paint guards sel too)
    }
    _sel = s;
    screens::paint(_scr, _sel);
    return;
  }
  _nav_step();
  if (input::fire_pressed()) {
    buzz::play(buzz::jingle::menu);
    if (_sel == 0) {
      _start_game(_net_multi);
    } else {
      _enter_menu();
    }
  }
  screens::paint(_scr, _sel);
  if (_net_multi) {
    _broadcast(); // the over event keeps flowing until the host restarts or exits
  }
}
