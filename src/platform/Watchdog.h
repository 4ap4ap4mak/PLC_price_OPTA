// Аппаратный watchdog mbed. Запускается первым делом в setup() с таймаутом по
// умолчанию: фатальная ошибка при запуске (mbed_die — мигание красным) заканчивается
// перезагрузкой, а не остановкой до снятия питания (D-041). Любой цикл ожидания
// в основном потоке (Wi-Fi, HTTP-сервер) должен внутри вызывать kickWatchdog().
// Рабочий поток FetchWorker его не вызывает: иначе зависший loop() оставался бы
// незамеченным, пока идет запрос (D-026).
#pragma once

#include <drivers/Watchdog.h>

inline void kickWatchdog() {
    mbed::Watchdog::get_instance().kick();
}

// Запуск watchdog без вывода в Serial (до Serial.begin()). false — не запустился.
bool startHardwareWatchdog();
// Печатает фактический таймаут с тегом [WATCHDOG] или ошибку запуска.
void reportHardwareWatchdog();
