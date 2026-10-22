// Цены Elering по TLS через Wi-Fi или Ethernet (D-036): синхронные запросы текущей цены
// и цен за период.
// Разбор JSON — lib/elering, здесь — соединение, HTTP-запрос и лог [HTTP]/[ERROR].
//
// С шага 2g вызывается только из рабочего потока FetchWorker: каждый запрос блокирует
// поток до ~20 с, loop() при этом работает. Поэтому watchdog здесь не сбрасывается —
// его сбрасывает только loop(), а зависший запрос ловит FetchWorker::stuck() (D-026).
// Лог пишется в log (буфер строк FetchWorker), а не в Serial напрямую.
#pragma once

#include <Arduino.h>
#include <EthernetSSLClient.h>
#include <WiFiSSLClient.h>

#include <PriceSeries.h>

#include "Uplink.h"

class EleringPriceSource {
public:
    explicit EleringPriceSource(Print& log) : log_(log) {}

    // Текущая цена через uplink. Актуальность не проверяется (часы — у основного
    // потока): это делает Controller по часам в момент результата.
    bool fetchCurrent(Uplink uplink, float& eurKwh, long& timestamp);
    // Цены за период path → out через uplink. При неудаче out не определен.
    bool fetchPrices(Uplink uplink, const char* path, prices::PriceSeries& out);

private:
    // Маршрут через uplink и TLS-соединение с Elering; nullptr — не удалось (в логе).
    arduino::Client* connect(Uplink uplink, const char* failure);
    void sendGet(arduino::Client& client, const char* path);
    bool readResponseBody(arduino::Client& client, String& response, size_t maxBodySize);

    Print& log_;
    arduino::WiFiSSLClient wifiClient_;
    arduino::EthernetSSLClient ethernetClient_;
};
