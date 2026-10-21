// Порты контроллера каналов: через них Controller получает время, пишет выходы,
// запрашивает цены и печатает лог. На устройстве их реализуют классы src/platform/
// и src/net/FetchWorker, в тестах env:native — фейки.
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

// Состояние фонового запроса PriceSource.
enum class FetchStatus : uint8_t {
    Idle,    // запроса нет (результат уже отдан)
    Busy,    // запрос идет
    Done,    // успех; отдается один раз, затем Idle
    Failed   // неудача (причина уже в логе); отдается один раз, затем Idle
};

// Источник цен Elering. С шага 2g асинхронный (D-008): start* только ставит запрос,
// сам запрос идет в фоне (на устройстве — поток FetchWorker), Controller забирает
// результат через poll() из loop(). Одновременно идет не больше одного запроса.
// Реализация сама печатает ход запроса и ошибки ([HTTP], [ERROR], [FORECAST]).
class PriceSource {
public:
    virtual ~PriceSource() = default;
    // Текущая цена (/api/nps/price/lv/current). Актуальность цены источник не
    // проверяет: это делает Controller по часам в момент результата.
    // false — предыдущий запрос еще не закончен.
    virtual bool startCurrent() = 0;
    // Цены за период path → out. До результата out принадлежит источнику: вызывающий
    // его не читает и не меняет. При неудаче out не определен. false — занято.
    virtual bool startPrices(const char* path, prices::PriceSeries& out) = 0;
    // Результат последнего запроса. eurKwh и timestamp заполняются при Done
    // запроса текущей цены.
    virtual FetchStatus poll(float& eurKwh, long& timestamp) = 0;
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
