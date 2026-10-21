// Аппаратный watchdog mbed. Запускается в конце setup() с таймаутом по умолчанию;
// любой цикл ожидания в основном потоке (Wi-Fi, HTTP-сервер) должен внутри вызывать
// kickWatchdog(). Рабочий поток FetchWorker его не вызывает: иначе зависший loop()
// оставался бы незамеченным, пока идет запрос (D-026).
#pragma once

#include <drivers/Watchdog.h>

inline void kickWatchdog() {
    mbed::Watchdog::get_instance().kick();
}

// Запуск watchdog. Печатает фактический таймаут с тегом [WATCHDOG] или ошибку.
void startHardwareWatchdog();
