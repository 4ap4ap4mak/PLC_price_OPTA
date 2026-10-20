// Ряд цен прогноза Elering по 15-минутным слотам и агрегация в часы.
// Чистый модуль L0: без Arduino, без динамической памяти, не логирует.
// Цены — EUR/kWh, могут быть отрицательными. Порядок слотов не проверяется:
// ряд хранит их в том порядке, в каком их отдал API.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace prices {

// 2 суток по 15 минут (192) + запас на 25-часовые сутки при переходе на зимнее время.
constexpr size_t kMaxSlots = 200;

struct PriceSlot {
    uint32_t start;   // epoch UTC начала слота
    float eurKwh;
};

struct PriceHour {
    uint32_t start;    // epoch UTC начала часа
    float avgEurKwh;   // среднее по кварталам часа
    uint8_t quarters;  // 4 → полный час
};

class PriceSeries {
public:
    void clear() { count_ = 0; }
    // false — ряд заполнен, слот не добавлен.
    bool append(uint32_t start, float eurKwh);
    size_t size() const { return count_; }
    const PriceSlot& operator[](size_t i) const { return slots_[i]; }

    // Сгруппировать соседние слоты одного часа UTC. Возвращает число часов (≤ cap).
    size_t buildHours(PriceHour* out, size_t cap) const;

private:
    PriceSlot slots_[kMaxSlots];
    size_t count_ = 0;
};

}  // namespace prices
