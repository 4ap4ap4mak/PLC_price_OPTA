// История переключений каналов: кольцевой буфер в RAM (D-019). Живет до перезапуска
// или пропадания питания, во flash не пишется. Нужна, чтобы проверить, выполнялись ли
// условия сценариев: каждое событие хранит причину.
// Чистый модуль L0: без Arduino, без динамической памяти, не логирует.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace history {

// Номер канала для событий устройства (запуск), а не реле.
constexpr uint8_t kDeviceEvent = 0xFF;

enum class Cause : uint8_t {
    Startup,      // устройство запущено (событие устройства)
    Threshold,
    Smart,
    Wakeup,
    Night,
    Block,
    Holiday,
    HolidayOff,   // отпуск: интервал не выбран
    NoScenario,   // ни один сценарий не выбрал интервал
    Ceiling,      // цена выше аварийного предела
    Failsafe,     // нет актуальной цены больше часа
    Manual        // кнопка USER или команда консоли
};

struct SwitchEvent {
    uint32_t epoch;          // UTC; 0 — время еще не было синхронизировано
    uint32_t uptimeSeconds;  // секунды от запуска
    uint8_t channel;         // индекс канала или kDeviceEvent
    bool on;
    Cause cause;
};

class SwitchHistory {
public:
    static constexpr size_t kCapacity = 512;

    // Новое событие вытесняет самое старое, когда буфер заполнен.
    void record(uint32_t epoch, uint32_t uptimeSeconds, uint8_t channel, bool on, Cause cause);
    // Проставить время событиям, записанным до синхронизации часов.
    void fillMissingEpochs(uint32_t nowEpoch, uint32_t nowUptimeSeconds);

    size_t size() const { return count_; }
    // Сколько старых событий вытеснено с запуска.
    uint32_t dropped() const { return dropped_; }
    // index 0 — самое новое событие.
    const SwitchEvent& newest(size_t index) const;

private:
    SwitchEvent events_[kCapacity];
    size_t next_ = 0;
    size_t count_ = 0;
    uint32_t dropped_ = 0;
};

const char* causeName(Cause cause);
// Время события: "20.10.2026 03:15:07" по Риге или "+42 с от запуска", если часы
// тогда еще не были синхронизированы. out — не меньше 24 байт.
void formatEventTime(const SwitchEvent& event, char* out, size_t cap);

}  // namespace history
