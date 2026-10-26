#include "FirmwareDownloader.h"

#include <string.h>

#include <Elering.h>
#include <mbedtls/sha256.h>

using fwupdate::Failure;

// Заголовки: всего до 20 с и до 16 КБ (у github.com длинная Content-Security-Policy),
// без байтов — 15 с; тело — без байтов 20 с.
static const uint32_t kHeaderTimeoutMs = 20000UL;
static const size_t kMaxHeaderBytes = 16384;
static const uint32_t kBodyIdleTimeoutMs = 20000UL;
// Сокетов в ядре 4 на все устройство, 2 постоянно у HTTP-сервера и mDNS: пока браузер
// держит одно-два соединения (переход после POST, запасное соединение), TLS не
// открывается. Браузерные соединения живут секунды — повтор с паузой (KI-028).
static const uint8_t kConnectAttempts = 3;
static const uint32_t kConnectRetryPauseMs = 4000UL;

static bool isRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool FirmwareDownloader::readLine(arduino::Client& client, uint32_t startedAtMs,
    bool& truncated) {
    size_t length = 0;
    truncated = false;
    while (millis() - startedAtMs < kHeaderTimeoutMs) {
        if (client.available() > 0) {
            const int value = client.read();
            if (value < 0) {
                delay(1);
                continue;
            }
            if (value == '\n') {
                if (length > 0 && line_[length - 1] == '\r') --length;
                line_[length] = '\0';
                return true;
            }
            if (length < kLineSize - 1) {
                line_[length++] = static_cast<char>(value);
            } else {
                truncated = true;
            }
        } else if (!client.connected()) {
            return false;
        } else {
            delay(1);
        }
    }
    return false;
}

Failure FirmwareDownloader::readHeaders(arduino::Client& client, int& status,
    bool& hasLocation) {
    const uint32_t startedAt = millis();
    size_t headerBytes = 0;
    bool truncated = false;
    status = 0;
    hasLocation = false;
    contentLength_ = -1;
    if (!readLine(client, startedAt, truncated)) {
        log_.println("[FIRMWARE] Нет ответа сервера.");
        return Failure::BadResponse;
    }
    status = elering::parseHttpStatus(line_);
    if (status == 0) {
        log_.println("[FIRMWARE] Ответ сервера не HTTP.");
        return Failure::BadResponse;
    }
    char statusLine[48];
    strncpy(statusLine, line_, sizeof(statusLine) - 1);
    statusLine[sizeof(statusLine) - 1] = '\0';

    for (;;) {
        if (!readLine(client, startedAt, truncated)) {
            log_.println("[FIRMWARE] Заголовки ответа не получены полностью.");
            return Failure::BadResponse;
        }
        if (line_[0] == '\0') break;
        headerBytes += strlen(line_) + 2;
        if (headerBytes > kMaxHeaderBytes) {
            log_.println("[FIRMWARE] Заголовки ответа превышают допустимый размер.");
            return Failure::BadResponse;
        }
        const char* value = nullptr;
        size_t valueLength = 0;
        if (fwupdate::headerValue(line_, "Location", value, valueLength)) {
            // Значение — внутри line_; host_/path_ меняются только при успешном разборе
            const_cast<char*>(value)[valueLength] = '\0';
            bool ok = !truncated;
            if (ok && value[0] == '/') {
                ok = valueLength < kPathSize;
                if (ok) memcpy(path_, value, valueLength + 1);
            } else if (ok) {
                ok = fwupdate::splitHttpsUrl(value, host_, kHostSize, path_, kPathSize);
            }
            if (!ok) {
                log_.println("[FIRMWARE] Адрес перенаправления не поддерживается.");
                return Failure::BadResponse;
            }
            hasLocation = true;
        } else if (fwupdate::headerValue(line_, "Content-Length", value, valueLength)) {
            contentLength_ = atol(value);
        } else if (fwupdate::headerValue(line_, "Transfer-Encoding", value, valueLength)) {
            // На запрос HTTP/1.0 сервер не должен отвечать по частям
            log_.println("[FIRMWARE] Ответ по частям (chunked) не поддерживается.");
            return Failure::BadResponse;
        }
    }
    if (status != 200 && !isRedirect(status)) {
        log_.print("[FIRMWARE] Сервер ответил: ");
        log_.println(statusLine);
    }
    return Failure::None;
}

bool FirmwareDownloader::connectWithRetry(arduino::Client& client) {
    for (uint8_t attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        if (client.connect(host_, fwupdate::kPort)) return true;
        client.stop();
        log_.print("[FIRMWARE] Не удалось установить TLS-соединение с ");
        log_.print(host_);
        log_.print(" (попытка ");
        log_.print(attempt);
        log_.print(" из ");
        log_.print(kConnectAttempts);
        log_.println(").");
        if (attempt < kConnectAttempts) delay(kConnectRetryPauseMs);
    }
    return false;
}

