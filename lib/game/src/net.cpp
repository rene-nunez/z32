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

// game net: ESP-NOW handlers, snapshot broadcast, client input shipping and the
// per-board playing frames. Host simulates, client mirrors; solo stays silent.

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
