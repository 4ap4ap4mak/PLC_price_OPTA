// Аппаратный watchdog mbed. Запускается в конце setup() с таймаутом по умолчанию;
// любой цикл ожидания (сеть, HTTP, Wi-Fi) должен внутри вызывать kickWatchdog().
#pragma once

#include <drivers/Watchdog.h>

inline void kickWatchdog() {
    mbed::Watchdog::get_instance().kick();
}

// Запуск watchdog. Печатает фактический таймаут с тегом [WATCHDOG] или ошибку.
void startHardwareWatchdog();