Failure FirmwareDownloader::openGet(Uplink uplink, arduino::Client*& client) {
    client = nullptr;
    for (uint8_t redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        arduino::Client* candidate = nullptr;
        switch (uplink) {
            case Uplink::WiFi: candidate = &wifiClient_; break;
            case Uplink::Ethernet: candidate = &ethernetClient_; break;
            case Uplink::None: break;
        }
        if (candidate == nullptr || !routeThrough(uplink)) {
            log_.println("[FIRMWARE] Нет связи с интернетом.");
            return Failure::NoNetwork;
        }
        log_.print("[FIRMWARE] Запрос к ");
        log_.print(host_);
        log_.print(" через ");
        log_.print(uplinkName(uplink));
        log_.println("...");
        if (!connectWithRetry(*candidate)) return Failure::Connect;
        candidate->print("GET ");
        candidate->print(path_);
        candidate->println(" HTTP/1.0");
        candidate->print("Host: ");
        candidate->println(host_);
        candidate->println("User-Agent: ArduinoOpta-PLC/1.0");
        candidate->println("Accept: */*");
        candidate->println("Connection: close");
        candidate->println();

        int status = 0;
        bool hasLocation = false;
        const Failure failure = readHeaders(*candidate, status, hasLocation);
        if (failure != Failure::None) {
            candidate->stop();
            return failure;
        }
        if (status == 200) {
            client = candidate;
            return Failure::None;
        }
        candidate->stop();
        if (isRedirect(status) && hasLocation) continue;
        return status == 404 ? Failure::NotFound : Failure::Http;
    }
    log_.println("[FIRMWARE] Слишком много перенаправлений.");
    return Failure::Http;
}

int FirmwareDownloader::readBody(arduino::Client& client, uint8_t* buffer, size_t length,
    uint32_t& lastByteMs) {
    for (;;) {
        const int available = client.available();
        if (available > 0) {
            const size_t wanted = static_cast<size_t>(available) < length
                ? static_cast<size_t>(available) : length;
            const int received = client.read(buffer, wanted);
            if (received > 0) {
                lastByteMs = millis();
                return received;
            }
        } else if (!client.connected()) {
            return -1;
        }
        if (millis() - lastByteMs >= kBodyIdleTimeoutMs) return -1;
        delay(1);
    }
}

Failure FirmwareDownloader::fetchManifest(Uplink uplink, fwupdate::Manifest& out) {
    strcpy(host_, fwupdate::kReleaseHost);
    strcpy(path_, fwupdate::kManifestPath);
    arduino::Client* client = nullptr;
    const Failure failure = openGet(uplink, client);
    if (failure == Failure::NotFound) {
        log_.println("[FIRMWARE] Релизов на GitHub пока нет.");
    }
    if (failure != Failure::None) return failure;

    char body[fwupdate::kMaxManifestBody + 1];
    size_t length = 0;
    uint32_t lastByteMs = millis();
    for (;;) {
        if (length >= fwupdate::kMaxManifestBody) {
            client->stop();
            log_.println("[FIRMWARE] Манифест превышает допустимый размер.");
            return Failure::Manifest;
        }
        const int received = readBody(*client,
            reinterpret_cast<uint8_t*>(body) + length, fwupdate::kMaxManifestBody - length,
            lastByteMs);
        if (received < 0) break;
        length += static_cast<size_t>(received);
    }
    client->stop();
    body[length] = '\0';

    const fwupdate::ManifestError error = fwupdate::parseManifest(body, length, out);
    if (error != fwupdate::ManifestError::Ok) {
        log_.print("[FIRMWARE] Манифест релиза отклонен: ");
        log_.println(fwupdate::manifestErrorText(error));
        return Failure::Manifest;
    }
    log_.print("[FIRMWARE] Последний релиз: ");
    log_.print(out.versionText);
    log_.print(", ");
    log_.print(out.size);
    log_.println(" байт.");
    return Failure::None;
}

