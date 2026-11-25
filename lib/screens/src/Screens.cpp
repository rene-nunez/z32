#include <Arduino.h>
#include <cstring>

#include <Display.h>
#include <Scores.h>
#include <Sim.h>

#include "Screens.h"

namespace {
  // menu rows: the full paint and the cursor repaint must agree on where a row is, so both
  // read these. item takes an absolute y; nothing may add the row pitch twice
  // The chrome sits centred: title at 64, first row at 96 on the 240px glass. Items are
  // size 2 (12x16 glyphs, 24px pitch); hints stay size 1
  constexpr int16_t menu_x = 16, menu_y = 96, menu_row = 24;
  constexpr int16_t over_x = 24, over_y = 128; // the game over screen is indented

  const char* const menu_items[] = { "Start Game", "Scores", "Exit" };
  const char* const mode_items[] = { "Solo", "Multiplayer", "Back" };
  const char* const pause_items[] = { "Continue", "Restart", "Exit to Menu" };
  const char* const over_items[] = { "Restart", "Menu" };
  // ASCII only: the TFT font has no glyphs for accents or ñ
  const char* const team_names[] = {
    "Rene Nunez", "David Fajardo", "Angel Terriquez", "Mario Vargas", "Victor Castillo",
  };

  struct list {
    const char* title; // null when the screen has no chrome at all, i.e. playing
    const char* const* items;
    uint8_t count;
    bool indented; // game over sits further right
    uint16_t title_col; // screen identity in the zombie palette
    const char* hint; // bottom-anchored footer verbs, null when the screen has none
  };

  // indexed by Screens::id, so the order here is the enum order
  const list tables[] = {
    {"z32", nullptr, 0, false, Colour::red, nullptr},  // logo (custom big paint, no items)
    {"TEAM", nullptr, 0, false, Colour::lime, nullptr}, // team (custom name list, no items)
    {"z32", menu_items, 3, false, Colour::lime, "JOY: move   FIRE: select"}, // menu
    {"GAME MODE", mode_items, 3, false, Colour::lime, "JOY: move   FIRE: select"}, // mode
    {"SCORES", nullptr, 0, false, Colour::lime, nullptr}, // scores (run history)
    {nullptr, nullptr, 0, false, Colour::black, nullptr}, // playing
    {"PAUSED", pause_items, 3, false, Colour::lime, "JOY: move   FIRE: select   PAUSE: resume"}, // pause
    {"GAME OVER", over_items, 2, true, Colour::red, "JOY: move   FIRE: select"}, // game over
    {"WAITING", nullptr, 0, false, Colour::lime, nullptr}, // waiting (custom peer text, no items)
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

  uint8_t painted_scr = 0xFF; // screen the chrome was last painted for, 0xFF = none
  uint8_t painted_sel = 0;    // selection currently on screen

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
    // footer pinned to the glass bottom (not glued to the list), points-style verbs;
    // always at the menu x so every screen lines its hint up the same way
    Display::text(t.hint, menu_x, 224, Colour::gray, 1);
  }

  void full_menu(const list& t, uint8_t sel) {
    background(t.title, t.title_col);
    list_rows(t, sel);
  }

  void full_logo() {
    Display::fill_rect(0, 0, Display::width(), Display::height(), Colour::black);
    // "z32" huge and centred: 3 glyphs of 6x8 at size 6 -> 108x48
    constexpr uint8_t size = 6;
    Display::text("z32", (Display::width() - 3 * 6 * size) / 2, (Display::height() - 8 * size) / 2, Colour::red, size);
  }

  void full_team() {
    // vertically centred block: title at 48, names at 80..176, all in 48..192 (centre 120)
    Display::fill_rect(0, 0, Display::width(), Display::height(), Colour::black);
    const char* title = "TEAM";
    Display::text(title, (Display::width() - 6 * (int16_t)strlen(title) * 2) / 2, 48, Colour::lime, 2);

    constexpr uint8_t names = sizeof(team_names) / sizeof(team_names[0]);
    for (uint8_t i = 0; i < names; ++i) {
      const int16_t w = 12 * (int16_t)strlen(team_names[i]);
      Display::text(team_names[i], (Display::width() - w) / 2, 80 + (int16_t)i * menu_row, Colour::white, 2);
    }
    // no footer: both intro screens advance alone (FIRE just hurries them)
  }

  void full_scores() {
    background("SCORES", Colour::lime);

    char buf[32];

    // last-4 runs, recent first, ranked gold/silver/bronze/gray. Empty state keeps the
    // "no runs yet" placeholder; once runs land, that slot becomes the section header
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

    // single stats line, size 2 and centred (same P/W/K shorthand as the points list)
    char buf[48];
    const Sim::state& v = Sim::view();
    snprintf(buf, sizeof(buf), "P:%lu W:%u K:%u", v.points, v.wave, v.kills);
    uint8_t len = 0;
    while (buf[len] != '\0') {
      ++len;
    }
    const int16_t tw = (int16_t)len * 12; // size-2 glyphs are 12px wide
    const int16_t sx = ((int16_t)Display::width() - tw) / 2;
    Display::text(buf, sx < 0 ? 0 : sx, 96, Colour::white, 2);

    list_rows(table(Screens::id::game_over), sel);
  }
} // namespace

uint8_t Screens::count(id scr) {
  return table(scr).count;
}

void Screens::invalidate() {
  painted_scr = 0xFF;
}

// A full-screen fill is 320*240*2 = 153600 bytes, ~31ms of SPI at 40MHz, so repainting it every
// frame both blew the 33ms budget and tore against the panel scan-out: that was the line
// sweeping corner to corner. Paint the chrome once per screen entry, then only the two cursor
// lines when the selection moves
void Screens::paint(id scr, uint8_t sel) {
  const list& t = table(scr);
  if (!t.title) {
    return; // playing draws the arena, not a menu
  }
  if (t.count > 0) {
    sel %= t.count; // a stale cursor wraps like the nav instead of blanking the chrome
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
    return; // nothing moved, or a chrome screen with no cursor (logo/team/points)
  }

  item(t.items, painted_sel, false, row_x(t), row_y0(t) + (int16_t)painted_sel * menu_row);
  item(t.items, sel, true, row_x(t), row_y0(t) + (int16_t)sel * menu_row);
  painted_sel = sel;
}
