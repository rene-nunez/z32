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

void game::_on_heartbeat(const uint8_t* data, size_t len) {
  if (len < sizeof(heartbeat_msg)) {
    return;
  }

  heartbeat_msg hb;
  memcpy(&hb, data, sizeof(hb));
  _peer_tick = hb.tick;
  // the host starts a co-op run off heartbeats; the client waits for a snapshot
  // (live map) instead, so heartbeats never flip a waiting client into playing.
  if (_handler.role() == ROLE_HOST) {
    _peer_seen = true; // any peer heartbeat counts while waiting (consumed there)
  }
}

void game::_on_state(const uint8_t* data, size_t len) {
  if (_handler.role() != ROLE_CLIENT) {
    return; // host never applies snapshots
  }
  if (len < sizeof(net::game_state_msg)) {
    return;
  }
  memcpy(&_rx_state, data, sizeof(net::game_state_msg));
  _rx_ready = true;
  _peer_seen = true; // snapshots also join a waiting client, enabling late join
}

void game::_on_input(const uint8_t* data, size_t len) {
  if (_handler.role() != ROLE_HOST) {
    return; // client never applies inputs
  }
  if (len < sizeof(net::player_input_msg)) {
    return;
  }
  net::player_input_msg in;
  memcpy(&in, data, sizeof(in));
  _in_jx = net::uqaxis(in.jx);
  _in_jy = net::uqaxis(in.jy);
  _in_buttons = in.buttons;
  _in_seq = in.seq;
  _in_last_ms = millis();
}

// P2 steel-blue, brightened to read at 8px (same as the panel HP2 row)
static constexpr uint16_t _mate_p2col = 0x54DA;

void game::_on_chat(const uint8_t* data, size_t len) {
  if (len < sizeof(net::chat_msg)) {
    return;
  }
  net::chat_msg m;
  memcpy(&m, data, sizeof(m));
  if (m.from > 1 || m.id != (uint8_t)net::chat_id::come) {
    return; // SAVE ME rides the come wire id; THX derives locally, nothing else travels
  }
  snprintf(_chat_buf, sizeof(_chat_buf), "P%u: SAVE ME!", (unsigned)(m.from + 1u));
  _chat_col = (m.from == 0) ? colour::green : _mate_p2col;
  _chat_until = millis() + 2000;
  _chat_pip = true; // buzzed from the frame loop, never from the rx task
}

// downed INTERACT shout: still the come wire id (no protocol change), now reads SAVE ME.
// The sender also sees its own shout as a local echo (visual only, never buzzed);
// only the peer's rx path sets _chat_pip, so the hurt pip stays remote.
void game::_send_chat(uint8_t from) {
  if (!_net_multi) {
    return; // solo: no peer, and send() with no peer sprays delivery-failed
  }
  net::chat_msg m;
  m.from = from;
  m.id = (uint8_t)net::chat_id::come;
  m.seq = _chat_seq++;
  _handler.send(&m, sizeof(m)); // triple: 4B each, one usually lands
  _handler.send(&m, sizeof(m));
  _handler.send(&m, sizeof(m));
  snprintf(_chat_buf, sizeof(_chat_buf), "P%u: SAVE ME!", (unsigned)(from + 1u));
  _chat_col = (from == 0) ? colour::green : _mate_p2col;
  _chat_until = millis() + 2000;
  // first shout this down: the HELP hint below retires (each board reads its own
  // pair, so set both; the unread one is harmless).
  if (from == 0) {
    _shouted0 = true;
    _cli_shouted0 = true;
  } else {
    _shouted1 = true;
    _cli_shouted1 = true;
  }
}

// urgent slot (above reloading): the voluntary SAVE ME! shout while fresh.
// Both boards see it now (local echo on send + rx on the peer); only the peer
// buzzes (rx sets _chat_pip, the echo never does). Silence otherwise, the panel
// DOWN countdown + yellow dot cover the state.
bool game::_urgent_callout(uint8_t /*me*/) {
  if (_net_multi && millis() < _chat_until && _chat_buf[0] != '\0') {
    render::prompt(_chat_buf, _chat_col); // freshest shout first
    return true;
  }
  return false;
}

// idle downed hint (below the shout, above reloading): PRESS INT FOR HELP stays up
// until the first shout of this down, so a player at 20m with no audio always knows
// how to call. Multi-only with a living partner (same gate as the shout) and only
// where INT really shouts: near a machine INT buys (shop prompts keep priority
// there), so the hint stays off. Yellow like the shop prompts. The fresh shout
// above wins while live, so the first mash swaps to SAVE ME! for 2s.
bool game::_help_hint(uint8_t me) {
  if (!_net_multi || me > 1) {
    return false;
  }
  const sim::state& v = sim::view();
  if (!v.players[me].active || !v.players[me].downed) {
    return false;
  }
  const uint8_t q = (me == 0) ? 1 : 0;
  if (!v.players[q].active || v.players[q].hp == 0) {
    return false; // nobody out there to help
  }
  const bool cli = (_handler.role() == ROLE_CLIENT);
  const bool shouted = (me == 0) ? (cli ? _cli_shouted0 : _shouted0)
                                 : (cli ? _cli_shouted1 : _shouted1);
  if (shouted) {
    return false; // already called once: the echo above covers further mashes
  }
  if (_shop_at(me) != 0) {
    return false; // INT buys here, it does not shout
  }
  render::prompt("PRESS INT FOR HELP"); // default yellow, like GET.../ROLL/REVIVE
  return true;
}

