# z32

Juego estilo COD Zombies para ESP32 con pantalla TFT de 2.4" (ST7789 240x320).
Sobrevive oleadas infinitas en un laberinto, gana puntos por cada baja y gástalos
en máquinas expendedoras para curarte y subir tu build, o prueba suerte en la
ruleta para conseguir mejores armas. Solo o coop a 2 placas por ESP-NOW.

## Inicio rápido

- Compilar y flashear según el rol de la placa: `pio run -e host` | `pio run -e client`.
- Flujo de pantallas: `logo` → `team` (2.5s cada una, se saltan con FIRE) → `menu` →
  `mode` (Solo / Multi) → `scores` / `waiting` → `playing` → `pause` / `game_over`.
- En Multi, si no aparece el compañero en 10s (o pulsas FIRE en la espera),
  entras en Solo.

## Objetivo

- Cada ronda pide `ronda + 3` bajas (ronda 1 = 4). Hay 10 zombis vivos como máximo
  con refill inmediato en el slot liberado, y 3s de calma silenciosa al limpiar
  la cuota antes de la siguiente oleada.
- Pierdes cuando nadie queda en pie (los caídos no cuentan): sale `game_over`
  y tu run se guarda.
- Solo hay un mundo y una vida por oleada: aguanta, gasta bien y no te encierren.

## Controles

| Entrada  | Acción                                                        |
| -------- | ------------------------------------------------------------- |
| Joystick | Moverse (también navega por los menús)                         |
| FIRE     | Disparar (mantener con MP9/AR-15) / confirmar                  |
| INTERACT | Comprar en tiendas y ruleta / revivir al compañero / pedir ayuda caído |
| RELOAD   | Rematar el cargador a medias (el vacío recarga solo)           |
| PAUSE    | Pausa (Continuar / Reiniciar / Salir); salir = menú            |

El centro del joystick se calibra al arrancar (~200ms, manos quietas) y tiene
zona muerta, así que el reposo nunca mueve al jugador.

## El mundo

- Laberinto de 60x30 tiles de 16px (mundo 960x480): suelo de pradera (lo único
  caminable) y muros de concreto. 1230 tiles caminables (68%), sin huérfanos.
- 4 máquinas expendedoras de 2x2 (H/D/S/C) + 4 ruletas de 2x2. Las máquinas son
  sólidas: hay que comprar pegado a ellas (alcance 28px desde el centro).
- La cámara sigue a tu jugador en una cuadrícula exacta de 3x3 con cortes duros
  entre celdas; la arena repinta 80 filas por frame con objetivo de 33ms.

## Zombis

Persiguen por BFS al jugador vivo más cercano, paran a distancia de contacto en
vez de apilarse y se desempatan entre ellos. Hay 0.4s de inmunidad entre golpes
recibidos.

| Tipo   | Color  | Vel.  | Vida                  | Daño | Paga               |
| ------ | ------ | ----- | --------------------- | ---- | ------------------ |
| Normal | rojo   | 40    | `2 + ronda/2`         | 1    | `10 + 2·ronda`     |
| Runner | naranja| 80    | `1 + ronda/4`         | 1    | `15 + 2·ronda`     |
| Boss   | morado | 30    | `20 + ronda`          | 3    | `150 + 10·ronda`   |

- Runners: 0 en ronda 1, luego `min(2·ronda/3, cuota/2)` repartidos uniformes en
  la cuota (nunca te encierran al abrir).
- Boss: cada ronda múltiplo de 5 roba el slot 0 de spawn.
- Spawns a ≥100px del jugador, en tile caminable aleatorio.

## Puntos y tiendas

Los puntos son la cartera compartida (también en coop): matas, cobras y gastas.
Acércate a la máquina y pulsa INTERACT. Sin puntos o al máximo, avisa
(`NEED 450 PTS`, `DMG MAX`). Cada máquina muestra su precio flotando encima.

| Máq. | Color   | Efecto                                            | Precio |
| ---- | ------- | ------------------------------------------------- | ------ |
| H Heal   | verde   | +2 HP (hasta 10)                              | 100 fijo |
| D Daño   | roja    | +25% daño por nivel, máx. 10 (panel: `DMG +75%!`) | base 150 + 300·nivel |
| S Velocidad | azul | +8% velocidad por nivel, máx. 10              | base 150 + 300·nivel |
| C Cadencia | naranja | −6% cooldown por nivel (multiplicativo), máx. 10 | base 150 + 300·nivel |

Niveles: `150/450/750/…/2850`, rama completa 15000, build completo 45000.
Los niveles son permanentes por jugador (cada uno arma su build) y la cartera
es compartida.

## Ruleta y armas

- **Ruleta 100**: arma aleatoria. Solo hay 1 pad activo cada 3 oleadas
  (desde la 3, por hash acordado entre placas); los apagados responden
  `NO LUCK HERE`. Si se mueve, anuncia `ROLL RELOCATED!`.
- Probabilidades: MP9 30 / Glock-19 10 / SPAS-12 25 / AR-15 12 / FAMAS 13 / M82A1 10.
  La Glock puede volver como premio tonto. Empiezas con Glock-19.
- El disparo apunta solo al zombi más cercano (alcance 160px, 220px el M82A1).
  Solo MP9 y AR-15 disparan manteniendo FIRE; el resto es tiro a tiro.

