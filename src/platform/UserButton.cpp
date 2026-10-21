#include "UserButton.h"

#include <Arduino.h>

void UserButton::begin() {
    pinMode(BTN_USER, INPUT_PULLUP);
}

userbutton::Event UserButton::service(uint32_t nowMs) {
    return detector_.update(nowMs, digitalRead(BTN_USER) == LOW);
}