// reload slot (below the urgent shout, above shop): own mag swap FYI, white info.
// Auto starts it the moment the mag hits 0, so this replaces the old OUT OF AMMO nag.
bool game::_reload_prompt(uint8_t me) {
  if (!sim::reloading(me)) {
    return false;
  }
  render::prompt("RELOADING...", colour::white); // bare in solo and multi: only own shows
  return true;
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

void game::_broadcast() {
  _tx_state.type = net::TYPE_STATE;
  _tx_state.seq = _seq_out++;
  sim::snapshot(_tx_state);
  // the menu mirror rides the snapshot: the host owns pause/over chrome, the client paints it
  if (_scr == screens::id::pause) {
    _tx_state.screen = net::SCREEN_PAUSE;
  } else if (_scr == screens::id::game_over) {
    _tx_state.screen = net::SCREEN_OVER;
  } else {
    _tx_state.screen = net::SCREEN_PLAYING;
  }
  _tx_state.sel = _sel;
  _handler.send(&_tx_state, sizeof(_tx_state));
}

uint8_t game::_roulette_active_at(uint16_t w, uint8_t n) {
  if (n == 0 || w == 0) {
    return 0;
  }
  // 3-wave epochs from wave 3 on: stable for waves 1-2, 3-5, 6-8, ... yet host
  // and client (plus late joiners) always derive the same pad with no extra net bytes
  const uint32_t e = (uint32_t)w / 3u;
  const uint32_t h = (e * 1103515245u + 12345u) & 0x7FFFFFFFu;
  return (uint8_t)((h >> 16u) % n);
}

uint8_t game::_roulette_active() {
  return _roulette_active_at(sim::view().wave, _shop_rn);
}

void game::_push_roll_marker() {
  if (_shop_rn == 0) {
    panel::set_roll(-1, -1);
    return;
  }
  const uint8_t act = _roulette_active();
  if (act >= _shop_rn || _shop_rx[act] < 0 || _shop_ry[act] < 0) {
    panel::set_roll(-1, -1);
    return;
  }
  // centres are exact tile multiples ((c+1)*TILE), so the top-left tile is one back
  panel::set_roll((int16_t)(_shop_rx[act] / tilemap::TILE - 1),
                  (int16_t)(_shop_ry[act] / tilemap::TILE - 1));
}

bool game::_roulette_moved() {
  // the banner fires only on a real relocation: never on wave 1, and never when
  // two epochs hash onto the same pad (the wheel did not move, nothing to say)
  const uint16_t w = sim::view().wave;
  if (w <= 1 || _shop_rn == 0) {
    return false;
  }
  return _roulette_active_at(w, _shop_rn) != _roulette_active_at(w - 1u, _shop_rn);
}

void game::_scan_shops() {
  _shop_hx = _shop_hy = -1;
  _shop_dx = _shop_dy = -1;
  _shop_sx = _shop_sy = -1;
  _shop_cx = _shop_cy = -1;
  for (uint8_t i = 0; i < MAX_PADS; ++i) {
    _shop_rx[i] = _shop_ry[i] = -1;
  }
  _shop_rn = 0;
  for (uint8_t r = 0; r < tilemap::ROWS; ++r) {
    for (uint8_t c = 0; c < tilemap::COLS; ++c) {
      const uint8_t t = tilemap::tiles[r][c];
      // top-left tile of the 2x2 block only, so each machine registers once
      if (c > 0 && tilemap::tiles[r][c - 1] == t) {
        continue;
      }
      if (r > 0 && tilemap::tiles[r - 1][c] == t) {
        continue;
      }
      int16_t* ox = nullptr;
      int16_t* oy = nullptr;
      switch (t) {
        case tilemap::VENDING: ox = &_shop_hx; oy = &_shop_hy; break;
        case tilemap::V_DMG: ox = &_shop_dx; oy = &_shop_dy; break;
        case tilemap::V_SPD: ox = &_shop_sx; oy = &_shop_sy; break;
        case tilemap::V_RPD: ox = &_shop_cx; oy = &_shop_cy; break;
        case tilemap::ROULETTE:
          // row-major scan order defines the pad index, shared with render tags:
          // 0 = north, 1 = east, 2 = centre, 3 = SE on the shipped map
          if (_shop_rn < MAX_PADS && _shop_rx[_shop_rn] < 0) {
            ox = &_shop_rx[_shop_rn];
            oy = &_shop_ry[_shop_rn];
          }
          break;
        default: break;
      }
      // first (top-left) tile of the 2x2 block wins, so the centre is one tile in
      if (ox != nullptr && *ox < 0) {
        *ox = (int16_t)(((uint16_t)c + 1u) * tilemap::TILE);
        *oy = (int16_t)(((uint16_t)r + 1u) * tilemap::TILE);
        if (t == tilemap::ROULETTE) {
          ++_shop_rn;
        }
      }
    }
  }
}

bool game::_near_inactive_roulette(uint8_t p) {
  const sim::state& v = sim::view();
  if (p >= sim::NUM_PLAYERS || !v.players[p].active || _shop_rn == 0) {
    return false;
  }
  const uint8_t act = _roulette_active();
  const float pcx = v.players[p].x + sim::PLAYER_SIZE / 2.0f;
  const float pcy = v.players[p].y + sim::PLAYER_SIZE / 2.0f;
  const float r2 = (float)(_shop_r * _shop_r);
  for (uint8_t i = 0; i < _shop_rn; ++i) {
    if (i == act || _shop_rx[i] < 0) {
      continue;
    }
    const float dx = pcx - (float)_shop_rx[i];
    const float dy = pcy - (float)_shop_ry[i];
    if (dx * dx + dy * dy <= r2) {
      return true;
    }
  }
  return false;
}

uint8_t game::_shop_at(uint8_t p) {
  const sim::state& v = sim::view();
  if (p >= sim::NUM_PLAYERS || !v.players[p].active) {
    return 0;
  }
  const float pcx = v.players[p].x + sim::PLAYER_SIZE / 2.0f;
  const float pcy = v.players[p].y + sim::PLAYER_SIZE / 2.0f;
  const float r2 = (float)(_shop_r * _shop_r);
  auto near = [&](int16_t sx, int16_t sy) -> bool {
    if (sx < 0) {
      return false;
    }
    const float dx = pcx - (float)sx;
    const float dy = pcy - (float)sy;
    return dx * dx + dy * dy <= r2;
  };
  if (near(_shop_hx, _shop_hy)) {
    return 1;
  } else if (near(_shop_dx, _shop_dy)) {
    return 2;
  } else if (near(_shop_sx, _shop_sy)) {
    return 3;
  } else if (near(_shop_cx, _shop_cy)) {
    return 5;
  } else if (_shop_rn > 0) {
    const uint8_t act = _roulette_active();
    if (act < _shop_rn && near(_shop_rx[act], _shop_ry[act])) {
      return 4;
    }
  }
  return 0;
}

bool game::_revive_near(uint8_t p) {
  const sim::state& v = sim::view();
  if (p >= sim::NUM_PLAYERS || !v.players[p].active || v.players[p].hp == 0) {
    return false; // only the standing can lift
  }
  const uint8_t q = (p == 0) ? 1 : 0;
  if (!v.players[q].active || !v.players[q].downed) {
    return false;
  }
  const float dx = (v.players[p].x - v.players[q].x);
  const float dy = (v.players[p].y - v.players[q].y);
  return dx * dx + dy * dy <= (float)(_revive_r * _revive_r);
}

bool game::_revive_update(uint32_t now) {
  const sim::state& v = sim::view();
  const bool d0 = v.players[0].active && v.players[0].downed;
  const bool d1 = _net_multi && v.players[1].active && v.players[1].downed;
  // bleed-outs announce once (2s): the strip frees after, the panel keeps the DOWN
  // countdown. Falls stay silent now: the downed shout below + the panel cover it.
  // A lift below overwrites with the risen's THX.
  const bool p0_died = _was_down0 && !d0 && v.players[0].hp == 0;
  const bool p1_died = _was_down1 && !d1 && v.players[1].hp == 0;
  if (p0_died || p1_died) {
    snprintf(_hint_buf, sizeof(_hint_buf), p0_died ? "P1: I'M OUT!" : "P2: I'M OUT!");
    _hint_col = p0_died ? colour::green : _mate_p2col; // the dead one's colour
    _hint_until = now + 2000;
  }
  // bled-out wave rejoins are announced in the wave-banner block below (BACK is
  // the least important of the three wave messages); downed players that held on
  // till the break rise silently (they never bled out). _was_dead advances after
  // the banners so they still see the previous frame.
  if (!_was_down0 && d0) {
    _shouted0 = false; // fall edge: HELP shows again until the first shout
  }
  if (!_was_down1 && d1) {
    _shouted1 = false;
  }
  _was_down0 = d0;
  _was_down1 = d1;
  const bool edge[2] = {input::interact_pressed(), _p2_interact};
  for (uint8_t p = 0; p < sim::NUM_PLAYERS; ++p) {
    if (!edge[p] || !_revive_near(p)) {
      continue;
    }
    const uint8_t q = (p == 0) ? 1 : 0;
    if (sim::revive(q)) {
      snprintf(_hint_buf, sizeof(_hint_buf), p == 0 ? "P2: THX!" : "P1: THX!");
      _hint_col = (p == 0) ? _mate_p2col : colour::green; // the risen one's colour
      _hint_until = now + 1500;
      if (p == 1) {
        _p2_interact = false; // consumed: no accidental buy next frame
      }
      return true;
    }
  }
  return false;
}

void game::_shop_update(uint32_t now) {
  const sim::state& v = sim::view();
  const uint8_t shop = _shop_at(0);
  const bool p2_out = v.players[1].active && v.players[1].hp > 0;
  const uint8_t shop2 = p2_out ? _shop_at(1) : 0;

  if (input::interact_pressed() && shop != 0) {
    bool ok = false;
    switch (shop) {
      case 1:
        ok = sim::buy_heal(now);
        if (ok) {
          snprintf(_hint_buf, sizeof(_hint_buf), "HEALED +2HP");
          _hint_col = colour::green;
        } else {
          snprintf(_hint_buf, sizeof(_hint_buf),
                   v.players[0].hp >= sim::PLAYER_HP_MAX ? "HP FULL" : "NEED %lu PTS",
                   (unsigned long)sim::PRICE_HEAL);
          _hint_col = colour::white; // info, not danger: red never shows in the strip
        }
        break;
      case 2:
        if (v.dmg_lvl[0] >= sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "DMG MAX");
          _hint_col = colour::white;
        } else {
          ok = sim::buy_damage(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "DMG +%u%%!",
                     sim::dmg_bonus(sim::view().dmg_lvl[0]));
            _hint_col = colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS",
                     (unsigned long)sim::price_for(sim::PRICE_DMG, v.dmg_lvl[0]));
            _hint_col = colour::white;
          }
        }
        break;
      case 3:
        if (v.spd_lvl[0] >= sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "SPD MAX");
          _hint_col = colour::white;
        } else {
          ok = sim::buy_speed(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "SPD +%u%%!",
                     sim::spd_bonus(sim::view().spd_lvl[0]));
            _hint_col = colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS",
                     (unsigned long)sim::price_for(sim::PRICE_SPD, v.spd_lvl[0]));
            _hint_col = colour::white;
          }
        }
        break;
      case 5:
        if (v.rpd_lvl[0] >= sim::MAX_LVL) {
          snprintf(_hint_buf, sizeof(_hint_buf), "ROF MAX");
          _hint_col = colour::white;
        } else {
          ok = sim::buy_rapid(now);
          if (ok) {
            snprintf(_hint_buf, sizeof(_hint_buf), "ROF -%u%%!",
                     sim::rpd_cut(sim::view().rpd_lvl[0]));
            _hint_col = colour::green;
          } else {
            snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS",
                     (unsigned long)sim::price_for(sim::PRICE_RPD, v.rpd_lvl[0]));
            _hint_col = colour::white;
          }
        }
        break;
      default:
        ok = sim::roll_roulette(now);
        if (ok) {
          snprintf(_hint_buf, sizeof(_hint_buf), "NEW GUN: %s", sim::gun_name_p(0));
          _hint_col = colour::green;
        } else {
          snprintf(_hint_buf, sizeof(_hint_buf), "NEED %lu PTS", (unsigned long)sim::PRICE_ROLL);
          _hint_col = colour::white;
        }
        break;
    }
    _hint_until = now + 1500;
  }

  // player 2 shops from the shared wallet on its own INTERACT edge (net, F6.3 sets it).
  // silent on the host screen: the buy runs, no P2 prompt/hint is painted here.
  // the client paints its own proximity off the snapshot.
  if (_p2_interact && shop2 != 0) {
    switch (shop2) {
      case 1: sim::buy_heal(now, 1); break;
      case 2: sim::buy_damage(now, 1); break;
      case 3: sim::buy_speed(now, 1); break;
      case 5: sim::buy_rapid(now, 1); break;
      default: sim::roll_roulette(now, 1); break;
    }
  }
  _p2_interact = false; // consumed every frame, edge semantics

  // downed P1 mashing INTERACT with nothing actionable nearby screams SAVE ME
  // (near a machine the buy below still wins, like a standing player)
  if (v.players[0].downed && v.players[1].hp > 0 && input::interact_pressed() &&
      !_revive_near(1) && shop == 0) {
    _send_chat(0);
  }

  if (now < _hint_until && _hint_buf[0] != '\0') {
    render::prompt(_hint_buf, _hint_col); // recent P1 result wins over the prompt
    return;
  }
  if (_revive_near(0)) {
    render::prompt("PRESS INT TO REVIVE"); // INT verb stays yellow
    return;
  }
  if (_urgent_callout(0)) {
    return; // voluntary SAVE ME over the reload nag
  }
  if (_help_hint(0)) {
    return; // PRESS INT FOR HELP until the first shout
  }
  if (_reload_prompt(0)) {
    return; // own mag swap under the shout
  }
  if (shop == 0) {
    // standing on a dead wheel reads as moved, not as silence (P1 view only)
    if (_near_inactive_roulette(0)) {
      render::prompt("NO LUCK HERE", colour::white);
      return;
    }
  }
  _shop_prompt(shop, "", 0);
}

