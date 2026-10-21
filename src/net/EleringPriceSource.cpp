#include "EleringPriceSource.h"

#include <Arduino.h>

#include <Elering.h>

#include "../platform/Watchdog.h"

static const char* serverHost = elering::kHost;
static const int   serverPort = elering::kPort;
static const char* apiPath    = elering::kCurrentPath;

// Тело HTTP-ответа после заголовков: общий таймаут 20 с, без байтов 5 с,
// заголовки до 2 КБ, тело до maxBodySize.
static bool readHttpResponseBody(arduino::Client& client, String& response, size_t maxBodySize) {
    const unsigned long totalTimeoutMs = 20000UL;
    const unsigned long idleTimeoutMs = 5000UL;
    const size_t maxHeaderSize = 2048;
    const char headerTerminator[] = "\r\n\r\n";
    size_t terminatorMatch = 0;
    size_t headerSize = 0;
    bool headersComplete = false;
    unsigned long startedAt = millis();
    unsigned long lastByteAt = startedAt;

    response = "";
    if (!response.reserve(maxBodySize)) {
        Serial.println("[ERROR] Недостаточно памяти для HTTP-ответа.");
        return false;
    }

    while (client.connected() || client.available()) {
        kickWatchdog();
        unsigned long now = millis();
        if (now - startedAt >= totalTimeoutMs ||
            now - lastByteAt >= idleTimeoutMs) {
            Serial.println("[ERROR] Таймаут чтения HTTP-ответа.");
            return false;
        }
        if (client.available() > 0) {
            int value = client.read();
            if (value < 0) {
                delay(1);
                continue;
            }
            lastByteAt = millis();
            char byte = static_cast<char>(value);
            if (!headersComplete) {
                if (++headerSize > maxHeaderSize) {
                    Serial.println("[ERROR] HTTP-заголовки превышают допустимый размер.");
                    return false;
                }
                if (byte == headerTerminator[terminatorMatch]) {
                    ++terminatorMatch;
                    if (terminatorMatch == sizeof(headerTerminator) - 1) {
                        headersComplete = true;
                    }
                } else {
                    terminatorMatch = byte == headerTerminator[0] ? 1 : 0;
                }
            } else {
                if (response.length() >= maxBodySize) {
                    Serial.println("[ERROR] Тело HTTP-ответа превышает допустимый размер.");
                    return false;
                }
                response += byte;
            }
        } else {
            delay(1);
        }
    }

    if (!headersComplete) {
        Serial.println("[ERROR] HTTP-ответ не содержит полного заголовка.");
        return false;
    }
    return true;
}

bool EleringPriceSource::fetchPrices(const char* path, prices::PriceSeries& out) {
    if (!client_.connect(serverHost, serverPort)) {
        Serial.println("[ERROR] Не удалось установить TLS-соединение для прогноза.");
        client_.stop();
        return false;
    }
    client_.print("GET ");
    client_.print(path);
    client_.println(" HTTP/1.0");
    client_.print("Host: ");
    client_.println(serverHost);
    client_.println("User-Agent: ArduinoOpta-PLC/1.0");
    client_.println("Accept: application/json");
    client_.println("Connection: close");
    client_.println();

    String response;
    if (!readHttpResponseBody(client_, response, 40000)) {
        client_.stop();
        return false;
    }
    client_.stop();
    response.trim();

    const char* jsonError = "";
    switch (elering::parseForecast(response.c_str(), response.length(), out, jsonError)) {
        case elering::Error::Ok:
            break;
        case elering::Error::Json:
            Serial.print("[ERROR] Ошибка разбора прогноза Elering: ");
            Serial.println(jsonError);
            return false;
        case elering::Error::Empty:
            Serial.println("[ERROR] Elering вернул пустой прогноз цен LV.");
            return false;
        default:
            Serial.println("[ERROR] В ответе Elering отсутствует массив цен LV.");
            return false;
    }
    Serial.print("[FORECAST] Получено квартальных значений: ");
    Serial.println(out.size());
    return true;
}

bool EleringPriceSource::fetchCurrent(float& currentPriceEurKwh, long& priceTimestamp) {
    Serial.print("[HTTP] Подключение к ");
    Serial.print(serverHost);
    Serial.println("...");

    if (!client_.connect(serverHost, serverPort)) {
        Serial.println("[ERROR] Не удалось установить защищенное соединение (SSL)!");
        client_.stop();
        return false;
    }

    Serial.println("[HTTP] Отправка GET-запроса...");
    client_.print("GET ");
    client_.print(apiPath);
    client_.println(" HTTP/1.0");
    client_.print("Host: ");
    client_.println(serverHost);
    client_.println("User-Agent: ArduinoOpta-PLC/1.0");
    client_.println("Accept: application/json");
    client_.println("Connection: close");
    client_.println();

    String jsonBody;
    if (!readHttpResponseBody(client_, jsonBody, 4096)) {
        client_.stop();
        return false;
    }
    client_.stop();

    jsonBody.trim();

    // Извлечение чистого JSON (от первого '{' до последнего '}')
    size_t jsonStart = 0;
    size_t jsonEnd = 0;
    if (elering::jsonObjectBounds(jsonBody.c_str(), jsonBody.length(), jsonStart, jsonEnd)) {
        jsonBody = jsonBody.substring(jsonStart, jsonEnd);
    }

    Serial.print("[HTTP] Ответ сервера: ");
    Serial.println(jsonBody);

    elering::CurrentPrice price = {};
    const char* jsonError = "";
    uint32_t nowEpoch = clock_.synced() ? clock_.nowUtc() : 0;
    switch (elering::parseCurrent(jsonBody.c_str(), jsonBody.length(), nowEpoch, price, jsonError)) {
        case elering::Error::Ok:
            break;
        case elering::Error::Json:
            Serial.print("[ERROR] Ошибка парсинга JSON: ");
            Serial.println(jsonError);
            return false;
        case elering::Error::NoPrice:
            Serial.println("[ERROR] В ответе API нет числовой цены или timestamp!");
            return false;
        case elering::Error::OutOfRange:
            Serial.print("[ERROR] Цена вне допустимого диапазона: ");
            Serial.println(price.eurMwh);
            return false;
        case elering::Error::BadTimestamp:
            Serial.print("[ERROR] Некорректный timestamp цены: ");
            Serial.println(price.timestamp);
            return false;
        case elering::Error::Stale:
            Serial.print("[ERROR] Цена не относится к текущему времени (timestamp ");
            Serial.print(price.timestamp);
            Serial.print(", сейчас ");
            Serial.print(nowEpoch);
            Serial.println(").");
            return false;
        default:
            Serial.println("[ERROR] Некорректный формат данных в ответе API!");
            return false;
    }

    currentPriceEurKwh = price.eurKwh;
    priceTimestamp = price.timestamp;

    Serial.print("[NORDPOOL-LV] Цена из API: ");
    Serial.print(currentPriceEurKwh, 5);
    Serial.print(" EUR/kWh, timestamp: ");
    Serial.println(priceTimestamp);

    return true;
}
