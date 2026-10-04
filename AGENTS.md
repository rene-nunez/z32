# AGENTS.md — Z32 (ex game)

## Style

- `snake_case` everywhere; class private members and instances prefixed `_`; read `.editorconfig`

## Network

- packed structs (`__attribute__((packed))`); first field is always `type`
- 250 bytes/msg max; frequent state must be binary, not JSON
- host is authoritative (runs game logic, sends state); client sends inputs
- fixed custom MACs; role picks the peer
- role comes from build: `pio run -e host` | `-e client` (flag `DEVICE_ROLE`, validated in `src/main.cpp`)

## Pins

- TFT SPI: `CS5/RST4/DC2/MOSI23/SCLK18/MISO19/BL21`; joystick ADC1 `32/33`; buttons `FIRE13/RELOAD14/INTERACT15/PAUSE27`
- buzzer pasivo: GPIO 26 vía LEDC (`lib/buzz`, non-blocking)
- microSD: comparte bus SPI del TFT (23/19/18) + CS dedicado GPIO 22 + VCC/GND
- ADC2 no se usa para analógico (WiFi/ESP-NOW lo inhabilita); LEDC en 25/26 es digital y no choca

## Layout

- `include/` — `pins.h`, `tft_setup.h` (ST7789 + `TFT_INVERSION_OFF` + `TFT_RGB_ORDER TFT_BGR`, panel BGR)
- `lib/network` — raw ESP-NOW, no message logic; `lib/protocol` — `msg_type` + packed structs (`NetState.h`: `game_state` 128B + `player_input` 5B, Arduino-free so `test_native` checks sizes/round-trips; spare flag bits carry the 3-bit facing, rapid levels cost one meta byte); `lib/handler` — typed routing/dispatch
- `lib/display` — `display` + `colour` (+`push_image` for one-burst RAM blits of terrain/actor composites, no transparency); the only place TFT_eSPI is used
- `lib/input` — joystick (ADC1) + buttons (debounce + edge); stick centre calibrated at boot (20 samples/axis, ~200ms, hands off) since pots rest off 2047 per board; deadzone 0.3, span scaled to the rail from the calibrated centre
- `lib/world` — `tilemap`: 60x30 grass maze, `_art` rows, wall queries, spawn, BFS `field` (+`field2` for player 2); tiles: meadow floor (only walkable), pale concrete walls, four 2x2 vendings (H heal green, D damage red, S speed blue, C rapid orange, some wall-embedded) + 4x 2x2 roulette pads (visual 32px sprites in `color_at`, price tags in `render`)
- `lib/sim` — players[2] (P1 local, P2 net/inactive Solo), zombies, bullets, waves, shared wallet/gun/levels per player (wallet/kills/wave shared, gun+DMG/SPD/ROF per player, P2 joins fresh pistol/0), downed/bleed/revive (0 HP → downed 15s, partner lifts with INTERACT to 1 HP, dead respawn per wave); stick rest is `len < 0.2` → zeroed, never moves (same threshold as facing, kills pot slide); every player/actor carries 8-wind `facing` (`dir_of`: 0=E 1=SE 2=S 3=SW 4=W 5=NW 6=N 7=NE; move-driven, fire aim overrides with 500ms hold, zombies follow the steer; updates only on real movement + 2-frame debounce so border jitter never sticks the art); owns state, never touches screen/menus/network/sound (emits `last_event` incl. `revive`); `snapshot/apply_snapshot` move the net state (132B, wave/kills ride u16 so the counter never wraps at 255), P2 ctl comes from `set_p2`
- `lib/sprites` — header-only PROGMEM RGB565 actor art (16px, boss 32px); one header per actor (`player1/2` N/S/E/NE/SE, `zombie`/`zombie_runner` N/S/E + `zombie_boss` N/S/E 32px; W side via hflip, zombie diagonals fold to E/W), each self-contained (`<cstdint>` + `<pgmspace.h>`) and pulled in only via the `sprites.h` fan-out, never a leaf directly
- `lib/render` — camera (focus player per board: host/solo P1, client P2; downed frames its body, bled-out spectates the living partner until respawn), terrain repaint (never wipes the HUD strip: game owns it via cache), actors as one-burst RAM composites (`_blit_buf` + `push_image`, terrain under transparent pixels) centred on the hitboxes (16px, boss 32px on the same 6px core; facing selector onto stored art + hflip, single frame; downed keeps its sprite + yellow frame), erase bounded to the opaque art bbox (`_art_bbox`, cached per pointer; downed erases full rect for the frame) + shop price tags (world-fixed anchors scanned once, erase only when stale via `_tags_stale`: cut, moved cam, price change, rebuild; paint every frame so actor/prompt erases self-heal) + centred prompt strip last (on top of the boss; its erase runs only with a prompt or rebuild pending); reads `sim::view()` + `tilemap` (tags are tile-anchored)
- `lib/panel` — bottom strip: `POINTS` size 2 + HP(solo)/HP1+HP2(co-op, `DOWN n` while bleeding, red at ≤2HP)/DMG/SPD pips + 2px/tile minimap (P1 white, P2 cyan, downed yellow, dead hidden till respawn); `draw()` cached (repaints only on value change, `init()` invalidates), `blips()` per frame; reads `render` + `sim::view()`
- `lib/buzz` — passive-buzzer jingles, non-blocking (`update(now)`); `game` fires it from `sim::last_event`
- `lib/points` — RTC-backed last-4 runs `{pts,kills,wave}` recent-first, mirrored to `/z32.json` on microSD; same callers (`load/add_run(kills,wallet,wave)/history/history_len`)
- `lib/screens` — `id` enum + item tables + menu chrome
- `lib/game` — state machine + input edges + **frame order**; only place calling sim + render + panel + screens + buzz together; HUD cached (wave/kills/gun/role, `_hud_first` forces wipe+repaint after menu chrome)
- `src/main.cpp` — `game::begin(DEVICE_ROLE)` + `game::update()`
- `test/test_native` — host-side tilemap/camera checks + net wire checks; `./test/test_native/run.sh` builds `map_test.cpp` + `net_test.cpp` with plain `g++` (binaries to `$TMPDIR`, no Unity); `test_ignore = test_native` keeps `pio test` off it