Failure FirmwareDownloader::download(Uplink uplink, const fwupdate::Manifest& manifest,
    OtaStorage& storage, mbed::Callback<void(uint32_t)> progress) {
    if (!OtaStorage::bootloaderSupportsOta()) {
        log_.print("[FIRMWARE] Загрузчик версии ");
        log_.print(OtaStorage::bootloaderVersion());
        log_.println(" не поддерживает обновление из QSPI.");
        return Failure::Bootloader;
    }
    strcpy(host_, fwupdate::kReleaseHost);
    if (!fwupdate::firmwarePath(manifest.versionText, path_, kPathSize)) {
        return Failure::Manifest;
    }
    // Раздел готовится до запроса: форматирование занимает секунды, и сервер закрыл бы
    // соединение без данных
    if (!storage.begin(log_)) return Failure::Storage;
    arduino::Client* client = nullptr;
    Failure failure = openGet(uplink, client);
    if (failure != Failure::None) {
        storage.abort();
        return failure;
    }
    if (contentLength_ >= 0 && static_cast<uint32_t>(contentLength_) != manifest.size) {
        client->stop();
        storage.abort();
        log_.print("[FIRMWARE] Длина файла ");
        log_.print(contentLength_);
        log_.print(" байт, в манифесте ");
        log_.print(manifest.size);
        log_.println(".");
        return Failure::Size;
    }
    log_.print("[FIRMWARE] Загрузка ");
    log_.print(manifest.versionText);
    log_.println(" на QSPI...");

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    uint8_t head[8] = {};
    uint32_t received = 0;
    uint8_t loggedQuarter = 0;
    const uint32_t startedAt = millis();
    uint32_t lastByteMs = startedAt;
    while (received < manifest.size) {
        if (millis() - startedAt >= kDownloadTimeoutMs) {
            log_.println("[FIRMWARE] Загрузка не уложилась в 10 минут.");
            failure = Failure::BadResponse;
            break;
        }
        const uint32_t left = manifest.size - received;
        const int count = readBody(*client, chunk_, left < kChunkSize ? left : kChunkSize,
            lastByteMs);
        if (count < 0) {
            log_.print("[FIRMWARE] Соединение прервано на ");
            log_.print(received);
            log_.println(" байт.");
            failure = Failure::BadResponse;
            break;
        }
        for (int i = 0; i < count && received + static_cast<uint32_t>(i) < sizeof(head); ++i) {
            head[received + i] = chunk_[i];
        }
        const uint32_t before = received;
        received += static_cast<uint32_t>(count);
        if (before < sizeof(head) && received >= sizeof(head) &&
            !fwupdate::plausibleVectorTable(head)) {
            log_.println("[FIRMWARE] Начало файла не похоже на прошивку Opta.");
            failure = Failure::Image;
            break;
        }
        mbedtls_sha256_update_ret(&sha, chunk_, static_cast<size_t>(count));
        if (!storage.write(chunk_, static_cast<size_t>(count))) {
            log_.print("[FIRMWARE] Ошибка записи на QSPI (");
            log_.print(storage.lastError());
            log_.println(").");
            failure = Failure::Storage;
            break;
        }
        progress(received);
        const uint8_t quarter = static_cast<uint8_t>(4ULL * received / manifest.size);
        if (quarter > loggedQuarter && quarter < 4) {
            loggedQuarter = quarter;
            log_.print("[FIRMWARE] Загружено ");
            log_.print(quarter * 25);
            log_.println("%.");
        }
    }
    client->stop();

    uint8_t digest[fwupdate::kSha256Size];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (failure == Failure::None && memcmp(digest, manifest.sha256, sizeof(digest)) != 0) {
        char hex[2 * fwupdate::kSha256Size + 1];
        fwupdate::sha256Hex(digest, hex);
        log_.print("[FIRMWARE] SHA-256 не совпал: получено ");
        log_.println(hex);
        failure = Failure::Checksum;
    }
    if (failure != Failure::None) {
        storage.abort();
        return failure;
    }
    if (!storage.finish()) {
        log_.print("[FIRMWARE] Не удалось закрыть UPDATE.BIN на QSPI (");
        log_.print(storage.lastError());
        log_.println(").");
        return Failure::Storage;
    }
    // Файл читается обратно так же, как его прочитает загрузчик, — через FAT
    failure = verifyStored(storage, manifest);
    if (failure != Failure::None) return failure;
    log_.print("[FIRMWARE] Образ ");
    log_.print(manifest.versionText);
    log_.print(" записан и проверен (");
    log_.print(received);
    log_.print(" байт за ");
    log_.print((millis() - startedAt) / 1000UL);
    log_.println(" с).");
    return Failure::None;
}

Failure FirmwareDownloader::verifyStored(OtaStorage& storage,
    const fwupdate::Manifest& manifest) {
    if (!storage.beginRead(log_)) return Failure::Storage;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    uint32_t total = 0;
    int count = 0;
    while ((count = storage.read(chunk_, kChunkSize)) > 0) {
        mbedtls_sha256_update_ret(&sha, chunk_, static_cast<size_t>(count));
        total += static_cast<uint32_t>(count);
    }
    uint8_t digest[fwupdate::kSha256Size];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);
    storage.abort();
    if (count < 0 || total != manifest.size ||
        memcmp(digest, manifest.sha256, sizeof(digest)) != 0) {
        log_.print("[FIRMWARE] UPDATE.BIN на QSPI не совпал с образом: прочитано ");
        log_.print(total);
        log_.print(" байт");
        log_.println(count < 0 ? ", ошибка чтения." : ".");
        return Failure::Storage;
    }
    return Failure::None;
}
