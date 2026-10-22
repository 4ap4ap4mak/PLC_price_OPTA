#include "EleringPriceSource.h"

#include <Elering.h>

static const char* serverHost = elering::kHost;
static const int   serverPort = elering::kPort;
static const char* apiPath    = elering::kCurrentPath;
// Предел тела ответа прогноза: сегодня–завтра по всем зонам — около 30 КБ
static const size_t kMaxForecastBody = 40000;

// Тело HTTP-ответа после заголовков: общий таймаут 20 с, без байтов 5 с,
// заголовки до 2 КБ, тело до maxBodySize.
arduino::Client* EleringPriceSource::connect(Uplink uplink, const char* failure) {
    arduino::Client* client = nullptr;
    switch (uplink) {
        case Uplink::WiFi: client = &wifiClient_; break;
        case Uplink::Ethernet: client = &ethernetClient_; break;
        case Uplink::None: break;
    }
    if (client == nullptr || !routeThrough(uplink)) {
        log_.println("[ERROR] Нет связи с интернетом: запрос не выполнен.");
        return nullptr;
    }
    if (!client->connect(serverHost, serverPort)) {
        log_.println(failure);
        client->stop();
        return nullptr;
    }
    return client;
}

void EleringPriceSource::sendGet(arduino::Client& client, const char* path) {
    client.print("GET ");
    client.print(path);
    client.println(" HTTP/1.0");
    client.print("Host: ");
    client.println(serverHost);
    client.println("User-Agent: ArduinoOpta-PLC/1.0");
    client.println("Accept: application/json");
    client.println("Connection: close");
    client.println();
}

bool EleringPriceSource::readResponseBody(arduino::Client& client, String& response,
    size_t maxBodySize) {
    const unsigned long totalTimeoutMs = 20000UL;
    const unsigned long idleTimeoutMs = 5000UL;
    const size_t maxHeaderSize = 2048;
    const char headerTerminator[] = "\r\n\r\n";
    size_t terminatorMatch = 0;
    size_t headerSize = 0;
    bool headersComplete = false;
    char statusLine[48] = {};
    size_t statusLength = 0;
    bool statusLineDone = false;
    unsigned long startedAt = millis();
    unsigned long lastByteAt = startedAt;

    response = "";
    if (!response.reserve(maxBodySize)) {
        log_.println("[ERROR] Недостаточно памяти для HTTP-ответа.");
        return false;
    }

    while (client.connected() || client.available()) {
        unsigned long now = millis();
        if (now - startedAt >= totalTimeoutMs ||
            now - lastByteAt >= idleTimeoutMs) {
            log_.println("[ERROR] Таймаут чтения HTTP-ответа.");
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
                // Первая строка заголовков — строка статуса
                if (!statusLineDone) {
                    if (byte == '\r' || byte == '\n') {
                        statusLineDone = true;
                    } else if (statusLength < sizeof(statusLine) - 1) {
                        statusLine[statusLength++] = byte;
                    }
                }
                if (++headerSize > maxHeaderSize) {
                    log_.println("[ERROR] HTTP-заголовки превышают допустимый размер.");
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
                    log_.println("[ERROR] Тело HTTP-ответа превышает допустимый размер.");
                    return false;
                }
                response += byte;
            }
        } else {
            delay(1);
        }
    }

    if (!headersComplete) {
        log_.println("[ERROR] HTTP-ответ не содержит полного заголовка.");
        return false;
    }
    // Ответ с ошибкой (429, 503…) не разбирается как цена
    const int status = elering::parseHttpStatus(statusLine);
    if (status != 200) {
        log_.print("[ERROR] Elering ответил HTTP ");
        log_.print(status);
        log_.print(": ");
        log_.println(statusLine);
        return false;
    }
    return true;
}

bool EleringPriceSource::fetchPrices(Uplink uplink, const char* path,
    prices::PriceSeries& out) {
    arduino::Client* client =
        connect(uplink, "[ERROR] Не удалось установить TLS-соединение для прогноза.");
    if (client == nullptr) return false;
    sendGet(*client, path);

    String response;
    const bool received = readResponseBody(*client, response, kMaxForecastBody);
    client->stop();
    if (!received) return false;
    response.trim();

    const char* jsonError = "";
    switch (elering::parseForecast(response.c_str(), response.length(), out, jsonError)) {
        case elering::Error::Ok:
            break;
        case elering::Error::Json:
            log_.print("[ERROR] Ошибка разбора прогноза Elering: ");
            log_.println(jsonError);
            return false;
        case elering::Error::Empty:
            log_.println("[ERROR] Elering вернул пустой прогноз цен LV.");
            return false;
        default:
            log_.println("[ERROR] В ответе Elering отсутствует массив цен LV.");
            return false;
    }
    // Размер ответа — чтобы видеть запас до предела (ответ содержит все зоны, не только LV)
    log_.print("[FORECAST] Получено квартальных значений: ");
    log_.print(out.size());
    log_.print(" (ответ ");
    log_.print(response.length());
    log_.print(" байт из ");
    log_.print(kMaxForecastBody);
    log_.println(").");
    return true;
}

bool EleringPriceSource::fetchCurrent(Uplink uplink, float& currentPriceEurKwh,
    long& priceTimestamp) {
    log_.print("[HTTP] Подключение к ");
    log_.print(serverHost);
    log_.print(" через ");
    log_.print(uplinkName(uplink));
    log_.println("...");

    arduino::Client* client =
        connect(uplink, "[ERROR] Не удалось установить защищенное соединение (SSL)!");
    if (client == nullptr) return false;

    log_.println("[HTTP] Отправка GET-запроса...");
    sendGet(*client, apiPath);

    String jsonBody;
    const bool received = readResponseBody(*client, jsonBody, 4096);
    client->stop();
    if (!received) return false;

    jsonBody.trim();

    // Извлечение чистого JSON (от первого '{' до последнего '}')
    size_t jsonStart = 0;
    size_t jsonEnd = 0;
    if (elering::jsonObjectBounds(jsonBody.c_str(), jsonBody.length(), jsonStart, jsonEnd)) {
        jsonBody = jsonBody.substring(jsonStart, jsonEnd);
    }

    log_.print("[HTTP] Ответ сервера: ");
    log_.println(jsonBody);

    elering::CurrentPrice price = {};
    const char* jsonError = "";
    // now == 0: актуальность проверит Controller по часам в момент результата
    switch (elering::parseCurrent(jsonBody.c_str(), jsonBody.length(), 0, price, jsonError)) {
        case elering::Error::Ok:
            break;
        case elering::Error::Json:
            log_.print("[ERROR] Ошибка парсинга JSON: ");
            log_.println(jsonError);
            return false;
        case elering::Error::NoPrice:
            log_.println("[ERROR] В ответе API нет числовой цены или timestamp!");
            return false;
        case elering::Error::OutOfRange:
            log_.print("[ERROR] Цена вне допустимого диапазона: ");
            log_.println(price.eurMwh);
            return false;
        case elering::Error::BadTimestamp:
            log_.print("[ERROR] Некорректный timestamp цены: ");
            log_.println(price.timestamp);
            return false;
        default:
            log_.println("[ERROR] Некорректный формат данных в ответе API!");
            return false;
    }

    currentPriceEurKwh = price.eurKwh;
    priceTimestamp = price.timestamp;
    return true;
}
