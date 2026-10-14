# z32

ESP32 wave-based zombie video game with ESP-NOW multiplayer. Survive endless waves in a maze on a 2.4" TFT (ST7789 240x320), spend kill points on vending machines and a weapon roulette. Solo on one board, co-op on two.

## Hardware

One set per player (co-op needs two identical sets):

- ESP32 dev board, ST7789 240x320 TFT over SPI
- Analog joystick + 4 buttons (FIRE, RELOAD, INTERACT, PAUSE)
- Passive buzzer, microSD card (shares the TFT SPI bus)

All pins are in [include/pins.h](include/pins.h) (single source of truth; display timing in [include/tft_setup.h](include/tft_setup.h). ADC2 is not used for analog input (WiFi/ESP-NOW disables it).

## Installation

Prereqs: [PlatformIO](https://platformio.org/) and ESP32 platform + a USB serial driver for your board. 

Library deps (install automatically by platformio.ini):

- TFT_eSPI
- ArduinoJson

```sh
pio run -e host -t upload    # board 1
pio run -e client -t upload  # board 2
pio device monitor           # 115200 baud; boot prints ready role=host/client
```

Each board's role is fixed at build time. Either board plays Solo alone.

## How to play

**Goal.** Each wave asks for `wave + 3` kills (wave 1 = 4), max 10 zombies alive with instant refill, then 3s of quiet before the next wave. You lose when nobody is left standing; the run is saved to scores.

**Controls.**

| Input    | Action                                                        |
| -------- | ------------------------------------------------------------- |
| Joystick | Move (also navigates menus)                                   |
| FIRE     | Shoot (hold with MP9/AR-15) / confirm                         |
| INTERACT | Buy, roll roulette, revive partner, call for help when downed |
| RELOAD   | Top up a half-spent mag (empty mags reload on their own)      |
| PAUSE    | Pause (Continue / Restart / Exit)                             |

Aim auto-targets the nearest zombie in range (160px, 220px for the M82A1).

**Shop.** Points are the shared wallet: kill, earn, spend. Stand by a machine (28px reach) and press INTERACT.

| Machine   | Effect                              | Price             |
| --------- | ----------------------------------- | ----------------- |
| H Heal    | +2 HP (up to 10)                    | 100 flat          |
| D Damage  | +25% damage per level, max 10       | 150 + 300 × level |
| S Speed   | +8% speed per level, max 10         | 150 + 300 × level |
| C Rapid   | −6% cooldown per level, max 10      | 150 + 300 × level |

Levels are permanent per player; each machine shows its live price overhead.

**Roulette (100).** Random weapon; only 1 of the 4 pads is active at a time (rotates every 3 waves from wave 3). New guns arrive with a full mag.

| Weapon   | Dmg | Rate | Mag | Notes                        |
| -------- | --- | ---- | --- | ---------------------------- |
| Glock-19 | 1   | 0.5s | 15  | starter                      |
| MP9      | 1   | 0.18s| 30  | hold FIRE (odds 30)          |
| SPAS-12  | 2 × 3 pellets | 0.9s | 8 | spread, 1.5s reload (odds 25) |
| AR-15    | 3   | 0.35s| 30  | hold FIRE (odds 12)          |
| FAMAS    | 1 × 3-round burst | 0.6s | 30 | one press = 3 rounds (odds 13) |
| M82A1    | 6   | 1.4s | 10  | 220px range, 2s reload (odds 10) |

**Zombies.** They path to the nearest living player and stop at contact range instead of piling up. 0.4s immunity between hits taken.

| Type   | Color  | Speed | HP            | Dmg | Pays        |
| ------ | ------ | ----- | ------------- | --- | ----------- |
| Normal | red    | 40    | `2 + wave/2`  | 1   | `10 + 2·wave` |
| Runner | orange | 80    | `1 + wave/4`  | 1   | `15 + 2·wave` |
| Boss   | purple | 30    | `20 + wave`   | 3   | `150 + 10·wave` |

No runners on wave 1; afterwards up to `min(2·wave/3, quota/2)`, spread evenly. Every 5th wave a boss takes spawn slot 0. Spawns are always ≥100px away.

**Downed & revive (co-op).** At 0 HP you go down, not out: 15s of bleed while your partner presses INTERACT to lift you back at 3 HP. Bleed out and you respawn next wave. Downed with no machines near, INTERACT shouts `SAVE ME!`.

## Co-op

Host is authoritative: it simulates both players and broadcasts state at ~30Hz (146B state + 5B input); the client sends inputs and mirrors. Pick Multi and the boards join via `waiting` (10s timeout or FIRE falls back to Solo). P2 shares the wallet/kills and joins with a fresh pistol. Pause can be requested from either board but is owned by the host; game-over mirrors to both.

## Development & tests

```sh
pio run -e host | pio run -e client   # build per role
./test/test_native/run.sh             # hardware-free checks (map + net wire)
```

The serial console (115200) logs events only, e.g. run saves and co-op joins. `AGENTS.md` is the developer contract (pins, net format, module bounds).

## License

GPL-2.0, see [LICENSE](LICENSE).
