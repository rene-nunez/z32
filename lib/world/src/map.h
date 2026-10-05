#pragma once

#include <cstdint>

namespace tilemap {
  constexpr uint8_t COLS = 60;
  constexpr uint8_t ROWS = 30;
  constexpr uint8_t TILE = 16;
  constexpr uint16_t WORLD_W = COLS * TILE;
  constexpr uint16_t WORLD_H = ROWS * TILE;

  constexpr uint8_t FLOOR = 0;  // meadow, walkable (the only walkable tile)
  constexpr uint8_t WALL = 1;   // maze wall, solid
  constexpr uint8_t VENDING = 2;// heal vending machine (2x2, green), solid, shop via INTERACT
  constexpr uint8_t ROULETTE = 3;// prize wheel (2x2), solid, roulette via INTERACT
constexpr uint8_t V_DMG = 4;  // damage vending machine (2x2, red), solid, shop via INTERACT
constexpr uint8_t V_SPD = 5;  // speed vending machine (2x2, blue), solid, shop via INTERACT
constexpr uint8_t V_RPD = 6;  // rapid vending machine (2x2, orange), solid, shop via INTERACT
  // NB: 'P' in _art is the player spawn; it parses straight to FLOOR.

  // Super-minimal grass maze: 60x30 all-meadow with a maze interconnecting
  // every district. Vendings sit one per district, some against maze walls, and
  // the 2x2 prize wheel rotates on 4 pads (north, east, centre, SE): only the
  // wave-active pad sells, the rest are dead furniture. Active pad is
  // hash(wave/3) % pads (3-wave epochs from wave 3: waves 1-2, 3-5, ...) in
  // row-major scan order, derived from the synced wave,
  // so host and client agree with no extra net bytes. INTERACT reach is short
  // on purpose, so a wall between player and machine blocks the buy.
  // 2-tile clear lanes on the ring double as the camera cut lines, so a hard cut
  // lands on open grass there; interior cuts may land on maze wall.
  // Legend: # wall  . grass  H heal  D damage  S speed  C rapid  R roulette
  // P spawn (V still parses as heal, for older art.)
  constexpr char _art[ROWS][COLS + 1] = {
"############################################################",
"#..........................................................#",
"#..........................................................#",
"#...............................RR.........................#",
"#..#.....########.....#########.RR.####....########..###...#",
"#..#.P...#HH....#.....#......#..................#..........#",
"#..#.....#HH..#.#....##.###..#...######....####.#.######...#",
"#......#.#....#.#.......#........#................#..DD#...#",
"#..#####.#..###.#....#######...###..###....######.#..DD#...#",
"#..........................................................#",
"#..........................................................#",
"#....................#########...######....................#",
"#..########..####....#.....#.....#....#....########....#...#",
"#..#............#....#..#..#........#...............RR.#...#",
"#..#.#..#####...#....#..#..#.#..###.#.#....#######..RR.#...#",
"#..#.#..#RR.....#.......#....#....#...#....#.....#.....#...#",
"#..#.#..#RR.#........#..#..###..#.#.#.#....#..#....###.#...#",
"#...........#...#....#.....#....#...#.#.......#......#.#...#",
"#..######..##.###....####..#...###.##.#....#######...#.#...#",
"#..........................................................#",
"#..........................................................#",
"#..........................................................#",
"#..#.########..##....###########...####....#.########..#...#",
"#..#............#..........RR#.............#.#.........#...#",
"#..#.#.########.#....#..##.RR#.########....#.#.#########...#",
"#....#......SS#.#....#..#..................#.........CC#...#",
"#..#######..SS#.#....#..#######..######....#######...CC#...#",
"#..........................................................#",
"#..........................................................#",
"############################################################",
  };

  extern uint8_t tiles[ROWS][COLS];
  extern uint16_t spawn_px;
  extern uint16_t spawn_py;

  // BFS distance field to a target tile, in tile steps, over walkable tiles only.
  // UNREACHABLE marks the tiles the target cannot reach. Every reachable tile with
  // d > 0 has a 4-neighbour with d - 1, so walking downhill never gets stuck.
  // field2 is the second player's field (F6 co-op): each zombie descends the field
  // of its nearest alive player. dist_at reads field (player 1).
  constexpr uint16_t UNREACHABLE = 0xFFFF;
  extern uint16_t field[ROWS][COLS];
  extern uint16_t field2[ROWS][COLS];

  void init();
  bool solid(int16_t tx, int16_t ty);
  bool solid_rect(int16_t x, int16_t y, uint8_t w, uint8_t h);
  uint8_t tile_at(int16_t wx, int16_t wy);
  uint16_t color(uint8_t tile);
  uint16_t color_at(int16_t wx, int16_t wy);
  void build_field(int16_t tx, int16_t ty);
  void build_field2(int16_t tx, int16_t ty);
  uint16_t dist_at(int16_t wx, int16_t wy);
}
