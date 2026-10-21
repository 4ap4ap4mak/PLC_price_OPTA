// Цены Elering по TLS (Wi-Fi): синхронные запросы текущей цены и цен за период.
// Разбор JSON — lib/elering, здесь — соединение, HTTP-запрос и лог [HTTP]/[ERROR].
//
// С шага 2g вызывается только из рабочего потока FetchWorker: каждый запрос блокирует
// поток до ~20 с, loop() при этом работает. Поэтому watchdog здесь не сбрасывается —
// его сбрасывает только loop(), а зависший запрос ловит FetchWorker::stuck() (D-026).
// Лог пишется в log (буфер строк FetchWorker), а не в Serial напрямую.
#pragma once

#include <Arduino.h>
#include <WiFiSSLClient.h>

#include <PriceSeries.h>

class EleringPriceSource {
public:
    explicit EleringPriceSource(Print& log) : log_(log) {}

    // Текущая цена. Актуальность не проверяется (часы — у основного потока): это
    // делает Controller по часам в момент результата.
    bool fetchCurrent(float& eurKwh, long& timestamp);
    // Цены за период path → out. При неудаче out не определен.
    bool fetchPrices(const char* path, prices::PriceSeries& out);

private:
    bool readResponseBody(String& response, size_t maxBodySize);

    Print& log_;
    arduino::WiFiSSLClient client_;
};
