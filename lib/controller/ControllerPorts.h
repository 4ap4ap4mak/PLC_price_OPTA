// Порты контроллера каналов: через них Controller получает время, пишет выходы,
// запрашивает цены и печатает лог. На устройстве их реализуют классы src/platform/
// и main.cpp, в тестах env:native — фейки.
//
// Абстракции только там, где Controller нужно подменить в тесте (ARCHITECTURE §4).
// Хранилище настроек — конкретный configstore::ConfigStore: в тесте его подменяет
// фейковый KvBackend.
#pragma once

#include <stdint.h>

#include <PriceSeries.h>

namespace controller {

class Clock {
public:
    virtual ~Clock() = default;
    // Часы синхронизированы по NTP хотя бы раз.
    virtual bool synced() const = 0;
    // Epoch UTC. До синхронизации — секунды от запуска (так считал монолит);
    // вызывающие проверяют synced().
    virtual uint32_t nowUtc() const = 0;
    // millis(): время работы устройства, переполняется через ~49 дней.
    virtual uint32_t uptimeMs() const = 0;
};

// Физические выходы каналов (реле Opta D0–D3). Канал вне диапазона игнорируется.
class Outputs {
public:
    virtual ~Outputs() = default;
    // Пишет выход всегда, даже если состояние не меняется.
    virtual void set(uint8_t channel, bool on) = 0;
    virtual bool get(uint8_t channel) const = 0;
};

// Источник цен Elering. До шага 2g запросы синхронные: блокируют до ~20 с.
// Реализация сама печатает ход запроса и ошибки ([HTTP], [ERROR], [FORECAST]).
class PriceSource {
public:
    virtual ~PriceSource() = default;
    // Текущая цена (/api/nps/price/lv/current). Актуальность проверяется по часам
    // устройства в момент ответа; без синхронизации — не проверяется.
    virtual bool fetchCurrent(float& eurKwh, long& timestamp) = 0;
    // Цены за период path → out. При неудаче out не определен.
    virtual bool fetchPrices(const char* path, prices::PriceSeries& out) = 0;
};

// Лог в Serial. Набор перегрузок повторяет Serial.print/println, поэтому перенесенный
// код печатает те же байты: дробные числа форматирует реализация (на устройстве —
// Serial.print(value, digits)), перевод строки — "\r\n", как у println.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void printText(const char* text) = 0;
    virtual void printSigned(long value) = 0;
    virtual void printUnsigned(unsigned long value) = 0;
    virtual void printFloat(float value, uint8_t digits) = 0;

    void print(const char* text) { printText(text); }
    void print(int value) { printSigned(value); }
    void print(long value) { printSigned(value); }
    void print(unsigned value) { printUnsigned(value); }
    void print(unsigned long value) { printUnsigned(value); }
    void print(float value, uint8_t digits) { printFloat(value, digits); }

    void println(const char* text) { printText(text); printText("\r\n"); }
    void println(long value) { printSigned(value); printText("\r\n"); }
    void println(unsigned long value) { printUnsigned(value); printText("\r\n"); }
};

}  // namespace controller
