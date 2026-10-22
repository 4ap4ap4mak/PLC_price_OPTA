// Разбор ответов API Elering (Nord Pool, зона LV) и путь запроса прогноза.
// Чистый модуль L0: без Arduino и сети, не логирует. Зависит от ArduinoJson 7
// (header-only, собирается и под Opta, и под env:native).
//
// API отдает цены в EUR/MWh, наружу модуль отдает EUR/kWh (деление на 1000).
// Цена может быть отрицательной.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <PriceSeries.h>

namespace elering {

constexpr const char* kHost = "dashboard.elering.ee";
constexpr int kPort = 443;
constexpr const char* kCurrentPath = "/api/nps/price/lv/current";

// Самая ранняя допустимая метка времени цены и NTP: 2025-01-01 00:00 UTC.
constexpr uint32_t kMinValidEpoch = 1735689600UL;
// Цена текущего интервала принимается, только если она не старше часа
// и не опережает часы устройства больше чем на 5 минут (часы Opta могут отставать
// от Elering).
constexpr uint32_t kMaxPriceAgeSeconds = 60UL * 60UL;
constexpr uint32_t kMaxPriceLeadSeconds = 300UL;

enum class Error : uint8_t {
    Ok,
    Json,        // не JSON; текст ошибки ArduinoJson — в jsonError
    Format,      // нет success/data или массив пуст
    NoPrice,     // в первом элементе нет числовой цены или timestamp
    OutOfRange,  // цена вне -1000…10000 EUR/MWh или не число
    BadTimestamp,// timestamp раньше kMinValidEpoch
    Stale,       // цена не относится к текущему времени
    Empty        // прогноз без единого годного слота
};

struct CurrentPrice {
    float eurMwh;     // как в ответе API; заполнено и при OutOfRange
    float eurKwh;
    long timestamp;   // заполнено начиная с OutOfRange
};

// Код ответа из строки статуса HTTP "HTTP/1.1 200 OK" (без \r\n). 0 — строка не
// похожа на строку статуса.
int parseHttpStatus(const char* statusLine);

// Границы JSON-объекта в теле: от первой '{' до последней '}'. false — скобок нет,
// тогда тело разбирается целиком.
bool jsonObjectBounds(const char* body, size_t len, size_t& start, size_t& end);

// Цена с меткой timestamp относится к моменту now (epoch UTC, часы синхронизированы):
// не старше kMaxPriceAgeSeconds и не опережает часы больше kMaxPriceLeadSeconds.
// Это проверка Error::Stale из parseCurrent.
bool priceIsCurrent(long timestamp, uint32_t now);

// Ответ /api/nps/price/lv/current. now == 0 — часы не синхронизированы,
// проверка актуальности пропускается (с шага 2g так разбирает FetchWorker,
// а актуальность по часам в момент результата проверяет Controller).
// jsonError — только при Error::Json.
Error parseCurrent(const char* json, size_t len, uint32_t now,
    CurrentPrice& out, const char*& jsonError);

// Ответ /api/nps/price?start=…&end=…: массив data.lv → out (сначала очищается).
// Остальные зоны (ee, fi, lt) не разбираются — так документ JSON в 4 раза меньше.
// Слоты с timestamp 0, нечисловой ценой или не по порядку пропускаются, лишние
// сверх емкости — тоже.
Error parseForecast(const char* json, size_t len, prices::PriceSeries& out,
    const char*& jsonError);

// Путь запроса прогноза на местные сегодня 00:00 … завтра 23:45 (Europe/Riga),
// границы — в UTC вида 2026-10-19T21:00:00.000Z. false — не хватило места в out.
bool forecastPath(uint32_t now, char* out, size_t cap);
// Путь запроса цен за одни местные сутки localDay: 00:00 … 23:45.
bool dayPricesPath(int64_t localDay, char* out, size_t cap);

}  // namespace elering
