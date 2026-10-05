// Native bank for the tilemap and the world/camera invariants. Builds without
// Arduino or TFT_eSPI: run test/test_native/run.sh. It includes the real map.cpp and
// the real display header, and only supplies the one display symbol map.cpp uses.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <vector>
#include <utility>

#include <Display.h>

uint16_t display::rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

#include "../../lib/map/src/map.cpp"

static int fails = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++fails;
  }
}

// mirror of game::_cell_cam
static int16_t cell_cam(int16_t p, int16_t step, int16_t max_cam) {
  int16_t cam = (int16_t)((p / step) * step);
  if (cam < 0) cam = 0;
  else if (cam > max_cam) cam = max_cam;
  return cam;
}

int main() {
  tilemap::init();
  const int16_t SW = 320, SH = 240, HUD = 10;
  const int16_t PANEL_H = 70, AH = 160;
  const int16_t ARENA_BOTTOM = HUD + AH;
  const int16_t AW = SW;
  const int16_t MAX_X = tilemap::WORLD_W - AW, MAX_Y = tilemap::WORLD_H - AH;
  const int16_t MM_SCALE = 2;
  const int16_t MM_W = tilemap::COLS * MM_SCALE, MM_H = tilemap::ROWS * MM_SCALE;
  const int16_t MM_Y = ARENA_BOTTOM + 5, MM_GAP = 4;
  const int16_t MM_X = SW - MM_W - MM_GAP;

  printf("world %ux%u  arena %dx%d  cam max %d,%d\n", tilemap::WORLD_W, tilemap::WORLD_H,
         AW, AH, MAX_X, MAX_Y);
  printf("spawn %u,%u  tile %u,%u\n", tilemap::spawn_px, tilemap::spawn_py,
         tilemap::spawn_px / tilemap::TILE, tilemap::spawn_py / tilemap::TILE);

  // 0) screen layout adds up, minimap fits the panel, camera tiles the world 3x3
  check(HUD + AH + PANEL_H == SH, "hud + arena + panel != screen height");
  check(ARENA_BOTTOM == 170, "arena bottom");
  check(MM_Y + MM_H <= SH && MM_X + MM_W <= SW && MM_X > 0, "minimap does not fit the panel");
  check(MAX_X % AW == 0 && MAX_Y % AH == 0, "world is not an exact multiple of the cell");
  check(MAX_X / AW == 2 && MAX_Y / AH == 2, "camera is not a 3x3 grid of cells");
  printf("layout: hud %d + arena %d + panel %d = %d | minimap %dx%d at %d,%d | cells %dx%d\n",
         HUD, AH, PANEL_H, SH, MM_W, MM_H, MM_X, MM_Y, MAX_X / AW + 1, MAX_Y / AH + 1);

  // 0a) menu row layout, pinned. This cannot watch Game.cpp's coordinate arithmetic (the
  // bank does not compile it, so the double-offset bug it documents was invisible here);
  // what it does catch is a layout change that makes a row collide with the text above it
  // or run into the footer. Rows sit at y0 + i*row in both the full paint and the cursor.
  {
    const int ROW = 24, GLYPH = 16, FOOT = 224; // footer pinned to the glass bottom
    struct Screen { int y0, count, above, last; const char* label; };
    const Screen screens[3] = {
        {96, 3, 64 + 2 * 8, 144, "menu/mode/pause"},
        {128, 2, 96 + 2 * 8, 152, "game over"}, // centred size-2 stat line at 96, rows at 128
        {80, 5, 48 + 2 * 8, 176, "team"}, // centred block: title 48, names 80..176
    };
    check(FOOT + 8 <= 240, "footer runs off the glass");
    for (int s = 0; s < 3; ++s) {
      const Screen& sc = screens[s];
      const int foot = FOOT;
      check(sc.y0 + (sc.count - 1) * ROW == sc.last, "menu row pitch misses the last row");
      for (int i = 0; i < sc.count; ++i) {
        const int y = sc.y0 + i * ROW;
        if (y < sc.above + GLYPH) {
          printf("FAIL %s row %d at y=%d runs into the text above (%d)\n", sc.label, i, y, sc.above);
          ++fails;
        }
        if (y + GLYPH > foot) {
          printf("FAIL %s row %d at y=%d runs into the footer at %d\n", sc.label, i, y, foot);
          ++fails;
        }
      }
      // room for one more item past the current list, so growth still fits the footer
      const int grown = sc.y0 + sc.count * ROW;
      check(grown + GLYPH <= foot, "no room for one more menu item above the footer");
      printf("%s: rows", sc.label);
      for (int i = 0; i < sc.count; ++i) printf(" %d", sc.y0 + i * ROW);
      printf(", footer %d, text above ends %d\n", foot, sc.above);
    }
  }

  // 0b) the minimap camera-cell frame: the cell is a whole number of tiles, the 1px
  // outline stays inside the minimap, and every line lands inside the tile strips the
  // restore repaints (so erasing the frame cannot leave a yellow pixel behind)
  {
    const int cell_tw = AW / tilemap::TILE, cell_th = AH / tilemap::TILE;
    check(AW % tilemap::TILE == 0 && AH % tilemap::TILE == 0, "cell is not a whole number of tiles");
    check(cell_tw * 3 == tilemap::COLS && cell_th * 3 == tilemap::ROWS, "cells do not tile the map");
    const int fw = cell_tw * MM_SCALE, fh = cell_th * MM_SCALE;
    for (int gy = 0; gy < 3; ++gy) {
      for (int gx = 0; gx < 3; ++gx) {
        const int fx = MM_X + gx * fw, fy = MM_Y + gy * fh;
        const int sx = MM_X + gx * fw, sy = MM_Y + gy * fh; // origin of the cell strips
        const bool inside = fx >= MM_X && fy >= MM_Y && fx + fw <= MM_X + MM_W && fy + fh <= MM_Y + MM_H;
        // the 4 lines, each inside the first/last tile strip along its axis
        const bool lines = fy >= sy && fy <= sy + MM_SCALE - 1 &&
                           fy + fh - 1 >= sy + fh - MM_SCALE && fy + fh - 1 <= sy + fh - 1 &&
                           fx >= sx && fx <= sx + MM_SCALE - 1 &&
                           fx + fw - 1 >= sx + fw - MM_SCALE && fx + fw - 1 <= sx + fw - 1;
        if (!inside || !lines) {
          printf("FAIL frame %d,%d %dx%d inside=%d lines_in_strips=%d\n", fx, fy, fw, fh, inside, lines);
        }
      }
    }
    printf("camera cell frame: %d tiles -> %dx%d px, 9 origins inside %dx%d, all lines in strips\n",
           cell_tw, fw, fh, MM_W, MM_H);
  }

  // 1) spawn rect walkable, and the P tile really is the one we parsed
  const int px = tilemap::spawn_px - 4;
  const int py = tilemap::spawn_py - 4;
  check(!tilemap::solid_rect(px, py, 8, 8), "spawn rect is not inside a wall");
  check(tilemap::tile_at(px + 1, py + 1) == tilemap::FLOOR, "spawn tile is floor");

  // 2) out of world == wall
  check(tilemap::solid_rect(-1, 100, 8, 8), "left of world is wall");
  check(tilemap::solid_rect(tilemap::WORLD_W - 1, 100, 8, 8), "right of world is wall");
  check(tilemap::solid_rect(100, tilemap::WORLD_H - 1, 8, 8), "below world is wall");
  check(tilemap::tile_at(-1, -1) == tilemap::WALL, "tile_at out of range -> WALL");

  // 3) the whole walkable world: camera in clamp, player (almost) always on screen
  int worst_x = 0, worst_y = 0;
  int worst_at_x = -1, worst_at_y = -1;
  std::set<int> cam_xs, cam_ys;
  for (int wy = 0; wy < tilemap::WORLD_H; ++wy) {
    for (int wx = 0; wx < tilemap::WORLD_W; ++wx) {
      if (tilemap::solid_rect(wx, wy, 8, 8)) continue; // player top-left here
      const int cx = cell_cam(wx + 4, AW, MAX_X);
      const int cy = cell_cam(wy + 4, AH, MAX_Y);
      cam_xs.insert(cx);
      cam_ys.insert(cy);
      if (cx < 0 || cx > MAX_X || cy < 0 || cy > MAX_Y) {
        printf("FAIL cam out of clamp at %d,%d -> %d,%d\n", wx, wy, cx, cy);
        ++fails;
        continue;
      }
      const int sx = wx - cx, sy = wy - cy + HUD;
      // per axis: the hard cut can hide at most half the sprite on one side
      int lost_x = sx < 0 ? -sx : (sx + 8 > SW ? sx + 8 - SW : 0);
      int lost_y = sy < HUD ? HUD - sy : (sy + 8 > ARENA_BOTTOM ? sy + 8 - ARENA_BOTTOM : 0);
      if (lost_x > worst_x) { worst_x = lost_x; worst_at_x = wx; worst_at_y = wy; }
      if (lost_y > worst_y) { worst_y = lost_y; worst_at_x = wx; worst_at_y = wy; }
    }
  }
  printf("reachable cam x:");
  for (int v : cam_xs) printf(" %d", v);
  printf("\nreachable cam y:");
  for (int v : cam_ys) printf(" %d", v);
  printf("\n");
  printf("worst player clip: x %dpx, y %dpx of 8 (at world %d,%d)\n", worst_x, worst_y, worst_at_x, worst_at_y);
  check(worst_x <= 4 && worst_y <= 4, "hard cut hides at most half the sprite per axis");
  int straddling = 0, walkable_total = 0;
  for (int wy = 0; wy < tilemap::WORLD_H; ++wy) {
    for (int wx = 0; wx < tilemap::WORLD_W; ++wx) {
      if (tilemap::solid_rect(wx, wy, 8, 8)) continue;
      ++walkable_total;
      const int cx = cell_cam(wx + 4, AW, MAX_X);
      const int cy = cell_cam(wy + 4, AH, MAX_Y);
      const int sx = wx - cx, sy = wy - cy + HUD;
      if (sx < 0 || sy < HUD || sx + 8 > SW || sy + 8 > ARENA_BOTTOM) ++straddling;
    }
  }
  printf("walkable px %d, of which %d straddle a cell edge (%.1f%%)\n",
         walkable_total, straddling, 100.0 * straddling / walkable_total);
  printf("  (those are 2-frame transitions, and _fill_world_box clips them)\n");

  // 4) how many world rows no camera position ever shows
  int hidden = 0;
  for (int wy = 0; wy < tilemap::WORLD_H; ++wy) {
    bool walkable = false;
    for (int wx = 0; wx < tilemap::WORLD_W; ++wx) {
      if (!tilemap::solid_rect(wx, wy, 8, 8)) { walkable = true; break; }
    }
    if (!walkable) continue;
    bool shown = false;
    for (int cy : cam_ys) {
      if (wy >= cy && wy < cy + AH) { shown = true; break; }
    }
    if (!shown) { ++hidden; printf("  world row %d is never visible\n", wy); }
  }
  printf("walkable world rows never visible: %d\n", hidden);

  // 5) run-length erase == per-pixel erase, every camera, every tile-aligned rect
  int cases = 0;
  for (int cy = 0; cy <= MAX_Y; cy += 17) {
    for (int cx = 0; cx <= MAX_X; cx += 17) {
      for (int ey = 0; ey + 8 < AH; ey += 5) {
        for (int ex = 0; ex + 8 < AW; ex += 5) {
          const int wx = cx + ex, wy = cy + ey;
          uint16_t want[8][8], got[8][8];
          for (int16_t dy = 0; dy < 8; ++dy) {          // per-pixel reference
            for (int16_t dx = 0; dx < 8; ++dx)
              want[dy][dx] = tilemap::color_at(wx + dx, wy + dy);
          }
          for (int16_t dy = 0; dy < 8; ++dy) {          // run-length impl
            int16_t run_x = 0;
            uint16_t run_col = tilemap::color_at(wx, wy + dy);
            for (int16_t dx = 1; dx < 8; ++dx) {
              const uint16_t col = tilemap::color_at(wx + dx, wy + dy);
              if (col != run_col) {
                for (int16_t i = run_x; i < dx; ++i) got[dy][i] = run_col;
                run_x = dx;
                run_col = col;
              }
            }
            for (int16_t i = run_x; i < 8; ++i) got[dy][i] = run_col;
          }
          if (memcmp(want, got, sizeof(want)) != 0) {
            if (fails < 5) printf("FAIL erase mismatch cam %d,%d rect %d,%d\n", cx, cy, ex, ey);
            ++fails;
          }
          ++cases;
        }
      }
    }
  }
  printf("erase run-length == per-pixel on %d rects across %d camera positions\n", cases,
         (MAX_X + 1) * (MAX_Y + 1));

  // 6) a bullet fired east dies on the west wall, and the border blocks it
  check(tilemap::solid_rect(1, 16 * tilemap::TILE, 4, 4), "bullet at x=1 is inside the border wall");
  {
    // row 1 is the ring road: walkable right up to the border wall (checked in 8)
    const float lane = (float)(tilemap::TILE) + 8.0f;
    check(!tilemap::solid_rect(60, (int16_t)(lane - 2.0f), 4, 4), "ring road lane is walkable");
    float bx = 60.0f, by = lane;
    bool died = false;
    for (int i = 0; i < 400 && !died; ++i) {
      bx -= 4.0f;
      if (tilemap::solid_rect((int16_t)bx, (int16_t)by, 4, 4)) died = true;
    }
    check(died && bx < 20, "bullet travelling west stops at the border wall");
  }

  // 7) every tile kind renders a distinct minimap colour
  {
    int distinct = 0;
    uint16_t seen[16];
    for (int t = 0; t <= 6; ++t) {
      bool dup = false;
      for (int i = 0; i < distinct; ++i) dup |= (seen[i] == tilemap::color(t));
      if (!dup) seen[distinct++] = tilemap::color(t);
    }
    printf("distinct tile colours for tiles 0..6: %d\n", distinct);
    check(distinct == 7, "each tile kind has its own colour");
  }

  // 8) flood fill from the spawn: no sealed walkable pocket, ring lanes are
  //    open, every vending machine and the roulette touch reachable ground
  {
    const int stx = tilemap::spawn_px / tilemap::TILE, sty = tilemap::spawn_py / tilemap::TILE;
    std::set<std::pair<int, int> > seen;
    std::vector<std::pair<int, int> > q;
    q.push_back(std::make_pair(sty, stx)); // (row, col)
    seen.insert(q[0]);
    for (size_t i = 0; i < q.size(); ++i) {
      const int r = q[i].first, c = q[i].second;
      const int dr[4] = {1, -1, 0, 0}, dc[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k) {
        const int nr = r + dr[k], nc = c + dc[k];
        if (nr < 0 || nc < 0 || nr >= tilemap::ROWS || nc >= tilemap::COLS) continue;
        if (tilemap::solid(nc, nr)) continue;
        if (seen.insert(std::make_pair(nr, nc)).second) q.push_back(std::make_pair(nr, nc));
      }
    }
    int walk = 0;
    for (int r = 0; r < tilemap::ROWS; ++r)
      for (int c = 0; c < tilemap::COLS; ++c)
        if (!tilemap::solid(c, r)) ++walk;
    printf("walkable tiles %d/%d (%.0f%%), reachable %zu, orphans %d\n", walk,
           tilemap::COLS * tilemap::ROWS, 100.0 * walk / (tilemap::COLS * tilemap::ROWS),
           seen.size(), walk - (int)seen.size());
    check(walk - (int)seen.size() == 0, "there are sealed walkable pockets");

    // per-block: wall-embedded machines are legit (corners may touch maze),
    // what matters is each 2x2 block has reachable ground on at least one side
    int shops = 0, blocks = 0, blocks_ok = 0;
    int heal = 0, dmg = 0, spd = 0, rpd = 0, roll = 0;
    for (int r = 0; r < tilemap::ROWS; ++r) {
      for (int c = 0; c < tilemap::COLS; ++c) {
        const uint8_t t = tilemap::tile_at(c * tilemap::TILE, r * tilemap::TILE);
        if (t != tilemap::VENDING && t != tilemap::V_DMG && t != tilemap::V_SPD &&
            t != tilemap::V_RPD && t != tilemap::ROULETTE) {
          continue;
        }
        ++shops;
        if (t == tilemap::VENDING) ++heal;
        else if (t == tilemap::V_DMG) ++dmg;
        else if (t == tilemap::V_SPD) ++spd;
        else if (t == tilemap::V_RPD) ++rpd;
        else ++roll;
        // top-left tile of the 2x2 block only, so each machine counts once
        if (c > 0 && tilemap::tile_at((c - 1) * tilemap::TILE, r * tilemap::TILE) == t) continue;
        if (r > 0 && tilemap::tile_at(c * tilemap::TILE, (r - 1) * tilemap::TILE) == t) continue;
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
    for (int c = 1; c < tilemap::COLS - 1; ++c) {
      if (tilemap::solid(c, 1) || tilemap::solid(c, 2) ||
          tilemap::solid(c, tilemap::ROWS - 2) || tilemap::solid(c, tilemap::ROWS - 3)) ring_open = 0;
    }
    for (int r = 1; r < tilemap::ROWS - 1; ++r) {
      if (tilemap::solid(1, r) || tilemap::solid(2, r) ||
          tilemap::solid(tilemap::COLS - 2, r) || tilemap::solid(tilemap::COLS - 3, r)) ring_open = 0;
    }
    check(ring_open, "the 2-tile ring road is broken (no kiting loop)");
  }

  // 9) the spawn scan always finds a walkable tile >= 100px from the player
  {
    const int total = tilemap::COLS * tilemap::ROWS;
    const int px = tilemap::spawn_px, py = tilemap::spawn_py;
    int placed = 0, too_close = 0;
    for (int start = 0; start < total; start += 7) {
      bool found = false;
      for (int k = 0; k < total; ++k) {
        const int idx = (start + k) % total;
        const int tx = idx % tilemap::COLS, ty = idx / tilemap::COLS;
        if (tilemap::solid(tx, ty)) continue;
        const int x = tx * tilemap::TILE + (tilemap::TILE - 6) / 2;
        const int y = ty * tilemap::TILE + (tilemap::TILE - 6) / 2;
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

  // 10) the chunked arena repaint covers every row exactly once, in 2 frames
  {
    const int CHUNK = 80;
    int frames = 0, y = 0, painted = 0;
    while (y < AH) { y += CHUNK; ++frames; }
    painted = y < AH ? y : AH;
    printf("arena %d rows repainted in %d frames of %d rows (last %d)\n", AH, frames, CHUNK,
           painted - (frames - 1) * CHUNK);
    check(frames == 2, "the arena repaint does not fit in 2 frames");
    check(painted == AH, "the chunked repaint does not cover the whole arena");
    check(AH * SW * 2 <= 120000, "arena repaint payload");
  }

  // 11) the menu must not repaint the whole panel every frame
  {
    // a full-screen fill is 320*240*2 bytes, ~31ms of SPI at 40MHz: done per frame it
    // both blew the 33ms budget and tore against the scan-out (a line sweeping the panel)
    const int full_bytes = SW * SH * 2;
    const int spi_hz = 40000000;
    const int full_ms = (int)((1000L * full_bytes * 8) / spi_hz);
    // what a steady-state menu frame costs: the two cursor lines, 14 chars
    // (longest item + '>') of 12x16 glyphs. Size-2 chrome costs ~4x the old size-1,
    // still ~2ms against the 31ms full fill.
    const int line_bytes = 14 * 12 * 16 * 2;
    const int cursor_bytes = 2 * line_bytes;
    printf("menu: full-screen fill %d bytes = %dms at 40MHz | steady frame %d bytes\n", full_bytes,
           full_ms, cursor_bytes);
    check(full_ms >= 30, "the full-screen fill model is off, re-check the SPI clock");
    check(cursor_bytes * 10 < full_bytes, "a menu frame is too close to a full repaint");
    check(cursor_bytes * 10 / (spi_hz / 8 / 1000) <= 33, "a menu frame blows the 33ms budget");
  }

  // 12) nothing may bleed into the hud strip or the panel
  {
    int bleeds = 0;
    for (int cy = 0; cy <= MAX_Y; cy += 17) {
      for (int cx = 0; cx <= MAX_X; cx += 17) {
        for (int ey = 0; ey + 8 < tilemap::WORLD_H; ++ey += 5) {
          for (int ex = 0; ex + 8 < tilemap::WORLD_W; ++ex += 5) {
            const int wx = cx + ex, wy = cy + ey;
            // _fill_world_box clip box, verbatim
            const int x0 = wx - cx, y0 = wy - cy + HUD;
            const int cx0 = x0 > 0 ? x0 : 0;
            const int cy0 = y0 > HUD ? y0 : HUD;
            const int cx1 = x0 + 8 < SW ? x0 + 8 : SW;
            const int cy1 = y0 + 8 < ARENA_BOTTOM ? y0 + 8 : ARENA_BOTTOM;
            if (cx0 >= cx1 || cy0 >= cy1) continue; // fully clipped, nothing drawn
            if (cy0 < HUD || cy1 > ARENA_BOTTOM) ++bleeds;
          }
        }
      }
    }
    printf("clip boxes overlapping hud or panel: %d\n", bleeds);
    check(bleeds == 0, "a sprite box can be drawn outside the arena");
  }

  // 13) the BFS distance field: no local minima, so walking downhill always arrives
  {
    const int stx = tilemap::spawn_px / tilemap::TILE, sty = tilemap::spawn_py / tilemap::TILE;
    tilemap::build_field(stx, sty);
    check(tilemap::field[sty][stx] == 0, "the field does not start at 0 on the target tile");
    check(tilemap::dist_at(tilemap::spawn_px, tilemap::spawn_py) == 0, "dist_at != 0 on the spawn");
    check(tilemap::dist_at(-1, 100) == tilemap::UNREACHABLE, "dist_at left of world");
    check(tilemap::dist_at(100, -1) == tilemap::UNREACHABLE, "dist_at above world");
    check(tilemap::dist_at(tilemap::WORLD_W, 100) == tilemap::UNREACHABLE, "dist_at right of world");
    check(tilemap::dist_at(100, tilemap::WORLD_H) == tilemap::UNREACHABLE, "dist_at below world");

    const int dr[4] = {1, -1, 0, 0}, dc[4] = {0, 0, 1, -1};
    int reach = 0, max_d = 0, no_descent = 0, solid_reached = 0;
    for (int r = 0; r < tilemap::ROWS; ++r) {
      for (int c = 0; c < tilemap::COLS; ++c) {
        const uint16_t d = tilemap::field[r][c];
        if (tilemap::solid(c, r)) {
          if (d != tilemap::UNREACHABLE) ++solid_reached;
          continue;
        }
        if (d == tilemap::UNREACHABLE) continue;
        ++reach;
        if ((int)d > max_d) max_d = d;
        if (d == 0) continue;
        bool down = false;
        for (int k = 0; k < 4; ++k) {
          const int nr = r + dr[k], nc = c + dc[k];
          if (nr < 0 || nr >= tilemap::ROWS || nc < 0 || nc >= tilemap::COLS) continue;
          if (tilemap::field[nr][nc] == d - 1) { down = true; break; }
        }
        if (!down) ++no_descent;
      }
    }
    printf("field from the spawn: %d tiles, max dist %d, tiles without a descending 4-neighbour %d\n",
           reach, max_d, no_descent);
    check(reach > 1000, "the field reaches too little of the map");
    {
      int walkable = 0;
      for (int r = 0; r < tilemap::ROWS; ++r)
        for (int c = 0; c < tilemap::COLS; ++c)
          if (!tilemap::solid(c, r)) ++walkable;
      check(reach == walkable, "the field does not reach every walkable tile");
    }
    check(no_descent == 0, "the field has a local minimum, a zombie would stall there");
    check(solid_reached == 0, "the field leaked a distance onto a wall");
    check(max_d < 255, "the field needs more than uint8_t");

    // mirror of game::_zombie_steer: pick the lowest-distance neighbour, aim at its centre
    const int nx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, ny[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    int stuck = 0, longest = 0, samples = 0;
    for (int r = 0; r < tilemap::ROWS; r += 1) {
      for (int c = 0; c < tilemap::COLS; ++c) {
        if ((r * 7 + c * 13) % 9) continue; // deterministic sample of tiles
        if (tilemap::solid(c, r) || tilemap::field[r][c] == tilemap::UNREACHABLE) continue;
        ++samples;
        int cr = r, cc = c, steps = 0;
        while (tilemap::field[cr][cc] > 0 && steps <= max_d + 2) {
          int best = -1;
          uint16_t best_d = tilemap::field[cr][cc];
          for (int k = 0; k < 8; ++k) {
            const int tr = cr + ny[k], tc = cc + nx[k];
            if (tr < 0 || tr >= tilemap::ROWS || tc < 0 || tc >= tilemap::COLS) continue;
            if (tilemap::field[tr][tc] < best_d) { best_d = tilemap::field[tr][tc]; best = k; }
          }
          if (best < 0) break;
          cr += ny[best];
          cc += nx[best];
          ++steps;
        }
        if (tilemap::field[cr][cc] != 0) ++stuck;
        if (steps > longest) longest = steps;
      }
    }
    printf("steering from %d sampled tiles: %d never reach the player, longest walk %d steps (max %d)\n",
           samples, stuck, longest, max_d);
    check(stuck == 0, "a zombie can descend the field and still never reach the player");

    // a wall tile has nothing to spread from
    int wtx = -1, wty = -1;
    for (int r = 0; r < tilemap::ROWS && wtx < 0; ++r) {
      for (int c = 0; c < tilemap::COLS; ++c) {
        if (tilemap::solid(c, r)) { wtx = c; wty = r; break; }
      }
    }
    int wall_leaks = 0;
    if (wtx >= 0) {
      tilemap::build_field(wtx, wty);
      for (int rr = 0; rr < tilemap::ROWS; ++rr)
        for (int cc = 0; cc < tilemap::COLS; ++cc)
          if (tilemap::field[rr][cc] != tilemap::UNREACHABLE) ++wall_leaks;
    }
    check(wtx >= 0 && wall_leaks == 0, "build_field on a wall must leave everything UNREACHABLE");
    printf("build_field on a wall tile %d,%d: %d tiles reachable, no hang\n", wtx, wty, wall_leaks);
  }

  printf(fails ? "\n%d FAILURES\n" : "\nall checks passed\n", fails);
  return fails ? 1 : 0;
}
