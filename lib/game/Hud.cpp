// Game HUD: status strip, prompts and Buzz dispatch

#include <Arduino.h>

#include <Buzz.h>
#include <Display.h>
#include <Render.h>
#include <Sim.h>

#include "Game.h"

bool Game::urgent_callout(uint8_t /*me*/) {
  if (net_multi && millis() < chat_until && chat_buf[0] != '\0') {
    Render::prompt(chat_buf, chat_col);
    return true;
  }
  return false;
}

bool Game::help_hint(uint8_t me) {
  if (!net_multi || me > 1) {
    return false;
  }
  const Sim::state& v = Sim::view();
  if (!v.players[me].active || !v.players[me].downed) {
    return false;
  }
  const uint8_t q = (me == 0) ? 1 : 0;
  if (!v.players[q].active || v.players[q].hp == 0) {
    return false;
  }
  const bool cli = (handler.role() == ROLE_CLIENT);
  const bool shouted = (me == 0) ? (cli ? cli_shouted0 : shouted0)
                                 : (cli ? cli_shouted1 : shouted1);
  if (shouted) {
    return false;
  }
  if (shop_at(me) != 0) {
    return false;
  }
  Render::prompt("PRESS INT FOR HELP");
  return true;
}

bool Game::reload_prompt(uint8_t me) {
  if (!Sim::reloading(me)) {
    return false;
  }
  Render::prompt("RELOADING...", Colour::white);
  return true;
}

void Game::fire_buzz() {
  switch (Sim::view().last_event) {
    case Sim::event::shoot: Buzz::play(Buzz::jingle::shoot); break;
    case Sim::event::buy_heal:
    case Sim::event::buy_dmg:
    case Sim::event::buy_spd:
    case Sim::event::buy_rpd: Buzz::play(Buzz::jingle::buy); break;
    case Sim::event::revive: Buzz::play(Buzz::jingle::buy); break;
    case Sim::event::roulette: Buzz::play(Buzz::jingle::roulette); break;
    case Sim::event::denied: Buzz::play(Buzz::jingle::denied); break;
    case Sim::event::reload: Buzz::play(Buzz::jingle::reload); break;
    case Sim::event::hurt: Buzz::play(Buzz::jingle::hurt); break;
    case Sim::event::wave: Buzz::play(Buzz::jingle::wave); break;
    case Sim::event::over: Buzz::play(Buzz::jingle::over); break;
    default: break;
  }
  if (chat_pip) {
    chat_pip = false;
    if (Sim::view().last_event == Sim::event::none) {
      Buzz::play(Buzz::jingle::hurt);
    }
  }
}

bool Game::boss_alive() {
  const Sim::state& v = Sim::view();
  for (uint8_t i = 0; i < Sim::MAX_ZOMBIES; ++i) {
    if (v.zombies[i].active && v.zombies[i].kind == Sim::actor_kind::boss) {
      return true;
    }
  }
  return false;
}

void Game::draw_hud() {
  const Sim::state& v = Sim::view();
  const uint8_t role = !net_multi ? 0 : (handler.role() == ROLE_HOST ? 1 : 2);
  const uint8_t f = Render::focus();
  const uint8_t q = (uint8_t)(1 - f);
  const bool f_dead = v.players[f].active && v.players[f].hp == 0 && !v.players[f].downed;
  const bool q_alive = v.players[q].active && (v.players[q].hp > 0 || v.players[q].downed);
  const uint8_t s = (f_dead && q_alive) ? q : f;
  if (!hud_first && v.wave == hud_wave && v.kills == hud_kills && v.guns[s] == hud_gun &&
      v.ammo[s] == hud_ammo && role == hud_role && s == hud_shown) {
    return;
  }
  hud_wave = v.wave;
  hud_kills = v.kills;
  hud_gun = v.guns[s];
  hud_ammo = v.ammo[s];
  hud_role = role;
  hud_shown = s;
  const int16_t sw = (int16_t)Display::width();
  if (hud_first) {
    hud_first = false;
    Display::fill_rect(0, 0, sw, Render::HUD_H, Colour::black);
  }
  char buf[24];

  Display::fill_rect(0, 0, 124, 8, Colour::black);
  snprintf(buf, sizeof(buf), "WAVE %u KILLS %u", v.wave, v.kills);
  Display::text(buf, 4, 1, Colour::white, 1);

  char gun[16];
  snprintf(gun, sizeof(gun), "GUN %s %u", Sim::gun_name_p(s), v.ammo[s]);
  uint8_t glen = 0;
  while (gun[glen] != '\0') {
    ++glen;
  }
  const int16_t gx = (sw - (int16_t)glen * 6) / 2;
  Display::fill_rect(112, 0, 96, 8, Colour::black);
  Display::text(gun, gx < 0 ? 0 : gx, 1, Colour::white, 1);

  const char* badge = !net_multi ? "SOLO" : (handler.role() == ROLE_HOST ? "P1" : "P2");
  uint8_t blen = 0;
  while (badge[blen] != '\0') {
    ++blen;
  }
  const int16_t bx = sw - 4 - (int16_t)blen * 6;
  Display::fill_rect(sw - 46, 0, 46, 8, Colour::black);
  Display::text(badge, bx < 0 ? 0 : bx, 1, Colour::cyan, 1);
}
