#include <game.h>

void setup() {
  #ifndef DEVICE_ROLE
    #error "DEVICE_ROLE not defined: use `pio run -e host` or `pio run -e client`"
  #endif

  game::begin(DEVICE_ROLE);
}

void loop() {
  game::update();
}
