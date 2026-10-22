#include "WebServer.h"

#include <string.h>

#include <HttpParse.h>

#include "../platform/Watchdog.h"
#include "HistoryPage.h"
#include "SettingsPage.h"

using configstore::WifiCredentials;

static const unsigned long HTTP_REQUEST_TIMEOUT_MS = 3000UL;
static const size_t HTTP_MAX_LINE_LENGTH = 512;
static const uint8_t HTTP_MAX_HEADER_COUNT = 32;

// Читает одну строку HTTP-запроса с общим таймаутом и ограничением длины,
// чтобы медленный или некорректный клиент не блокировал loop() и память.
static bool readHttpLine(arduino::Client& client, String& line, unsigned long requestStartedAt) {
    line = "";
    while (millis() - requestStartedAt < HTTP_REQUEST_TIMEOUT_MS) {
        kickWatchdog();
        if (client.available() > 0) {
            int value = client.read();
            if (value < 0) {
                delay(1);
                continue;
            }
            if (value == '\n') {
                line.trim();
                return true;
            }
            if (line.length() >= HTTP_MAX_LINE_LENGTH) {
                return false;
            }
            line += static_cast<char>(value);
        } else if (!client.connected()) {
            return false;
        } else {
            delay(1);
        }
    }
    return false;
}

// Тело POST длиной contentLength (1…kMaxFormBody). false — ответ об ошибке уже отправлен.
static bool readHttpBody(arduino::Client& client, int contentLength, String& body) {
    if (contentLength <= 0 || contentLength > static_cast<int>(httpparse::kMaxFormBody)) {
        client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid form"));
        return false;
    }
    unsigned long bodyStartedAt = millis();
    while (body.length() < static_cast<unsigned int>(contentLength) &&
           millis() - bodyStartedAt < 5000UL) {
        kickWatchdog();
        while (client.available() && body.length() < static_cast<unsigned int>(contentLength)) {
            body += static_cast<char>(client.read());
        }
        delay(1);
    }
    if (body.length() != static_cast<unsigned int>(contentLength)) {
        client.println(F("HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\nIncomplete form"));
        return false;
    }
    return true;
}

// Возврат на страницу того канала, с которым работала форма (KI-010), на ту же
// вкладку графика (D-030).
static void sendRedirectHome(arduino::Client& client, uint8_t channelIndex, int8_t chartDay) {
    char location[httpparse::kHomeLocationSize];
    httpparse::formatHomeLocation(channelIndex, chartDay, location, sizeof(location));
    client.println(F("HTTP/1.1 303 See Other"));
    client.print(F("Location: "));
    client.println(location);
    client.println(F("Cache-Control: no-store"));
    client.println(F("Connection: close"));
    client.println();
}

void WebServer::handleClient(arduino::Client& client) {
    unsigned long requestStartedAt = millis();
    String requestLine;
    if (!readHttpLine(client, requestLine, requestStartedAt)) {
        Serial.println("[WEB] Запрос отклонен: таймаут или слишком длинная строка.");
        client.stop();
        return;
    }
    httpparse::RequestLine parsedLine;
    httpparse::parseRequestLine(requestLine.c_str(), parsedLine);
    String method = requestLine.substring(0, parsedLine.methodLength);
    String path = parsedLine.hasPath
        ? requestLine.substring(parsedLine.pathStart, parsedLine.pathStart + parsedLine.pathLength)
        : String("/");
    Serial.print("[WEB] Запрос: ");
    Serial.print(method);
    Serial.println(path);

    int contentLength = 0;
    bool headersComplete = false;
    for (uint8_t headerCount = 0; headerCount <= HTTP_MAX_HEADER_COUNT; ++headerCount) {
        String header;
        if (!readHttpLine(client, header, requestStartedAt)) {
            break;
        }
        if (header.length() == 0) {
            headersComplete = true;
            break;
        }
        long value = 0;
        if (httpparse::parseContentLength(header.c_str(), value)) {
            contentLength = static_cast<int>(value);
        }
    }
    if (!headersComplete) {
        Serial.println("[WEB] Запрос отклонен: заголовки не получены полностью.");
        client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n"));
        client.stop();
        return;
    }

    if (method == "GET" && path.startsWith("/history")) {
        // Реле, с которого пришли: туда ведет ссылка «Настройки» (D-030)
        const httpparse::PageSelection selection = httpparse::pageSelection(path.c_str());
        sendHistoryPage(client, switchHistory_, selection.channelIndex);
        client.stop();
        return;
    }

    if (method == "POST" && path == "/force") {
        handleForce(client, contentLength);
        return;
    }

    if (method == "POST" && path == "/save") {
        handleSave(client, contentLength);
        return;
    }

    // Реле и сутки — только из этого запроса, у каждой вкладки свои (KI-010)
    const httpparse::PageSelection selection = httpparse::pageSelection(path.c_str());
    sendSettingsPage(client, channels_, clock_, selection.channelIndex, selection.chartDay,
        network_.wifiFieldsVisible(), String());
    client.stop();
}

void WebServer::handleForce(arduino::Client& client, int contentLength) {
    String body;
    if (!readHttpBody(client, contentLength, body)) {
        client.stop();
        return;
    }
    httpparse::ForceForm form;
    if (!httpparse::parseForceForm(body.c_str(), body.length(), form)) {
        client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid force-on request"));
        client.stop();
        return;
    }
    if (form.start) {
        channels_.startForceOn(form.channelIndex);
    } else {
        channels_.stopForceOn(form.channelIndex);
    }
    sendRedirectHome(client, form.channelIndex, form.chartDay);
    client.stop();
}

void WebServer::handleSave(arduino::Client& client, int contentLength) {
    String body;
    if (!readHttpBody(client, contentLength, body)) {
        client.stop();
        return;
    }

    httpparse::SettingsForm form;
    httpparse::FormError formError = httpparse::parseSettingsForm(
        body.c_str(), body.length(), channels_.configs(),
        clock_.synced() ? clock_.nowUtc() : 0,
        network_.wifiFieldsVisible(), form);
    if (formError != httpparse::FormError::Ok) {
        client.print(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"));
        client.println(httpparse::formErrorText(formError));
        client.stop();
        return;
    }

    if (!channels_.saveConfig(form.channelIndex, form.config)) {
        client.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nUnable to save device settings"));
        client.stop();
        Serial.println("[ERROR] Не удалось сохранить настройки устройства в KVStore.");
        return;
    }

    bool credentialsChanged = form.hasWifi;
    bool restartRequested = form.restartRequested;
    if (credentialsChanged) {
        WifiCredentials credentials = {};
        memcpy(credentials.ssid, form.ssid, sizeof(credentials.ssid));
        memcpy(credentials.password, form.password, sizeof(credentials.password));
        if (!wifiCredentials_.save(credentials)) {
            client.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nUnable to save Wi-Fi credentials"));
            client.stop();
            Serial.println("[ERROR] Не удалось сохранить Wi-Fi данные в KVStore.");
            return;
        }
    }
    // Решение — по последней цене и загруженному прогнозу, без сети: и по Ethernet без
    // Wi-Fi (D-030, п. 2)
    if (!credentialsChanged) {
        channels_.applyConfiguredControl(form.channelIndex);
    }
    if (credentialsChanged || restartRequested) {
        sendRedirectHome(client, form.channelIndex, form.chartDay);
        client.flush();
        client.stop();
        delay(500);
        NVIC_SystemReset();
        return;
    }
    sendRedirectHome(client, form.channelIndex, form.chartDay);
    client.stop();
}