Libraries resolve via LDF `chain`. Every `lib/*/src/*.cpp` compiles always; cross-library includes use `<>`, same-directory use `""`. TFT config repo-wide from `[env]`: `-D USER_SETUP_LOADED` + `-include tft_setup.h`. Filenames case-sensitive on Linux.

## Display (TFT)

- glass 240x320 ST7789; world 960x480 (60x30 x 16px); bands: HUD 10px (`W+K` left, `GUN` centre, role badge right, owned by `game`), arena 160px, panel 70px (10+160+70 = 240 exactly)
- arena 320x160, camera clamped x[0,640] y[0,320] → exact **3x3 grid**, x{0,320,640} y{0,160,320}; hard-cut by cell (`_cell_cam`), repaint **80 rows/frame** before sprites
- frame pacing is a target deadline (`_frame_ms` 33), not `delay(33)`
- menus never repaint per frame: `screens::paint(scr, sel)` full-paints on entry, then only the two cursor lines; items are size 2 (12x16 glyphs) at `y0 + i*24` (`_menu_y` 96, footer +16), `_item` takes absolute y; zombie chrome (lime titles/cursor, gray rows/hints, red over/logo, points-style verbs pinned at 224); stale `sel` wraps via `% count`; anything entering `playing` calls `screens::invalidate()`
- menu wrap reads `screens::count(scr)`; `static_assert` pins one table row per `screens::id`
- erase is terrain-exact (actors via single-burst `_erase_box` over the opaque art bbox, bullets/prompt strip via `_erase_world_rect` runs over `tilemap::color_at`); frame order `render::clear()` → `sim::step()` → `render::update_camera()` → `render::draw()` owned by `game`; all arena fills clip to `[10,170)`
- `test/test_native` never compiles `Game.cpp`/sim/render/panel/screens (need Arduino); verify menu geometry on glass

## World / sim contracts

