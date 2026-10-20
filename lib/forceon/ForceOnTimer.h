// Таймер принудительного включения канала (D-006, D-007, docs/SCENARIOS.md §6).
// Чистый модуль L0: без Arduino, не логирует. Время — параметр nowMs (millis()).
//
// Отсчет идет по времени работы устройства, а не по epoch: таймер работает без NTP
// и без интернета. Сравнения только как `nowMs - start >= duration`, поэтому
// переполнение millis() через ~49 дней не мешает. После перезагрузки таймер пуст:
// принудительное включение не возобновляется.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace forceon {

constexpr uint8_t kMinHours = 1;
constexpr uint8_t kMaxHours = 24;

class ForceOnTimer {
public:
    // Запустить или перезапустить на полные hours часов (вне 1…24 — к ближайшей границе).
    void start(uint32_t nowMs, uint8_t hours);
    void stop() { active_ = false; }
    bool active() const { return active_; }
    // Время истекло: таймер останавливается и возвращает true один раз.
    bool expire(uint32_t nowMs);
    // Сколько осталось; 0, если таймер не активен или время вышло.
    uint32_t remainingMs(uint32_t nowMs) const;

private:
    bool active_ = false;
    uint32_t startedMs_ = 0;
    uint32_t durationMs_ = 0;
};

// Оставшееся время для страницы и консоли: "2 ч 05 мин", "45 мин". Минуты
// округляются вверх, чтобы при работающем таймере не показывать "0 мин".
// out — не меньше 24 байт (UTF-8).
void formatRemaining(uint32_t remainingMs, char* out, size_t cap);

}  // namespace forceon
