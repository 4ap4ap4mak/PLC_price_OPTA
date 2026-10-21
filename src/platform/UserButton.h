// Кнопка USER на корпусе Opta (BTN_USER, нажата — LOW). Логика нажатий —
// lib/userbutton.
#pragma once

#include <stdint.h>

#include <PressDetector.h>

class UserButton {
public:
    void begin();
    // Вызывать в каждом loop() с millis() начала итерации.
    userbutton::Event service(uint32_t nowMs);

private:
    userbutton::PressDetector detector_;
};
