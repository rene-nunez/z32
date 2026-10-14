# AGENTS.md

Gameplay numbers (prices, stats, waves) live in README.md; code truth is the
`lib/sim/src/sim.h` constants. This file is the dev contract only.

## Style

- `snake_case` everywhere; class private members and instances prefixed `_`; read `.editorconfig`

## Network

- packed structs (`__attribute__((packed))`); first field is always `type`
- 250 bytes/msg max; frequent state must be binary, not JSON
- host is authoritative (runs game logic, sends state); client sends inputs
- fixed custom MACs in `lib/network/src/network.h`; role picks the peer
- role comes from build: `pio run -e host` | `-e client` (flag `DEVICE_ROLE`, validated in `src/main.cpp`)

## Pins

- single source of truth: `include/pins.h` (TFT SPI, ADC1 joystick, buttons, buzzer 26, microSD sharing the TFT SPI bus + CS 22)
- buzzer is passive via LEDC (`lib/buzz`, non-blocking); ADC2 never analog (WiFi/ESP-NOW disables it); LEDC on 25/26 is digital, no clash

## Layout

- `include/` — `pins.h`, `tft_setup.h` (ST7789 + `TFT_INVERSION_OFF` + `TFT_RGB_ORDER TFT_BGR`, panel BGR)
- `lib/network` — raw ESP-NOW, no message logic; `lib/protocol` — `msg_type` + packed structs (`net_state.h`: `game_state` 146B + `player_input` 5B + `chat` 4B, Arduino-free so `test_native` checks sizes/round-trips; facing rides spare flag bits); `lib/handler` — typed routing/dispatch
- `lib/display` — `display` + `colour` (+`push_image` one-burst RAM blits); only TFT_eSPI user
- `lib/input` — joystick ADC1 + buttons (debounce + edge); centre calibrated at boot (~200ms, hands off); deadzone 0.3
- `lib/map` — `tilemap` 60x30 maze, BFS `field`/`field2`; `lib/sim` — owns state, never touches screen/menus/network/sound (emits `last_event`); `lib/sprites` — header-only PROGMEM RGB565 art, pulled only via the `sprites.h` fan-out; `lib/render` — camera + terrain + actors + prompt strip, reads `sim::view()` + `tilemap`; `lib/panel` — bottom strip + minimap; `lib/buzz` — jingles fired from `last_event`; `lib/scores` — RTC last-4 + `/z32_recent.json` + `/z32_log.jsonl`; `lib/screens` — `id` enum + item tables + chrome; `lib/game` — state machine, only place calling sim + render + panel + screens + buzz together
- `src/main.cpp` — `game::begin(DEVICE_ROLE)` + `game::update()`
- LDF `chain`; every `lib/*/src/*.cpp` always compiles — a lib's external deps must surface in its public header (`render.h` includes `sprites.h`). Cross-lib `<>`, same-dir `""`. TFT config repo-wide via `[env]` `-D USER_SETUP_LOADED` + `-include tft_setup.h`. Filenames case-sensitive on Linux.

## Display

- glass 240x320 ST7789; world 960x480 (60x30 x16px); bands HUD 10 / arena 160 / panel 70
- arena 320x160, camera clamped to an exact 3x3 grid, hard-cut by cell; repaint 80 rows/frame; frame deadline 33ms (not delay)
- menus paint on entry + cursor lines only; items size 2 at `y0 + i*24`; wrap via `screens::count`; `static_assert` one table row per `screens::id`; entering `playing` calls `screens::invalidate()`
- erase is terrain-exact; frame order `render::clear()` → `sim::step()` → `render::update_camera()` → `render::draw()` owned by `game`; arena fills clip to `[10,170)`
- `test_native` never compiles Game/sim/render/panel/screens (need Arduino); menu geometry verified on glass

## Sim contracts

- `_art` rows exactly `COLS`; map pins: 1230 walkable, 0 orphans, 8/8 machine blocks reachable, BFS max 76, 0 local minima (all in `run.sh`)
- `solid_rect` gates movement per axis (X then Y); bullets die on non-walkable; spawns walkable, ≥100px away
- BFS `field` rebuilds only when the player's tile changes; steer descends the nearest alive player's field (direct chase on UNREACHABLE, standoff ring at contact, one declump pass at 7px); pass `float&` members to `_move_entity`
- API: `view()` / `reset()` / `step(now)->bool` (death = nobody standing) / `set_p2` / `revive(p)`; points zeroed only in `reset()`; silent 3s break between waves (host-only); MP9/AR-15 hold-to-fire, rest edge (cadence limits, no wire change)
- net: `snapshot/apply_snapshot` move 146B (wave/kills u16); `ammo[2]` + `PF_RELOADING` ride flags; `screen`+`sel` mirror bytes are game-owned, sim leaves them alone; roulette active pad derives from `hash(wave/3) % pads` on both ends, no net bytes; voluntary SAVE ME `chat` is triple-sent
- `game` owns shop proximity + INTERACT edges + buzz firing; `render`/`panel` never move state; INTERACT reach 28px (`game::_shop_r`); prompt priority: results > REVIVE > SAVE ME echo > HELP idle > RELOADING > proximity

## Tests

- `./test/test_native/run.sh` builds `map_test.cpp` + `net_test.cpp` with plain `g++` (bins to `$TMPDIR`); `test_ignore = test_native` keeps `pio test` off it

## Roadmap

- F1 shop+roulette / F2 intro+screens / F3 buzzer / F4 runners+boss / F5 microSD (needs hardware) / F6 net multiplayer / F7 facing+sprites — done; F8 quota+refill folded into waves; F9–F13 balance passes baked into current numbers (see README + `sim.h`)

## Add a message

- add `msg_type` in `lib/protocol/src/protocol.h`, packed struct with `type` first, `on_message(type, cb)` in `lib/game/src/game.cpp`, send via `_handler.send(&msg, sizeof(msg))`
