// Strength Tank LED wall. See ../design.md and include/wall_shared.h.
#include <Arduino.h>

#include "wall_shared.h"

void setup() {
  Serial.begin(115200);
  wall::display_begin();  // first: the DMA buffers need contiguous internal RAM
  wall::net_begin();
}

void loop() {
  wall::net_loop();
  delay(10);
}
