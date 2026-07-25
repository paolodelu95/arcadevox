// status_led.h — animazioni sul LED RGB WS2812 saldato sulla DevKitC-1 (GPIO 48).
#pragma once

#include <Arduino.h>

namespace StatusLed {

// Spegne subito il LED e prepara le animazioni.
void begin();

// Da chiamare ad ogni giro di loop(): si auto-limita a ~40 fps.
void update(uint32_t now);

// Luminosita' massima, 0..1 (default 0.20).
void setBrightness(float b);

}  // namespace StatusLed
