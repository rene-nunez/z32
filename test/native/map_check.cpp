// Native map/camera checks: includes the real Map.cpp, stubs Display::rgb565.
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>
#include <utility>

#include <Display.h>

uint16_t Display::rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

#include "../../lib/map/Map.cpp"

static int fails = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++fails;
  }
}

// mirror of Game::cell_cam
static int16_t cell_cam(int16_t p, int16_t step, int16_t max_cam) {
  int16_t cam = (int16_t)((p / step) * step);
  if (cam < 0) cam = 0;
  else if (cam > max_cam) cam = max_cam;
  return cam;
}

int main() {
  Tilemap::init();
  const int16_t SW = 320, SH = 240, HUD = 10;
  const int16_t PANEL_H = 70, AH = 160;
  const int16_t ARENA_BOTTOM = HUD + AH;
  const int16_t AW = SW;
  const int16_t MAX_X = Tilemap::WORLD_W - AW, MAX_Y = Tilemap::WORLD_H - AH;
  const int16_t MM_SCALE = 2;
  const int16_t MM_W = Tilemap::COLS * MM_SCALE, MM_H = Tilemap::ROWS * MM_SCALE;
  const int16_t MM_Y = ARENA_BOTTOM + 5, MM_GAP = 4;
  const int16_t MM_X = SW - MM_W - MM_GAP;

  printf("world %ux%u  arena %dx%d  cam max %d,%d\n", Tilemap::WORLD_W, Tilemap::WORLD_H,
         AW, AH, MAX_X, MAX_Y);
  printf("spawn %u,%u  tile %u,%u\n", Tilemap::spawn_px, Tilemap::spawn_py,
         Tilemap::spawn_px / Tilemap::TILE, Tilemap::spawn_py / Tilemap::TILE);

  // 0) screen layout adds up, minimap fits the panel, camera tiles the world 3x3
  check(HUD + AH + PANEL_H == SH, "hud + arena + panel != screen height");
  check(ARENA_BOTTOM == 170, "arena bottom");
  check(MM_Y + MM_H <= SH && MM_X + MM_W <= SW && MM_X > 0, "minimap does not fit the panel");
  check(MAX_X % AW == 0 && MAX_Y % AH == 0, "world is not an exact multiple of the cell");
  check(MAX_X / AW == 2 && MAX_Y / AH == 2, "camera is not a 3x3 grid of cells");
  printf("layout: hud %d + arena %d + panel %d = %d | minimap %dx%d at %d,%d | cells %dx%d\n",
         HUD, AH, PANEL_H, SH, MM_W, MM_H, MM_X, MM_Y, MAX_X / AW + 1, MAX_Y / AH + 1);

  // 0a) menu rows sit at y0 + i*ROW, clear of the text above and the footer
  {
    const int ROW = 24, GLYPH = 16, FOOT = 224; // footer pinned to the glass bottom
    struct Screen { int y0, count, above; const char* label; };
    const Screen screens[3] = {
        {96, 3, 64 + 2 * 8, "menu/mode/pause"},
        {128, 2, 96 + 2 * 8, "game over"}, // centred size-2 stat line at 96, rows at 128
        {80, 5, 48 + 2 * 8, "team"}, // centred block: title 48, names 80..176
    };
    check(FOOT + 8 <= 240, "footer runs off the glass");
    for (int s = 0; s < 3; ++s) {
      const Screen& sc = screens[s];
      for (int i = 0; i < sc.count; ++i) {
        const int y = sc.y0 + i * ROW;
        if (y < sc.above + GLYPH) {
          printf("FAIL %s row %d at y=%d runs into the text above (%d)\n", sc.label, i, y, sc.above);
          ++fails;
        }
        if (y + GLYPH > FOOT) {
          printf("FAIL %s row %d at y=%d runs into the footer at %d\n", sc.label, i, y, FOOT);
          ++fails;
        }
      }
      // room for one more item past the current list, so growth still fits the footer
      const int grown = sc.y0 + sc.count * ROW;
      check(grown + GLYPH <= FOOT, "no room for one more menu item above the footer");
      printf("%s: rows", sc.label);
      for (int i = 0; i < sc.count; ++i) printf(" %d", sc.y0 + i * ROW);
      printf(", footer %d, text above ends %d\n", FOOT, sc.above);
    }
  }

  // 0b) the camera cell is a whole number of tiles
  {
    const int cell_tw = AW / Tilemap::TILE, cell_th = AH / Tilemap::TILE;
    check(AW % Tilemap::TILE == 0 && AH % Tilemap::TILE == 0, "cell is not a whole number of tiles");
    check(cell_tw * 3 == Tilemap::COLS && cell_th * 3 == Tilemap::ROWS, "cells do not tile the map");
    printf("camera cell frame: %d tiles -> %dx%d px\n", cell_tw, cell_tw * MM_SCALE,
           cell_th * MM_SCALE);
  }

  // 1) spawn rect walkable, and the P tile really is the one we parsed
  const int px = Tilemap::spawn_px - 4;
  const int py = Tilemap::spawn_py - 4;
  check(!Tilemap::solid_rect(px, py, 8, 8), "spawn rect is not inside a wall");
  check(Tilemap::tile_at(px + 1, py + 1) == Tilemap::FLOOR, "spawn tile is floor");

  // 2) out of world == wall
  check(Tilemap::solid_rect(-1, 100, 8, 8), "left of world is wall");
  check(Tilemap::solid_rect(Tilemap::WORLD_W - 1, 100, 8, 8), "right of world is wall");
  check(Tilemap::solid_rect(100, Tilemap::WORLD_H - 1, 8, 8), "below world is wall");
  check(Tilemap::tile_at(-1, -1) == Tilemap::WALL, "tile_at out of range -> WALL");

  // 3) camera in clamp, player at most half clipped, reachable camera origins
  int worst_x = 0, worst_y = 0, worst_at_x = -1, worst_at_y = -1;
  int straddling = 0, walkable_total = 0;
  std::set<int> cam_xs, cam_ys;
  for (int wy = 0; wy < Tilemap::WORLD_H; ++wy) {
    for (int wx = 0; wx < Tilemap::WORLD_W; ++wx) {
      if (Tilemap::solid_rect(wx, wy, 8, 8)) continue; // player top-left here
      ++walkable_total;
      const int cx = cell_cam(wx + 4, AW, MAX_X);
      const int cy = cell_cam(wy + 4, AH, MAX_Y);
      cam_xs.insert(cx);
      cam_ys.insert(cy);
      const int sx = wx - cx, sy = wy - cy + HUD;
      const int lost_x = sx < 0 ? -sx : (sx + 8 > SW ? sx + 8 - SW : 0);
      const int lost_y = sy < HUD ? HUD - sy : (sy + 8 > ARENA_BOTTOM ? sy + 8 - ARENA_BOTTOM : 0);
      if (lost_x > worst_x) { worst_x = lost_x; worst_at_x = wx; worst_at_y = wy; }
      if (lost_y > worst_y) { worst_y = lost_y; worst_at_x = wx; worst_at_y = wy; }
      if (sx < 0 || sy < HUD || sx + 8 > SW || sy + 8 > ARENA_BOTTOM) ++straddling;
    }
  }
  printf("reachable cam x:");
  for (int v : cam_xs) printf(" %d", v);
  printf("\nreachable cam y:");
  for (int v : cam_ys) printf(" %d", v);
  printf("\nworst player clip: x %dpx, y %dpx of 8 (at world %d,%d)\n",
         worst_x, worst_y, worst_at_x, worst_at_y);
  printf("walkable px %d, of which %d straddle a cell edge (%.1f%%)\n",
         walkable_total, straddling, 100.0 * straddling / walkable_total);
  check(worst_x <= 4 && worst_y <= 4, "hard cut hides at most half the sprite per axis");

  // 4) how many world rows no camera position ever shows
  int hidden = 0;
  for (int wy = 0; wy < Tilemap::WORLD_H; ++wy) {
    bool walkable = false;
    for (int wx = 0; wx < Tilemap::WORLD_W; ++wx) {
      if (!Tilemap::solid_rect(wx, wy, 8, 8)) { walkable = true; break; }
    }
    if (!walkable) continue;
    bool shown = false;
    for (int cy : cam_ys) {
      if (wy >= cy && wy < cy + AH) { shown = true; break; }
    }
    if (!shown) { ++hidden; printf("  world row %d is never visible\n", wy); }
  }
  printf("walkable world rows never visible: %d\n", hidden);
  check(hidden == 0, "a walkable world row is never visible");

  // 5) a bullet fired west stops at the border wall
  check(Tilemap::solid_rect(1, 16 * Tilemap::TILE, 4, 4), "bullet at x=1 is inside the border wall");
  {
    // row 1 is the ring road: walkable right up to the border wall (checked in 7)
    const float lane = (float)(Tilemap::TILE) + 8.0f;
    check(!Tilemap::solid_rect(60, (int16_t)(lane - 2.0f), 4, 4), "ring road lane is walkable");
    float bx = 60.0f, by = lane;
    bool died = false;
    for (int i = 0; i < 400 && !died; ++i) {
      bx -= 4.0f;
      if (Tilemap::solid_rect((int16_t)bx, (int16_t)by, 4, 4)) died = true;
    }
    check(died && bx < 20, "bullet travelling west stops at the border wall");
  }

  // 6) every tile kind renders a distinct minimap colour
  {
    int distinct = 0;
    uint16_t seen[16];
    for (int t = 0; t <= 6; ++t) {
      bool dup = false;
      for (int i = 0; i < distinct; ++i) dup |= (seen[i] == Tilemap::color(t));
      if (!dup) seen[distinct++] = Tilemap::color(t);
    }
    printf("distinct tile colours for tiles 0..6: %d\n", distinct);
    check(distinct == 7, "each tile kind has its own colour");
  }

  // 7) flood fill from the spawn: no sealed walkable pocket, ring lanes are
  //    open, every vending machine and the roulette touch reachable ground
  {
    const int stx = Tilemap::spawn_px / Tilemap::TILE, sty = Tilemap::spawn_py / Tilemap::TILE;
    std::set<std::pair<int, int> > seen;
    std::vector<std::pair<int, int> > q;
    q.push_back(std::make_pair(sty, stx)); // (row, col)
    seen.insert(q[0]);
    for (size_t i = 0; i < q.size(); ++i) {
      const int r = q[i].first, c = q[i].second;
      const int dr[4] = {1, -1, 0, 0}, dc[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k) {
        const int nr = r + dr[k], nc = c + dc[k];
        if (nr < 0 || nc < 0 || nr >= Tilemap::ROWS || nc >= Tilemap::COLS) continue;
        if (Tilemap::solid(nc, nr)) continue;
        if (seen.insert(std::make_pair(nr, nc)).second) q.push_back(std::make_pair(nr, nc));
      }
    }
    int walk = 0;
    for (int r = 0; r < Tilemap::ROWS; ++r)
      for (int c = 0; c < Tilemap::COLS; ++c)
        if (!Tilemap::solid(c, r)) ++walk;
    printf("walkable tiles %d/%d (%.0f%%), reachable %zu, orphans %d\n", walk,
           Tilemap::COLS * Tilemap::ROWS, 100.0 * walk / (Tilemap::COLS * Tilemap::ROWS),
           seen.size(), walk - (int)seen.size());
    check(walk - (int)seen.size() == 0, "there are sealed walkable pockets");

    // each 2x2 machine block needs reachable ground on at least one side
    int shops = 0, blocks = 0, blocks_ok = 0;
    int heal = 0, dmg = 0, spd = 0, rpd = 0, roll = 0;
    for (int r = 0; r < Tilemap::ROWS; ++r) {
      for (int c = 0; c < Tilemap::COLS; ++c) {
        const uint8_t t = Tilemap::tile_at(c * Tilemap::TILE, r * Tilemap::TILE);
        if (t != Tilemap::VENDING && t != Tilemap::V_DMG && t != Tilemap::V_SPD &&
            t != Tilemap::V_RPD && t != Tilemap::ROULETTE) {
          continue;
        }
        ++shops;
        if (t == Tilemap::VENDING) ++heal;
        else if (t == Tilemap::V_DMG) ++dmg;
        else if (t == Tilemap::V_SPD) ++spd;
        else if (t == Tilemap::V_RPD) ++rpd;
        else ++roll;
        // top-left tile of the 2x2 block only, so each machine counts once
        if (c > 0 && Tilemap::tile_at((c - 1) * Tilemap::TILE, r * Tilemap::TILE) == t) continue;
        if (r > 0 && Tilemap::tile_at(c * Tilemap::TILE, (r - 1) * Tilemap::TILE) == t) continue;
        ++blocks;
        const int dr[4] = {1, -1, 0, 0}, dc[4] = {0, 0, 1, -1};
        bool ok = false;
        for (int br = r; br <= r + 1 && !ok; ++br)
          for (int bc = c; bc <= c + 1 && !ok; ++bc)
            for (int k = 0; k < 4 && !ok; ++k)
              ok = seen.count(std::make_pair(br + dr[k], bc + dc[k])) > 0;
        if (ok) {
          ++blocks_ok;
        } else {
          printf("  unreachable block at col %d row %d type %d\n", c, r, t);
        }
      }
    }
    printf("shops %d (heal %d dmg %d spd %d rpd %d roll %d), blocks %d reachable %d\n",
           shops, heal, dmg, spd, rpd, roll, blocks, blocks_ok);
    check(shops == 32 && blocks == 8 && blocks_ok == 8, "not every machine block is reachable");
    check(heal == 4 && dmg == 4 && spd == 4 && rpd == 4 && roll == 16,
          "shops must be 2x2 (H/D/S/C x1, roll x4)");

    int ring_open = 1;
    for (int c = 1; c < Tilemap::COLS - 1; ++c) {
      if (Tilemap::solid(c, 1) || Tilemap::solid(c, 2) ||
          Tilemap::solid(c, Tilemap::ROWS - 2) || Tilemap::solid(c, Tilemap::ROWS - 3)) ring_open = 0;
    }
    for (int r = 1; r < Tilemap::ROWS - 1; ++r) {
      if (Tilemap::solid(1, r) || Tilemap::solid(2, r) ||
          Tilemap::solid(Tilemap::COLS - 2, r) || Tilemap::solid(Tilemap::COLS - 3, r)) ring_open = 0;
    }
    check(ring_open, "the 2-tile ring road is broken (no kiting loop)");
  }

  // 8) the spawn scan always finds a walkable tile >= 100px from the player
  {
    const int total = Tilemap::COLS * Tilemap::ROWS;
    const int px = Tilemap::spawn_px, py = Tilemap::spawn_py;
    int placed = 0, too_close = 0;
    for (int start = 0; start < total; start += 7) {
      bool found = false;
      for (int k = 0; k < total; ++k) {
        const int idx = (start + k) % total;
        const int tx = idx % Tilemap::COLS, ty = idx / Tilemap::COLS;
        if (Tilemap::solid(tx, ty)) continue;
        const int x = tx * Tilemap::TILE + (Tilemap::TILE - 6) / 2;
        const int y = ty * Tilemap::TILE + (Tilemap::TILE - 6) / 2;
        const int dx = x - px, dy = y - py;
        if (dx * dx + dy * dy < 100 * 100 && k + 1 < total) continue;
        if (dx * dx + dy * dy < 100 * 100) ++too_close;
        found = true;
        break;
      }
      placed += found;
    }
    printf("spawn scan: %d/%d start offsets placed a zombie, %d closer than 100px\n", placed,
           (total + 6) / 7, too_close);
    check(placed == (total + 6) / 7, "the spawn scan failed to place a zombie");
    check(too_close == 0, "a zombie spawned on top of the player");
  }

  // 9) the arena repaint fits two 80-row frames and one RAM payload
  {
    const int CHUNK = 80;
    printf("arena %d rows repainted in %d frames of %d rows\n", AH, (AH + CHUNK - 1) / CHUNK, CHUNK);
    check(AH <= 2 * CHUNK, "the arena repaint does not fit in 2 frames");
    check(AH * SW * 2 <= 120000, "arena repaint payload");
  }

  // 10) the BFS distance field: no local minima, so walking downhill always arrives
  {
    const int stx = Tilemap::spawn_px / Tilemap::TILE, sty = Tilemap::spawn_py / Tilemap::TILE;
    Tilemap::build_field(stx, sty);
    check(Tilemap::field[sty][stx] == 0, "the field does not start at 0 on the target tile");
    check(Tilemap::dist_at(Tilemap::spawn_px, Tilemap::spawn_py) == 0, "dist_at != 0 on the spawn");
    check(Tilemap::dist_at(-1, 100) == Tilemap::UNREACHABLE, "dist_at left of world");
    check(Tilemap::dist_at(100, -1) == Tilemap::UNREACHABLE, "dist_at above world");
    check(Tilemap::dist_at(Tilemap::WORLD_W, 100) == Tilemap::UNREACHABLE, "dist_at right of world");
    check(Tilemap::dist_at(100, Tilemap::WORLD_H) == Tilemap::UNREACHABLE, "dist_at below world");

    const int dr[4] = {1, -1, 0, 0}, dc[4] = {0, 0, 1, -1};
    int reach = 0, max_d = 0, no_descent = 0, solid_reached = 0;
    for (int r = 0; r < Tilemap::ROWS; ++r) {
      for (int c = 0; c < Tilemap::COLS; ++c) {
        const uint16_t d = Tilemap::field[r][c];
        if (Tilemap::solid(c, r)) {
          if (d != Tilemap::UNREACHABLE) ++solid_reached;
          continue;
        }
        if (d == Tilemap::UNREACHABLE) continue;
        ++reach;
        if ((int)d > max_d) max_d = d;
        if (d == 0) continue;
        bool down = false;
        for (int k = 0; k < 4; ++k) {
          const int nr = r + dr[k], nc = c + dc[k];
          if (nr < 0 || nr >= Tilemap::ROWS || nc < 0 || nc >= Tilemap::COLS) continue;
          if (Tilemap::field[nr][nc] == d - 1) { down = true; break; }
        }
        if (!down) ++no_descent;
      }
    }
    printf("field from the spawn: %d tiles, max dist %d, tiles without a descending 4-neighbour %d\n",
           reach, max_d, no_descent);
    {
      int walkable = 0;
      for (int r = 0; r < Tilemap::ROWS; ++r)
        for (int c = 0; c < Tilemap::COLS; ++c)
          if (!Tilemap::solid(c, r)) ++walkable;
      check(reach == walkable, "the field does not reach every walkable tile");
    }
    check(no_descent == 0, "the field has a local minimum, a zombie would stall there");
    check(solid_reached == 0, "the field leaked a distance onto a wall");
    check(max_d < 255, "the field needs more than uint8_t");

    // mirror of Game::zombie_steer: pick the lowest-distance neighbour, aim at its centre
    const int nx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, ny[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    int stuck = 0, longest = 0, samples = 0;
    for (int r = 0; r < Tilemap::ROWS; r += 1) {
      for (int c = 0; c < Tilemap::COLS; ++c) {
        if ((r * 7 + c * 13) % 9) continue; // deterministic sample of tiles
        if (Tilemap::solid(c, r) || Tilemap::field[r][c] == Tilemap::UNREACHABLE) continue;
        ++samples;
        int cr = r, cc = c, steps = 0;
        while (Tilemap::field[cr][cc] > 0 && steps <= max_d + 2) {
          int best = -1;
          uint16_t best_d = Tilemap::field[cr][cc];
          for (int k = 0; k < 8; ++k) {
            const int tr = cr + ny[k], tc = cc + nx[k];
            if (tr < 0 || tr >= Tilemap::ROWS || tc < 0 || tc >= Tilemap::COLS) continue;
            if (Tilemap::field[tr][tc] < best_d) { best_d = Tilemap::field[tr][tc]; best = k; }
          }
          if (best < 0) break;
          cr += ny[best];
          cc += nx[best];
          ++steps;
        }
        if (Tilemap::field[cr][cc] != 0) ++stuck;
        if (steps > longest) longest = steps;
      }
    }
    printf("steering from %d sampled tiles: %d never reach the player, longest walk %d steps (max %d)\n",
           samples, stuck, longest, max_d);
    check(stuck == 0, "a zombie can descend the field and still never reach the player");

    int wtx = -1, wty = -1;
    for (int r = 0; r < Tilemap::ROWS && wtx < 0; ++r) {
      for (int c = 0; c < Tilemap::COLS; ++c) {
        if (Tilemap::solid(c, r)) { wtx = c; wty = r; break; }
      }
    }
    int wall_leaks = 0;
    if (wtx >= 0) {
      Tilemap::build_field(wtx, wty);
      for (int rr = 0; rr < Tilemap::ROWS; ++rr)
        for (int cc = 0; cc < Tilemap::COLS; ++cc)
          if (Tilemap::field[rr][cc] != Tilemap::UNREACHABLE) ++wall_leaks;
    }
    check(wtx >= 0 && wall_leaks == 0, "build_field on a wall must leave everything UNREACHABLE");
    printf("build_field on a wall tile %d,%d: %d tiles reachable, no hang\n", wtx, wty, wall_leaks);
  }

  printf(fails ? "\n%d FAILURES\n" : "\nall checks passed\n", fails);
  return fails ? 1 : 0;
}