- `_art` rows exactly `COLS` chars; map: meadow + 2-tile ring lanes, maze everywhere, 4 dispersed vendings + 4 roulette pads (casino 4-colour wheel, 1 active per 3-wave epoch via `hash(wave/3) % pads` from wave 3, dead pads tagless); 1240 walkable (69%), 0 orphans, 8/8 machine blocks reachable (per-block: wall-embedded corners allowed), BFS max 76, 0 local minima (all in `run.sh`)
- machine sprites must read at 32px (ASCII dump of `color_at`, never by eye)
- `tilemap::solid_rect` gates movement per axis (X then Y); bullets die on non-walkable
- zombie spawns: random-offset scan, first walkable tile >= 100px away
- BFS `field` rebuilds only when the player's **tile** changes (one field per player); `_zombie_steer` descends the nearest alive player's field to the best of 8 neighbours' centres (3 retries, direct chase on `UNREACHABLE` and terminal homing on the player's tile, standoff ring at `contact_dist` so zombies stop at hit range instead of piling onto the centre, contact tested post-move same-frame; one pairwise declump pass at 7px keeps stacked bodies side by side); pass `float&` members (never copies) to `_move_entity`
- `sim` exposes one read-only `view()` (`reset()`, `step(now)->bool`, `set_p2/set_p2_active` for the peer, `revive(p)` for lifts); death (nobody standing: downed doesn't count) reported by return value, screens raised by caller; points zeroed only in `reset()`; bled-out respawn at the next wave, downed rise at 1 HP
- economy: points are the spendable wallet (shared co-op); each death stores wallet/kills/wave; `render`/`panel` never move state; `game` owns shop proximity + `INTERACT` edge per player + `buzz` firing from `last_event`

## Shop (F1) — agreed prices/stats

- vending (proximity + `INTERACT`, one machine per buff): **H heal green 100** (+2 HP), **D damage red +25%/lvl max5 base 150**, **S speed blue +8%/lvl max5 base 120**, **C rapid orange −8% cooldown/lvl max5 base 150** (min 50ms); level price = base + 200·lvl; denied/MAX hints; live price tags (`DMG 650`) float over the machines
- roulette 100 → weighted weapon (MP9 30 / Glock-19 10 / SPAS-12 25 / AR-15 12 / FAMAS 13 / M82A1 10); weapons come only from roulette, never bought directly; start Glock-19 (dmg1/cd500); MP9 (dmg1/cd180); SPAS-12 (3 pellets/cd900); AR-15 (dmg4/cd550); FAMAS (3-round burst dmg1 at 100ms gaps/cd600, host-side ticks, no net bytes); M82A1 (dmg6/cd1400/range220); 4 pads, 1 active per 3-wave epoch via `hash(wave/3) % pads` from wave 3 (LCG step, host+client agree, no net bytes), banner only on a real move, dead pads prompt `UNAVAILABLE`
- INTERACT reach 28px from machine centre (`_shop_r`): buys through a wall no longer register; prompts unchanged, no prices in proximity text
- HUD shows `W+K/GUN`/badge (focus gun per board), panel shows `POINTS` + HP/DMG/SPD/ROF pips (focus build per board; text, no sprites; co-op pitch 10px so HP2+ROF end at 240); prompt is the arena-centred strip owned by `render`
- zombies: total `min(wave+3,8)`; normal (spd40/hp 2+wave/2/dmg1, +10+2·wave pts, red), **runner** (spd70/hp 1+wave/6 —2 hits desde w6—/dmg1, +15+2·wave pts, orange; 0 en w1, luego `min(wave/2,total/2)`), **boss** (spd30/hp 20+wave —25 en w5, 30 en w10—/dmg2, +150+10·wave pts —200 en w5—, purple, roba slot 0 cada `wave%5==0`); `render`+`panel` colorean por `actor.kind`

## Roadmap

- **F1 shop+roulette** ✅ done: `sim::state` += `weapon/dmg_lvl/spd_lvl/points/last_event` (+`actor.kind`); `game` proximity+buy; `panel` pips+`GUN`; verify exact-points buys, levels, wallet on glass
- **F2 Z32+intro+screens** ✅ done: `z32` title strings (repo path unchanged); `screens::id` += `logo` → `team` (both centred chrome, 2.5s timed/FIRE-skippable; team lists 5 ASCII names) → `menu`
- **F3 buzzer** ✅ done: `lib/buzz` on GPIO 26 via LEDC (ch 0), non-blocking sequencer (`update(now)`); jingles menu/shoot/buy/roulette/hurt/wave/game-over (+denied), fired from `last_event`
- **F4 runners+boss** ✅ done: kinds (normal spd40/hp 2+wave/2/dmg1/pts 10+2·wave / runner spd70/hp 1+wave/6/dmg1/pts 15+2·wave / boss spd30/hp 20+wave/dmg2/pts 150+10·wave), waves (total `min(wave+3,8)`; runners 0 en w1, luego `min(wave/2,total/2)`; boss roba slot 0 cada `wave%5==0`), colours red/orange/purple (`render`+`panel` por `actor.kind`), cap-8 slots; balance on glass
- **F5 microSD**: share TFT SPI + CS22; `Points.cpp` → JSON run history; same 3 functions; needs hardware
- **F6 net co-op** (implemented, needs 2-board test): packed `game_state` 132B (u16 qpos + bit flags incl. downed/bleed/facing, u16 wave/kills, no floats; meta += game-owned `screen`+`sel` for the pause mirror, sim leaves them alone) + `player_input` 5B; host authoritative (sims both, broadcasts ~30Hz), client sends inputs + mirrors snapshots; Solo local on both boards and silent (`playing`/`game_over` never branch on role when `!_net_multi`, focus always P1), Multi via `waiting` (host joins on heartbeat, client joins on live snapshot w/ `_cli_last_rx` grace, peer/10s-timeout/FIRE-solo); P2 shares wallet/kills, own gun/build (joins fresh pistol/0), heals self, downed→revive-to-3HP or wave-respawn; multi focus per board (`render::set_focus` host P1/client P2, HUD/panel/tags show the focus build); pause is host-owned and screen-driven on the client (`_mirror_pause` paints the host cursor, never steps it; PAUSE toggles from either board, Continue/Restart/Exit stay host-only); game-over is the same full mirror; client quiet 3s → menu. Test: Solo OK on both envs; waiting timeout solo; 2 boards join/move/P2-kill/down-revive/pause-from-either/resume-from-either/restart/exit/over, 0 `delivery failed`
- **F7 facing+sprites** ✅ done: 8-wind `facing` per player/actor (move + 500ms aim hold, zombie steer; `dir_of` 0=E..7=NE), 3-bit in spare net flags (132B intact); `render` facing selector onto stored art + hflip (players N/S/E/NE/SE, zombies N/S/E folded by halves: northbound N, southbound S, pure E/W profile, W mirrored symmetric 45°); one header per actor in `lib/sprites` (boss native 32px via `BOSS_ART`)

## Add a message

- add `msg_type` in `lib/protocol/src/Protocol.h`, packed struct with `type` first, `on_message(type, cb)` in `lib/game/src/Game.cpp`, send via `_handler.send(&msg, sizeof(msg))`
