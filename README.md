# z32

Juego estilo COD Zombies para ESP32 con pantalla TFT de 2.4" (320x240). Sobrevive oleadas de
zombis en un laberinto, gana puntos por cada baja y gástalos en máquinas expendedoras para
curarte, subir daño y velocidad, o prueba suerte en la ruleta para conseguir mejores armas.

## Características

- Supervivencia por oleadas infinitas (4 zombis en ronda 1 hasta 8 simultáneos).
- 6 armas: Glock-19 inicial + MP9, SPAS-12 (3 perdigones), SKS, FAMAS (ráfaga de 3) y M82A1 (daño 6, más alcance) por ruleta (la Glock puede volver como premio tonto).
- 3 máquinas expendedoras con niveles permanentes + ruleta de armas.
- Puntos como cartera: ganas por matar, gastas en tienda; al morir se guardan puntos, ronda y bajas.
- Minimapa en vivo con posición, zombis y encuadre de cámara.
- Últimas 4 partidas (puntos, ronda y bajas) guardadas entre partidas (memoria RTC, sobreviven al sueño profundo).
- Menús: inicio, modo de juego, puntos, pausa y game over. Botón de apagado con sueño profundo.

## Mecánicas

- **Oleadas**: cada ronda trae `ronda + 3` zombis (máximo 8 a la vez). Al limpiarlos llega la siguiente.
- **Zombis**: vida `2 + ronda/2` (r1=2, r6=5, r10=7). Persiguen por el laberinto y quitan 1 HP
  por contacto (con 0.5s de inmunidad entre golpes). Jugador con 5 HP.
- **Puntos**: cada baja paga `10 + 2·ronda` (r1=12 … r6=22). Gastar baja tu cartera; al morir
  se guardan los puntos que tenías (ganado menos gastado), la ronda y las bajas.
- **Tiendas** (acércate y pulsa INTERACT):
  - **H verde — Heal 100**: +2 HP.
  - **D roja — Daño 150**: +25% de daño por nivel, máximo 5 (pistola pega 2 desde nv2).
  - **S azul — Velocidad 120**: +8% de velocidad por nivel, máximo 5.
  - Cada nivel cuesta más: `base + 200·nivel` (p. ej. daño: 150/350/550/750/950). Niveles
    permanentes por jugador (cada uno arma su build), cartera compartida. Sin puntos
    o al máximo, avisa (`NEED`, `MAX`).
- **Ruleta 100**: arma aleatoria entre MP9 (rápido), SPAS-12 (abanico de 3), SKS
  (daño 3), FAMAS (ráfaga de 3) y M82A1 (daño 6, alcance 220px). La Glock puede
  volver como premio tonto.
- **Disparo**: auto-apuntado al zombi más cercano (alcance 160px, 220px el M82A1).
  Cada arma tiene su cadencia: Glock 0.5s, MP9 0.18s, SPAS-12 0.9s, SKS 0.35s,
  FAMAS ráfagas de 3 cada 0.6s, M82A1 1.4s.
- **Cargadores**: cada arma tiene su mag (Glock 15, MP9 30, SPAS-12 8, SKS 30,
  FAMAS 30, M82A1 10). La recarga es manual con RELOAD (~1s, M82A1 2s, SPAS-12
  1.5s; moverse es libre, disparar no) y el gatillo en vacío suena a click sin
  auto-rescate. Arma nueva de ruleta llega cargada.

## Controles

| Entrada   | Acción                                              |
| --------- | --------------------------------------------------- |
| Joystick  | Moverse (también navega por los menús)              |
| FIRE      | Disparar / confirmar en menús                       |
| INTERACT  | Comprar en tiendas y ruleta                         |
| RELOAD    | Recargar el cargador (manual, reserva infinita)    |
| PAUSE     | Pausa (Continuar / Reiniciar / Salir); salir = menú |

## Pantalla (UI)

- **Arriba**: HUD con ronda/bajas (`WAVES 3 KILLS 12`), arma y balas (`GUN MP9 18`); arena
  de juego con etiquetas de precio sobre cada máquina (`HEAL 100`, …) y
  franja central inferior con el prompt de compra (`E: DMG LV3 - 650`, `HEALED +2HP`…,
  `OUT OF AMMO!` cuando toca).
- **Abajo (panel)**: `POINTS`, ronda y bajas (`W3 K12`), arma (`GUN MP9`), pips de
  `HP` (5, en rojo si quedan ≤2), `DMG` y `SPD` (5 niveles) + minimapa con tu punto
  blanco, zombis rojos y marco amarillo de cámara.

## Hardware (ESP32)

- TFT ST7789 SPI: CS 5, RST 4, DC 2, MOSI 23, SCLK 18, MISO 19, BL 21.
- Joystick analógico: X 32, Y 33. Botones: FIRE 13, RELOAD 14, INTERACT 15, PAUSE 27.
- Buzzer pasivo GPIO 26 y microSD (CS 22, comparte SPI) previstos.
- Compilar: `pio run -e host` | `pio run -e client`.

## En camino

- Más enemigos: *runner* rápido y *boss* cada 5 rondas.
- Sonido del buzzer (disparos, compras, oleadas…).
- Intro, pantallas de logo/equipo y guardado en microSD.
- Multijugador ESP-NOW (un ESP32 hace de host autoritario).
