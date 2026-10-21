// Часы устройства: epoch UTC по NTP (pool.ntp.org) плюс millis() с момента
// синхронизации. Реализует controller::Clock.
//
// sync() блокирует до 1,5 с, пока ждет ответ сервера (KI-006; неблокирующий NTP —
// шаг 2g). Когда повторять синхронизацию, решает вызывающий (main.cpp).
#pragma once

#include <Arduino.h>

#include <ControllerPorts.h>

class SystemClock : public controller::Clock {
public:
    // Вызывается при первой удачной синхронизации: события, записанные до нее,
    // получают время задним числом.
    using FirstSyncHandler = void (*)(uint32_t epoch, uint32_t uptimeSeconds);

    SystemClock(arduino::UDP& udp, FirstSyncHandler onFirstSync)
        : udp_(udp), onFirstSync_(onFirstSync) {}

    // Запрос времени у NTP-сервера. Печатает результат с тегом [NTP].
    bool sync();
    bool hasAttemptedSync() const { return hasAttemptedSync_; }
    uint32_t lastSyncAttemptMs() const { return lastSyncAttemptMs_; }

    bool synced() const override { return synced_; }
    uint32_t nowUtc() const override;
    uint32_t uptimeMs() const override { return millis(); }

private:
    arduino::UDP& udp_;
    FirstSyncHandler onFirstSync_;
    bool udpStarted_ = false;
    bool hasAttemptedSync_ = false;
    bool synced_ = false;
    uint32_t lastSyncAttemptMs_ = 0;
    uint32_t syncedEpoch_ = 0;
    uint32_t syncedAtMs_ = 0;
};
