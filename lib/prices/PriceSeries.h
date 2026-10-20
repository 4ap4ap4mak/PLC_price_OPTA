// Ряд цен Elering по 15-минутным интервалам (слотам) и агрегация в часы.
// Чистый модуль L0: без Arduino, без динамической памяти, не логирует.
// Цены — EUR/kWh, могут быть отрицательными. Слоты хранятся строго по возрастанию
// времени: append() отклоняет слот не позже последнего, на этом держится поиск.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace prices {

constexpr uint32_t kSlotSeconds = 15UL * 60UL;
// 3 суток по 15 минут (вчера, сегодня, завтра — 288) + запас на 25-часовые сутки.
constexpr size_t kMaxSlots = 300;
// Часы для buildHours(): весь ряд с запасом на неполные часы по краям.
constexpr size_t kMaxHours = kMaxSlots / 4 + 2;

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
    // false — ряд заполнен или слот не позже последнего (тогда он не добавлен).
    bool append(uint32_t start, float eurKwh);
    size_t size() const { return count_; }
    const PriceSlot& operator[](size_t i) const { return slots_[i]; }

    // Индекс слота, внутри которого лежит момент epoch, или -1.
    int indexAt(uint32_t epoch) const;
    // Первый слот, который начинается не раньше epoch (size(), если такого нет).
    size_t lowerBound(uint32_t epoch) const;
    // Конец последнего слота; 0 для пустого ряда.
    uint32_t coverageEnd() const;

    // Заменить ряд свежим прогнозом newer, сохранив свои слоты из [keepFrom, начало newer):
    // так вчерашние цены переживают запрос на «сегодня–завтра». Лишнее сверх емкости
    // отбрасывается с конца.
    void replaceKeepingOlder(uint32_t keepFrom, const PriceSeries& newer);
    // Дописать в начало слоты older, которые раньше первого своего слота.
    void prependOlder(const PriceSeries& older);

    // Сгруппировать соседние слоты одного часа UTC. Возвращает число часов (≤ cap).
    size_t buildHours(PriceHour* out, size_t cap) const;

private:
    PriceSlot slots_[kMaxSlots];
    size_t count_ = 0;
};

}  // namespace prices
