#include "Watchdog.h"

#include <Arduino.h>

bool startHardwareWatchdog() {
    mbed::Watchdog& watchdog = mbed::Watchdog::get_instance();
    return watchdog.is_running() || watchdog.start();
}

void reportHardwareWatchdog() {
    mbed::Watchdog& watchdog = mbed::Watchdog::get_instance();
    if (!watchdog.is_running() && !watchdog.start()) {
        Serial.println("[ERROR] Не удалось запустить аппаратный watchdog.");
        return;
    }

    Serial.print("[WATCHDOG] Аппаратный watchdog активен, таймаут ");
    Serial.print(watchdog.get_timeout());
    Serial.println(" мс.");
}
