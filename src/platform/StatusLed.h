// Светодиод LED_USER: медленное мигание (1.5 с) — сеть подключена,
// быстрое (200 мс) — подключения нет.
#pragma once

#include <stdint.h>

class StatusLed {
public:
    void begin();
    // Вызывать в каждом loop() с millis() начала итерации.
    void service(uint32_t nowMs, bool connected);

private:
    uint32_t lastToggleMs_ = 0;
};
