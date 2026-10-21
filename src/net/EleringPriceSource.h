// Цены Elering по TLS (Wi-Fi): текущая цена и цены за период для Controller.
// Разбор JSON — lib/elering, здесь — соединение, HTTP-запрос и лог [HTTP]/[ERROR].
// Синхронный: каждый запрос блокирует loop() до 20 с, watchdog сбрасывается
// внутри чтения (KI-006; асинхронный FetchWorker — шаг 2g).
#pragma once

#include <WiFiSSLClient.h>

#include <ControllerPorts.h>

class EleringPriceSource : public controller::PriceSource {
public:
    // clock — для проверки актуальности текущей цены (до NTP не проверяется).
    explicit EleringPriceSource(const controller::Clock& clock) : clock_(clock) {}

    bool fetchCurrent(float& eurKwh, long& timestamp) override;
    // Цены за период path → out. Прогноз контроллера не трогается.
    bool fetchPrices(const char* path, prices::PriceSeries& out) override;

private:
    const controller::Clock& clock_;
    arduino::WiFiSSLClient client_;
};
