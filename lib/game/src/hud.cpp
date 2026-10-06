#include <Arduino.h>
#include <cmath>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <buzz.h>
#include <display.h>
#include <map.h>
#include <panel.h>
#include <render.h>
#include <screens.h>
#include <scores.h>
#include <sim.h>

#include "game.h"
#include "pins.h"

// game hud: the 10px strip, the prompt/callout priority slots, boss watch and
// the buzz dispatch. Text only, never state: buys already moved the wallet.

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
