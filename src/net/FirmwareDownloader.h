// Манифест и файл прошивки с GitHub Releases по TLS через Wi-Fi или Ethernet (D-038).
// GitHub отдает файлы релиза перенаправлением (302) на хранилище
// release-assets.githubusercontent.com с длинной подписанной ссылкой: загрузчик
// следует до kMaxRedirects перенаправлениям, только по https и порту 443.
// Сертификат сервера проверяет TLS-клиент ядра по /wlan/cacert.pem.
//
// Вызывается только из рабочего потока FetchWorker, как EleringPriceSource: блокирует
// поток на время запроса (файл — до kDownloadTimeoutMs), loop() при этом работает.
// Watchdog не сбрасывается: зависание ловит FetchWorker::stuck() по отсутствию
// прогресса (D-026). Лог — в буфер строк FetchWorker.
#pragma once

#include <Arduino.h>
#include <EthernetSSLClient.h>
#include <WiFiSSLClient.h>
#include <mbed.h>

#include <FwUpdate.h>

#include "../platform/OtaStorage.h"
#include "Uplink.h"

class FirmwareDownloader {
public:
    static constexpr uint32_t kDownloadTimeoutMs = 10UL * 60UL * 1000UL;

    explicit FirmwareDownloader(Print& log) : log_(log) {}

    // Манифест последнего релиза.
    fwupdate::Failure fetchManifest(Uplink uplink, fwupdate::Manifest& out);
    // firmware.bin релиза manifest → UPDATE.BIN на QSPI с проверкой длины, таблицы
    // векторов и SHA-256; затем файл читается с QSPI и сверяется еще раз.
    // progress(байт записано) — после каждого блока. При ошибке файл удален.
    fwupdate::Failure download(Uplink uplink, const fwupdate::Manifest& manifest,
        OtaStorage& storage, mbed::Callback<void(uint32_t)> progress);

private:
    static constexpr uint8_t kMaxRedirects = 5;
    static constexpr size_t kHostSize = 96;
    // Подписанная ссылка хранилища GitHub — около 800 символов
    static constexpr size_t kPathSize = 1536;
    static constexpr size_t kLineSize = 2048;
    static constexpr size_t kChunkSize = 1024;

    // GET https://host_ path_ с перенаправлениями. Done — client на начале тела,
    // contentLength_ — из заголовка или -1.
    fwupdate::Failure openGet(Uplink uplink, arduino::Client*& client);
    // TLS-соединение с host_ до kConnectAttempts раз с паузой (KI-028).
    bool connectWithRetry(arduino::Client& client);
    // Заголовки ответа: код, Location (→ host_/path_), Content-Length.
    fwupdate::Failure readHeaders(arduino::Client& client, int& status, bool& hasLocation);
    // Строка заголовка в line_ до '\n'. false — таймаут или соединение закрыто.
    bool readLine(arduino::Client& client, uint32_t startedAtMs, bool& truncated);
    // Чтение до length байт тела с таймаутом простоя; <0 — таймаут или конец потока.
    int readBody(arduino::Client& client, uint8_t* buffer, size_t length, uint32_t& lastByteMs);
    // Записанный UPDATE.BIN читается через FAT и сверяется с длиной и SHA-256 манифеста.
    fwupdate::Failure verifyStored(OtaStorage& storage, const fwupdate::Manifest& manifest);

    Print& log_;
    arduino::WiFiSSLClient wifiClient_;
    arduino::EthernetSSLClient ethernetClient_;
    char host_[kHostSize] = {};
    char path_[kPathSize] = {};
    char line_[kLineSize] = {};
    long contentLength_ = -1;
    uint8_t chunk_[kChunkSize] = {};
};
