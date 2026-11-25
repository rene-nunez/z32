// Menu painting: full on entry, cursor lines on change

#include <cstdio>
#include <cstring>

#include <Display.h>
#include <Scores.h>
#include <Sim.h>

#include "Screens.h"

namespace {
  constexpr int16_t menu_x = 16, menu_y = 96, menu_row = 24;
  constexpr int16_t over_x = 24, over_y = 128;

  const char* const menu_items[] = { "Start Game", "Scores", "Exit" };
  const char* const mode_items[] = { "Solo", "Multiplayer", "Back" };
  const char* const pause_items[] = { "Continue", "Restart", "Exit to Menu" };
  const char* const over_items[] = { "Restart", "Menu" };
  // ASCII only: the TFT font has no glyphs for accents or ñ
  const char* const team_names[] = {
    "Rene Nunez", "David Fajardo", "Angel Terriquez", "Mario Vargas", "Victor Castillo",
  };

  struct list {
    const char* title;
    const char* const* items;
    uint8_t count;
    bool indented;
    uint16_t title_col;
    const char* hint;
  };

  // Indexed by Screens::id; order must match the enum
  const list tables[] = {
    {"z32", nullptr, 0, false, Colour::red, nullptr},
    {"TEAM", nullptr, 0, false, Colour::lime, nullptr},
    {"z32", menu_items, 3, false, Colour::lime, "JOY: move   FIRE: select"},
    {"GAME MODE", mode_items, 3, false, Colour::lime, "JOY: move   FIRE: select"},
    {"SCORES", nullptr, 0, false, Colour::lime, nullptr},
    {nullptr, nullptr, 0, false, Colour::black, nullptr},
    {"PAUSED", pause_items, 3, false, Colour::lime, "JOY: move   FIRE: select   PAUSE: resume"},
    {"GAME OVER", over_items, 2, true, Colour::red, "JOY: move   FIRE: select"},
    {"WAITING", nullptr, 0, false, Colour::lime, nullptr},
  };
  static_assert(sizeof(tables) / sizeof(tables[0]) == 9, "one row per Screens::id");

  const list& table(Screens::id scr) {
    return tables[(uint8_t)scr];
  }

  int16_t row_x(const list& t) {
    return t.indented ? over_x : menu_x;
  }

  int16_t row_y0(const list& t) {
    return t.indented ? over_y : menu_y;
  }

  uint8_t painted_scr = 0xFF;
  uint8_t painted_sel = 0;

  void item(const char* const* items, uint8_t i, bool selected, int16_t x, int16_t y) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%c %s", selected ? '>' : ' ', items[i]);
    Display::text(buf, x, y, selected ? Colour::lime : Colour::gray, 2);
  }

  void background(const char* title, uint16_t col) {
    Display::fill_rect(0, 0, Display::width(), Display::height(), Colour::black);
    Display::text(title, (Display::width() - 6 * (int16_t)strlen(title) * 2) / 2, 64, col, 2);
  }

  void list_rows(const list& t, uint8_t sel) {
    for (uint8_t i = 0; i < t.count; ++i) {
      item(t.items, i, i == sel, row_x(t), row_y0(t) + (int16_t)i * menu_row);
    }
    Display::text(t.hint, menu_x, 224, Colour::gray, 1);
  }

  void full_menu(const list& t, uint8_t sel) {
    background(t.title, t.title_col);
    list_rows(t, sel);
  }

  void full_logo() {
    Display::fill_rect(0, 0, Display::width(), Display::height(), Colour::black);
    constexpr uint8_t size = 6;
    Display::text("z32", (Display::width() - 3 * 6 * size) / 2, (Display::height() - 8 * size) / 2, Colour::red, size);
  }

  void full_team() {
    Display::fill_rect(0, 0, Display::width(), Display::height(), Colour::black);
    const char* title = "TEAM";
    Display::text(title, (Display::width() - 6 * (int16_t)strlen(title) * 2) / 2, 48, Colour::lime, 2);

    constexpr uint8_t names = sizeof(team_names) / sizeof(team_names[0]);
    for (uint8_t i = 0; i < names; ++i) {
      const int16_t w = 12 * (int16_t)strlen(team_names[i]);
      Display::text(team_names[i], (Display::width() - w) / 2, 80 + (int16_t)i * menu_row, Colour::white, 2);
    }
  }

  void full_scores() {
    background("SCORES", Colour::lime);

    char buf[32];

    static const uint16_t rank_col[] = {Colour::yellow, Colour::white, Colour::orange, Colour::gray};
    const uint8_t n = Scores::history_len();
    const Scores::run* h = Scores::history();
    if (n == 0) {
      Display::text("no runs yet", 16, 100, Colour::gray, 2);
      return;
    }
    Display::text("last 4 rounds", 16, 100, Colour::gray, 2);
    for (uint8_t i = 0; i < n && i < Scores::HISTORY_N; ++i) {
      snprintf(buf, sizeof(buf), "R%u P%lu K%lu W%u", i + 1, h[i].pts, h[i].kills, h[i].wave);
      Display::text(buf, 16, (int16_t)(124 + i * 24), rank_col[i], 2);
    }

    Display::text("FIRE/PAUSE: back", 16, 224, Colour::gray, 1);
  }

  void full_waiting() {
    background("WAITING", Colour::lime);
    Display::text("for peer ...", 16, 100, Colour::white, 2);
    Display::text("FIRE: solo   PAUSE: back", 16, 140, Colour::gray, 2);
  }

  void full_game_over(uint8_t sel) {
    background("GAME OVER", Colour::red);

    char buf[48];
    const Sim::state& v = Sim::view();
    snprintf(buf, sizeof(buf), "P:%lu W:%u K:%u", v.points, v.wave, v.kills);
    uint8_t len = 0;
    while (buf[len] != '\0') {
      ++len;
    }
    const int16_t tw = (int16_t)len * 12;
    const int16_t sx = ((int16_t)Display::width() - tw) / 2;
    Display::text(buf, sx < 0 ? 0 : sx, 96, Colour::white, 2);

    list_rows(table(Screens::id::game_over), sel);
  }
}

uint8_t Screens::count(id scr) {
  return table(scr).count;
}

void Screens::invalidate() {
  painted_scr = 0xFF;
}

void Screens::paint(id scr, uint8_t sel) {
  const list& t = table(scr);
  if (!t.title) {
    return;
  }
  if (t.count > 0) {
    sel %= t.count;
  }

  if (painted_scr != (uint8_t)scr) {
    switch (scr) {
      case id::logo: full_logo(); break;
      case id::team: full_team(); break;
      case id::scores: full_scores(); break;
      case id::game_over: full_game_over(sel); break;
      case id::waiting: full_waiting(); break;
      default: full_menu(t, sel); break;
    }
    painted_scr = (uint8_t)scr;
    painted_sel = sel;
    return;
  }

  if (sel == painted_sel || t.count == 0) {
    return;
  }

  item(t.items, painted_sel, false, row_x(t), row_y0(t) + (int16_t)painted_sel * menu_row);
  item(t.items, sel, true, row_x(t), row_y0(t) + (int16_t)sel * menu_row);
  painted_sel = sel;
}