void game::_shop_prompt(uint8_t shop, const char* who, uint8_t p) {
  const sim::state& v = sim::view();
  switch (shop) {
    case 1:
      snprintf(_hint_buf, sizeof(_hint_buf), "%sGET HEAL +2HP", who);
      render::prompt(_hint_buf);
      break;
    case 2:
      if (v.dmg_lvl[p] >= sim::MAX_LVL) {
        render::prompt("DMG MAX", colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET DMG +%u%%", who,
                 sim::dmg_bonus((uint8_t)(v.dmg_lvl[p] + 1u)));
        render::prompt(_hint_buf);
      }
      break;
    case 3:
      if (v.spd_lvl[p] >= sim::MAX_LVL) {
        render::prompt("SPD MAX", colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET SPD +%u%%", who,
                 sim::spd_bonus((uint8_t)(v.spd_lvl[p] + 1u)));
        render::prompt(_hint_buf);
      }
      break;
    case 4:
      snprintf(_hint_buf, sizeof(_hint_buf), "%sGET ROLL 200", who);
      render::prompt(_hint_buf);
      break;
    case 5:
      if (v.rpd_lvl[p] >= sim::MAX_LVL) {
        render::prompt("ROF MAX", colour::white);
      } else {
        snprintf(_hint_buf, sizeof(_hint_buf), "%sGET ROF -%u%%", who,
                 sim::rpd_cut((uint8_t)(v.rpd_lvl[p] + 1u)));
        render::prompt(_hint_buf);
      }
      break;
    default: render::prompt(nullptr); break;
  }
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

// one jingle per sim event, fired after step()+shop so buys win over same-frame shots
void game::_fire_buzz() {
  switch (sim::view().last_event) {
    case sim::event::shoot: buzz::play(buzz::jingle::shoot); break;
    case sim::event::buy_heal:
    case sim::event::buy_dmg:
    case sim::event::buy_spd:
    case sim::event::buy_rpd: buzz::play(buzz::jingle::buy); break;
    case sim::event::revive: buzz::play(buzz::jingle::buy); break; // a lift, not a purchase
    case sim::event::roulette: buzz::play(buzz::jingle::roulette); break;
    case sim::event::denied: buzz::play(buzz::jingle::denied); break;
    case sim::event::empty: buzz::play(buzz::jingle::denied); break; // legacy dry (auto rescues now)
    case sim::event::reload: buzz::play(buzz::jingle::reload); break; // mag swap
    case sim::event::hurt: buzz::play(buzz::jingle::hurt); break;
    case sim::event::wave: buzz::play(buzz::jingle::wave); break;
    case sim::event::over: buzz::play(buzz::jingle::over); break;
    default: break; // none: silence
  }
  if (_chat_pip) {
    _chat_pip = false;
    if (sim::view().last_event == sim::event::none) {
      buzz::play(buzz::jingle::hurt); // remote SAVE ME, only over silence
    }
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

void game::_update_playing_host() {
  // fold the latest peer input into player 2 (edges from levels, neutral when stale)
  const uint32_t now = millis();
  sim::ctl c = {};
  if (_net_multi && now - _in_last_ms < _in_stale_ms) {
    c.jx = _in_jx;
    c.jy = _in_jy;
    // P2 fire mirrors the local rule: level for autos (MP9/AR-15), edge for the rest
    const bool p2_lvl = (_in_buttons & net::fire_bit) != 0;
    c.fire = sim::auto_fire(sim::view().guns[1]) ? p2_lvl
                                                 : (p2_lvl && !(_in_prev & net::fire_bit));
    _p2_interact = (_in_buttons & net::interact_bit) && !(_in_prev & net::interact_bit);
    _p2_pause_edge = (_in_buttons & net::pause_bit) && !(_in_prev & net::pause_bit);
    c.reload = (_in_buttons & net::reload_bit) && !(_in_prev & net::reload_bit);
    _in_prev = _in_buttons;
  } else {
    _in_prev = _in_buttons; // stale: hold levels so the next packet re-edges cleanly
  }
  c.pause = false; // pause travels via _p2_pause_edge, sim never sees it
  sim::set_p2(c);

  if (input::pause_pressed() || _p2_pause_edge) {
    _p2_pause_edge = false;
    _scr = screens::id::pause;
    _sel = 0;
    return;
  }
  _p2_pause_edge = false;

  // the order matters: the clear-before-sim is what erases entities that die mid-frame
  render::clear();
  if (!sim::step(now)) {
    buzz::play(buzz::jingle::over); // death jingle, then the screen change below
    _enter_game_over(); // sim reports the death, the screen change belongs here
    if (_net_multi) {
      _broadcast(); // the client mirrors game over off the over event
    }
    return;
  }
  // pre-revive downed edges: the wave banner below needs them (held-on rises),
  // but _revive_update advances _was_down first
  const bool was_d0 = _was_down0;
  const bool was_d1 = _was_down1;
  if (_revive_update(now)) {
    render::prompt(_hint_buf, _hint_col); // lift result now, shop waits a frame
  } else {
    _shop_update(now); // INTERACT buys + prompt, before the panel paints it
  }
  if (sim::view().last_event == sim::event::wave) {
    if (_boss_alive()) {
      // boss waves announce over the proximity prompt: buys still win, proximity waits
      snprintf(_hint_buf, sizeof(_hint_buf), "BOSS WAVE!");
      _hint_col = colour::purple;
      _hint_until = now + 2000;
      render::prompt(_hint_buf, _hint_col);
    } else if (_roulette_moved() && now >= _hint_until) {
      // the wheel really relocated: announce it over the proximity prompt once
      snprintf(_hint_buf, sizeof(_hint_buf), "ROLL RELOCATED!");
      _hint_col = colour::yellow; // explicit: _shop_update repaints the buf with this
      _hint_until = now + 2000;
      render::prompt(_hint_buf);
    } else if (now >= _hint_until) {
      // wave rejoins, least important of the three: only when the strip is
      // free (fresher hints already won above). Bled-out rejoins and downed
      // players that held on till the break both announce; a same-frame lift
      // already overwrote the wave event (THX wins), so this only sees rises.
      // Ties name the bled-out first: the bigger news wins the single strip.
      const sim::state& wv = sim::view();
      const bool d0 = wv.players[0].active && wv.players[0].downed;
      const bool d1 = _net_multi && wv.players[1].active && wv.players[1].downed;
      const bool p0_back = _was_dead0 && wv.players[0].hp > 0;
      const bool p1_back = _was_dead1 && wv.players[1].hp > 0;
      const bool p0_held = was_d0 && !d0 && wv.players[0].hp > 0;
      const bool p1_held = was_d1 && !d1 && wv.players[1].hp > 0;
      if (p0_back || p1_back || p0_held || p1_held) {
        const bool p1 = p0_back || (!p1_back && p0_held);
        snprintf(_hint_buf, sizeof(_hint_buf), p1 ? "P1: I'M BACK!" : "P2: I'M BACK!");
        _hint_col = p1 ? colour::green : _mate_p2col; // the returner's colour
        _hint_until = now + 1500;
        render::prompt(_hint_buf, _hint_col);
      }
    }
  }
  {
    // _was_dead advances after the banners so they still see the previous frame
    const sim::state& wv = sim::view();
    const bool d0 = wv.players[0].active && wv.players[0].downed;
    const bool d1 = _net_multi && wv.players[1].active && wv.players[1].downed;
    _was_dead0 = wv.players[0].active && wv.players[0].hp == 0 && !d0;
    _was_dead1 = _net_multi && wv.players[1].active && wv.players[1].hp == 0 && !d1;
  }
  _fire_buzz(); // jingle for the frame's event (revive/buys already overwrote shots)
  _push_roll_marker(); // active pad to the minimap, moves on epoch change
  render::update_camera();
  render::draw();
  panel::draw();
  panel::blips();
  _draw_hud(); // wave/kills + gun + role, cached (repaints only on change)
  if (_net_multi) {
    _broadcast(); // ~30Hz state to the client, right after the frame simmed
  }
}

void game::_update_playing_client() {
  _send_input();

  // route off the last snapshot before touching the frame: while the host-owned menu is
  // up, clear() would spray terrain erases over it, so mirror frames skip clear/draw.
  if (_rx_state.screen == net::SCREEN_PAUSE) {
    _mirror_pause(); // _scr stays playing so update() keeps routing here
    return;
  }
  if (_cli_mirror) {
    // first playing frame after the menu: its chrome covered the arena, so rebuild it
    // exactly like the host resume path (progressive terrain over the next 2 frames).
    _cli_mirror = false;
    screens::invalidate(); // the next pause must repaint its chrome
    panel::init(); _hud_first = true;         // the pause menu covered the panel and the minimap
    render::repaint();     // clear leftover pause menu
  }

  // erase at the old frame, then step to the new one: same order as the host
  render::clear();
  if (_rx_ready) {
    sim::apply_snapshot(_rx_state);
    _rx_ready = false;
    _cli_last_rx = millis();
  }
  if (millis() - _cli_last_rx >= _cli_quiet_ms) {
    buzz::play(buzz::jingle::denied); // host went away (menu/sleep): drop to menu
    _net_multi = false;
    _enter_menu();
    return;
  }
  if (sim::view().last_event == sim::event::over) {
    _enter_game_over(); // host declared it, we only mirror (no SD write, see guard)
    return;
  }
  const sim::state& cv = sim::view();
  const bool c1_down = cv.players[0].active && cv.players[0].downed;
  const bool c2_down = cv.players[1].active && cv.players[1].downed;
  // edge news: the hint text never travels in the snapshot, so the client
  // derives lift/death edges itself. A wave respawn also moves bodies, but that
  // frame carries the wave event or a wave-number rise (BACK/banner wins below),
  // and a host restart drops the wave, so only lone edges land here. Priority
  // per frame: lift result, then the bleed-out; ties name the local body. Falls
  // stay silent (except arming HELP below, which shows until the first shout).
  bool edge_hint = false;
  // pre-edge downed flags: the wave banner below needs them (held-on rises),
  // but the chain advances _cli_was_down first
  const bool was_c1_down = _cli_was_down0;
  const bool was_c2_down = _cli_was_down1;
  // the wave event lives a single host frame: if that snapshot is skipped (two
  // arrivals between client frames), the next one still carries the higher wave
  // number, so the banner below only slips a frame instead of being lost. A rise
  // with a stamped event (lift/buy overwrote wave on the host) is not a wave
  // frame: the host skipped its banners too, and the edges below still run.
  const bool wave_rose = (cv.wave > _cli_wave);
  const bool wave_frame = (cv.last_event == sim::event::wave) ||
                          (wave_rose && cv.last_event == sim::event::none);
  if (!_cli_was_down0 && c1_down) {
    _cli_shouted0 = false; // fall edge: HELP shows again until the first shout
  }
  if (!_cli_was_down1 && c2_down) {
    _cli_shouted1 = false;
  }
  if (cv.wave < _cli_wave) {
    _cli_was_down0 = c1_down; // host restarted: resync, no announcement
    _cli_was_down1 = c2_down;
  } else if (!wave_frame) {
    // hp gate: bleeding out also clears downed (hp stays 0, dead till the wave),
    // only a real lift comes back with hp. Without it the death reads as a revive.
    // Bleed-outs announce once (2s); the strip frees after.
    const bool p1_rose = _cli_was_down0 && !c1_down && cv.players[0].hp > 0;
    const bool p2_rose = _cli_was_down1 && !c2_down && cv.players[1].hp > 0;
    const bool p1_died = _cli_was_down0 && !c1_down && cv.players[0].hp == 0;
    const bool p2_died = _cli_was_down1 && !c2_down && cv.players[1].hp == 0;
    const uint32_t now_rx = millis();
    if (p2_rose || p1_rose) {
      snprintf(_hint_buf, sizeof(_hint_buf), p2_rose ? "P2: THX!" : "P1: THX!");
      _hint_col = p2_rose ? _mate_p2col : colour::green; // the risen one's colour
      _hint_until = now_rx + 1500;
      edge_hint = true; // skip proximity below: _shop_prompt reuses _hint_buf as scratch
    } else if (p2_died || p1_died) {
      snprintf(_hint_buf, sizeof(_hint_buf), p2_died ? "P2: I'M OUT!" : "P1: I'M OUT!");
      _hint_col = p2_died ? _mate_p2col : colour::green; // the dead one's colour
      _hint_until = now_rx + 2000;
      edge_hint = true;
    }
    _cli_was_down0 = c1_down;
    _cli_was_down1 = c2_down;
    // _cli_was_dead advances after the banners below, like the host pair
  } else {
    _cli_was_down0 = c1_down;
    _cli_was_down1 = c2_down;
  }
  _cli_wave = cv.wave;
  // downed P2 mashing INTERACT with nothing actionable nearby screams SAVE ME
  // (near a machine the host-side buy still wins, like a standing player)
  const uint8_t cshop = _shop_at(1);
  if (cv.players[1].downed && cv.players[0].hp > 0 && input::interact_pressed() &&
      !_revive_near(0) && cshop == 0) {
    _send_chat(1);
  }
  if (edge_hint) {
    render::prompt(_hint_buf, _hint_col); // edge news wins over proximity
  } else if (_revive_near(1)) {
    render::prompt("PRESS INT TO REVIVE"); // INT verb stays yellow
  } else if (_urgent_callout(1)) {
    ; // voluntary SAVE ME over the reload nag
  } else if (_help_hint(1)) {
    ; // PRESS INT FOR HELP until the first shout
  } else if (_reload_prompt(1)) {
    ; // own mag swap under the shout
  } else {
    if (cshop == 0 && _near_inactive_roulette(1)) {
      render::prompt("NO LUCK HERE", colour::white);
    } else {
      _shop_prompt(cshop, "", 1);
    }
  }
  const uint32_t cli_now = millis();
  if (wave_frame) {
    if (_boss_alive()) {
      snprintf(_hint_buf, sizeof(_hint_buf), "BOSS WAVE!"); // 2s locally, like the host hint
      _hint_col = colour::purple;
      _hint_until = cli_now + 2000;
    } else if (_roulette_moved() && cli_now >= _hint_until) {
      snprintf(_hint_buf, sizeof(_hint_buf), "ROLL RELOCATED!");
      _hint_col = colour::yellow; // explicit: painted below with this, no stale boss purple
      _hint_until = cli_now + 2000;
    } else if (cli_now >= _hint_until) {
      // wave rejoins, least important of the three: only when the strip is
      // free. Bled-out rejoins and held-on rises both announce (ties name the
      // bled-out first); mirrors the host banner.
      const sim::state& wv = sim::view();
      const bool p1_back = _cli_was_dead0 && wv.players[0].hp > 0;
      const bool p2_back = _cli_was_dead1 && wv.players[1].hp > 0;
      const bool p1_held = was_c1_down && !c1_down && wv.players[0].hp > 0;
      const bool p2_held = was_c2_down && !c2_down && wv.players[1].hp > 0;
      if (p1_back || p2_back || p1_held || p2_held) {
        const bool p2 = p2_back || (!p1_back && p2_held);
        snprintf(_hint_buf, sizeof(_hint_buf), p2 ? "P2: I'M BACK!" : "P1: I'M BACK!");
        _hint_col = p2 ? _mate_p2col : colour::green; // the returner's colour
        _hint_until = cli_now + 1500;
      }
    }
  }
  {
    // _cli_was_dead advances after the banners so they still see the previous frame
    const sim::state& wv = sim::view();
    const bool d0 = wv.players[0].active && wv.players[0].downed;
    const bool d1 = wv.players[1].active && wv.players[1].downed;
    _cli_was_dead0 = wv.players[0].active && wv.players[0].hp == 0 && !d0;
    _cli_was_dead1 = wv.players[1].active && wv.players[1].hp == 0 && !d1;
  }
  if (cli_now < _hint_until && _hint_buf[0] != '\0') {
    render::prompt(_hint_buf, _hint_col); // recent boss/roll/BACK wave wins over the proximity prompt
  }
  _fire_buzz(); // the snapshot carries the event, so both buzzers sing
  _push_roll_marker(); // active pad to the minimap, same epoch math as the host
  render::update_camera();
  render::draw();
  panel::draw();
  panel::blips();
  _draw_hud();
}

bool game::_boss_alive() {
  const sim::state& v = sim::view();
  for (uint8_t i = 0; i < sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active && v.zombies[i].kind == sim::actor_kind::boss) {
      return true;
    }
  }
  return false;
}

void game::_draw_hud() {
  // the 10px strip is net+sim state, not renderer state, so game paints it: wave/kills
  // left, gun+current-mag centred, role badge right. Every field is cleared first (K12 -> K9 and
  // MP9 30/30 -> MP9 9/30 shrink, overpainting alone would leave ghost digits behind).
  // cached: wave/kills/gun/ammo/role barely change, so most frames skip all three
  // fill+text pairs (~2.6ms). _hud_first forces a full-strip wipe + repaint after
  // menu chrome covered the strip (render::repaint no longer wipes it, so camera
  // cuts never dirty the cache).
  const sim::state& v = sim::view();
  const uint8_t role = !_net_multi ? 0 : (_handler.role() == ROLE_HOST ? 1 : 2);
  const uint8_t f = render::focus(); // this board's gun: solo/host P1, client P2
  // spectator: bled-out (dead till the wave, not downed) watches the living
  // partner's gun till the respawn; downed keeps its own (it rises with it)
  const uint8_t q = (uint8_t)(1 - f);
  const bool f_dead = v.players[f].active && v.players[f].hp == 0 && !v.players[f].downed;
  const bool q_alive =
      v.players[q].active && (v.players[q].hp > 0 || v.players[q].downed);
  const uint8_t s = (f_dead && q_alive) ? q : f;
  if (!_hud_first && v.wave == _hud_wave && v.kills == _hud_kills && v.guns[s] == _hud_gun &&
      v.ammo[s] == _hud_ammo && role == _hud_role && s == _hud_shown) {
    return;
  }
  _hud_wave = v.wave;
  _hud_kills = v.kills;
  _hud_gun = v.guns[s];
  _hud_ammo = v.ammo[s];
  _hud_role = role;
  _hud_shown = s;
  const int16_t sw = (int16_t)display::width();
  if (_hud_first) {
    _hud_first = false;
    display::fill_rect(0, 0, sw, render::HUD_H, colour::black); // menu leftovers, incl. gaps
  }
  char buf[24];

  display::fill_rect(0, 0, 124, 8, colour::black);
  snprintf(buf, sizeof(buf), "WAVE %u KILLS %u", v.wave, v.kills);
  display::text(buf, 4, 1, colour::white, 1);

  char gun[16]; // "GUN GLOCK-19 15" is 15 chars: longest name (8) + current mag
  snprintf(gun, sizeof(gun), "GUN %s %u", sim::gun_name_p(s), v.ammo[s]);
  uint8_t glen = 0;
  while (gun[glen] != '\0') {
    ++glen;
  }
  const int16_t gx = (sw - (int16_t)glen * 6) / 2;
  display::fill_rect(112, 0, 96, 8, colour::black);
  display::text(gun, gx < 0 ? 0 : gx, 1, colour::white, 1);

  const char* badge = !_net_multi ? "SOLO" : (_handler.role() == ROLE_HOST ? "P1" : "P2");
  uint8_t blen = 0;
  while (badge[blen] != '\0') {
    ++blen;
  }
  const int16_t bx = sw - 4 - (int16_t)blen * 6; // same 4px margin as the left field
  display::fill_rect(sw - 46, 0, 46, 8, colour::black);
  display::text(badge, bx < 0 ? 0 : bx, 1, colour::cyan, 1);
}

void game::_send_input() {
  // ship our sticks and buttons every frame; the host owns the sim
  net::player_input_msg in;
  in.jx = net::qaxis(input::jx());
  in.jy = net::qaxis(input::jy());
  in.buttons = (input::fire_down() ? net::fire_bit : 0) |
               (input::reload_down() ? net::reload_bit : 0) |
               (input::interact_down() ? net::interact_bit : 0) |
               (input::pause_down() ? net::pause_bit : 0);
  in.seq = (uint8_t)_seq_out++;
  _handler.send(&in, sizeof(in));
}

void game::_mirror_pause() {
  // Host-owned menu on the client: keep shipping inputs (PAUSE resumes from either
  // board), paint the host cursor, never step it locally. _scr stays playing so
  // update() keeps routing here; the chrome entry is tracked by screens::paint itself.
  // No buzz here: the frozen snapshot event would otherwise jingle every frame.
  _send_input();

  if (_rx_ready) {
    sim::apply_snapshot(_rx_state);
    _rx_ready = false;
    _cli_last_rx = millis();
  }
  if (millis() - _cli_last_rx >= _cli_quiet_ms) {
    _cli_mirror = false;
    _net_multi = false;
    _enter_menu(); // host left: drop to menu
    return;
  }
  if (_rx_state.screen == net::SCREEN_PLAYING) {
    // host resumed or restarted: rebuild the arena chrome like the host resume path
    _cli_mirror = false;
    screens::invalidate(); // the next pause must repaint its chrome
    panel::init(); _hud_first = true;         // the pause menu covered the panel and the minimap
    render::repaint();     // clear leftover pause menu
    return;
  }
  if (_rx_state.screen == net::SCREEN_OVER) {
    _cli_mirror = false;
    _enter_game_over(); // host declared it, we only mirror (no SD write, see guard)
    return;
  }
  uint8_t s = _rx_state.sel;
  if (s >= screens::count(screens::id::pause)) {
    s = 0; // corrupt/clamped cursor never blanks the chrome (paint guards sel)
  }
  _cli_mirror = true; // the arena chrome is covered until the one-shot exit above
  screens::paint(screens::id::pause, s);
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
