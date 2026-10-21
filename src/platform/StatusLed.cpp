#include "StatusLed.h"

#include <Arduino.h>

void StatusLed::begin() {
    pinMode(LED_USER, OUTPUT);
}

void StatusLed::service(uint32_t nowMs, bool connected) {
    const uint32_t interval = connected ? 1500UL : 200UL;
    if (nowMs - lastToggleMs_ > interval) {
        lastToggleMs_ = nowMs;
        digitalWrite(LED_USER, !digitalRead(LED_USER));
    }
}
