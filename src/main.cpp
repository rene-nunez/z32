#include <Game.h>

#ifndef DEVICE_ROLE
  #error "DEVICE_ROLE not defined: use `pio run -e host` or `pio run -e client`"
#endif

void setup() {
  Game::begin(DEVICE_ROLE);
}

void loop() {
  Game::update();
}
