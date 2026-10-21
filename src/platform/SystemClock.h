// Часы устройства: epoch UTC по NTP (pool.ntp.org) плюс millis() с момента
// синхронизации. Реализует controller::Clock.
//
// С шага 2g сам запрос NTP идет в рабочем потоке FetchWorker (src/net/NtpQuery),
// main.cpp передает результат в apply(). Когда повторять синхронизацию, решает
// main.cpp. Все вызовы — из основного потока.
#pragma once

#include <Arduino.h>

#include <ControllerPorts.h>

class SystemClock : public controller::Clock {
public:
    // Вызывается при первой удачной синхронизации: события, записанные до нее,
    // получают время задним числом.
    using FirstSyncHandler = void (*)(uint32_t epoch, uint32_t uptimeSeconds);

    explicit SystemClock(FirstSyncHandler onFirstSync) : onFirstSync_(onFirstSync) {}

    // Ответ NTP: epoch UTC в момент atMs (millis()).
    void apply(uint32_t epoch, uint32_t atMs);

    bool synced() const override { return synced_; }
    uint32_t nowUtc() const override;
    uint32_t uptimeMs() const override { return millis(); }

private:
    FirstSyncHandler onFirstSync_;
    bool synced_ = false;
    uint32_t syncedEpoch_ = 0;
    uint32_t syncedAtMs_ = 0;
};