| Arma    | Daño        | Cadencia | Mag | Notas                          |
| ------- | ----------- | -------- | --- | ------------------------------ |
| Glock-19| 1           | 0.5s     | 15  | inicial                        |
| MP9     | 1           | 0.18s    | 30  | auto manteniendo FIRE          |
| SPAS-12 | 2 por perdigón (3×2, 6/gatillo) | 0.9s | 8 | abanico, recarga 1.5s |
| AR-15   | 3           | 0.35s    | 30  | auto manteniendo FIRE          |
| FAMAS   | 1 × ráfaga de 3 (cada 100ms) | 0.6s | 30 | un toque = 3 balas    |
| M82A1   | 6           | 1.4s     | 10  | alcance 220px, recarga 2s      |

- **Cargadores**: recarga auto al vaciar (~1s, M82A1 2s, SPAS-12 1.5s; moverse es
  libre, disparar no, avisa `RELOADING...`) y manual con RELOAD para rematar a
  medias. Arma nueva de ruleta llega cargada. La ruleta y la ráfaga cancelan
  la recarga en curso.
- El daño escala con tu nivel: `base·(1 + 0.25·nivel)`.

## Caído y revive (coop)

- A 0 HP caes (no mueres): sangras 15s (`DOWN n` en el panel, marco amarillo).
- Tu compañero te levanta con INTERACT (`PRESS INT TO REVIVE`) y vuelves con 3 HP
  (`THX!`). Si sangras del todo (`I'M OUT!`), respawneas en la siguiente oleada
  (`I'M BACK!`); si aguantaste caído hasta el cambio de oleada, también te levantas
  con 3 HP.
- Caído y sin máquinas cerca, INTERACT grita `SAVE ME!`; quieto sale
  `PRESS INT FOR HELP`. Los avisos salen en el color del que habla
  (P1 verde, P2 azul acero).
- El caído conserva su arma y build; el muerto especta al compañero (cámara, HUD
  y panel muestran su arma/build) hasta respawnear.

## Pantalla (UI)

- **Arriba (HUD 10px)**: `WAVE/KILLS` a la izquierda, `GUN + balas` en el centro
  y rol a la derecha. Cada placa muestra su propio foco (host P1, cliente P2).
- **Arena (160px)**: etiquetas de precio sobre cada máquina (`HEAL 100`, …) y
  franja central de avisos con prioridad: resultados (`THX!`/`I'M OUT!`/`I'M BACK!`)
  > `PRESS INT TO REVIVE` > `SAVE ME!` > `PRESS INT FOR HELP` > `RELOADING...` >
  proximidad (`GET DMG +75%`, `GET ROLL 100`…) / `NO LUCK HERE` / jefes.
- **Abajo (panel 70px)**: `POINTS` grande + pips de `HP` (10, en rojo con ≤4;
  en coop `HP1+HP2` con `DOWN n` sangrando) + `DMG/SPD/ROF` como `%` real de tu
  build + minimapa de 2px/tile (tú verde, P2 azul, rojo en crítico, caído
  amarillo, zombis rojo/naranja/morado según tipo, ruleta activa en lima y marco
  amarillo de cámara).

## Coop (2 placas, ESP-NOW)

- Host autoritario: simula ambas placas y emite estado a ~30Hz
  (`game_state` 146B + `player_input` 5B); el cliente envía inputs y espeja.
- Solo es local y silencioso en ambas placas. Multi vía `waiting`: el host entra
  con el latido del peer, el cliente con el primer snapshot vivo; timeout 10s o
  FIRE para ir Solo; si el cliente pierde al host 3s, vuelve al menú.
- P2 comparte cartera/kills y entra con pistola y build fresh; se cura a sí mismo
  y revive igual. La pausa es del host pero se puede pedir desde cualquiera
  (Continuar/Reiniciar/Salir solo en host) y el game-over se espeja completo.

## Sonido

Buzzer pasivo en GPIO 26 (no bloqueante): menú, disparo, compra, ruleta, daño,
oleada, game-over, denegado, intro y recarga.

## Scores

Cada muerte guarda `{puntos, bajas, oleada}` en tu propia placa: últimas 4 en
memoria RTC + `/z32_recent.json` en microSD, e historial completo en
`/z32_log.jsonl` (un JSON por línea). Pantalla `scores` en el menú.

## Hardware (ESP32)

- TFT ST7789 SPI: CS 5, RST 4, DC 2, MOSI 23, SCLK 18, MISO 19, BL 21.
- Joystick analógico: X 32, Y 33. Botones: FIRE 13, RELOAD 14, INTERACT 15, PAUSE 27.
- Buzzer pasivo GPIO 26. microSD compartiendo el SPI del TFT + CS 22.
- ADC2 no se usa para analógico (lo inhabilita el WiFi/ESP-NOW).

## Compilar y depurar

- Compilar: `pio run -e host` | `pio run -e client` (el rol viene del build).
- Tests nativos (sin hardware): `./test/test_native/run.sh` (tilemap/cámara + wire).
- Consola serial (115200): solo eventos, nada por frame. Boot: `ready role=host/client`,
  `microsd ok` (o `no sd, rtc only`). Cada muerte anuncia la línea idéntica a la
  del historial: `run saved {"p":1250,"k":42,"w":5}`. En coop: `joined multi`,
  `waiting timeout`, `peer quiet 3000ms` y `pause from peer`.
