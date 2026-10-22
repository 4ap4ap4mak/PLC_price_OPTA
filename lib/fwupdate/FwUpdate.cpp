#include "FwUpdate.h"

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

namespace fwupdate {

static constexpr uint16_t kMaxVersionPart = 9999;
static constexpr const char* kFirmwareFileName = "/firmware.bin";

// Число 0…kMaxVersionPart из text; end — первый символ после цифр.
static bool parseVersionPart(const char* text, uint16_t& out, const char*& end) {
    uint32_t value = 0;
    const char* p = text;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + static_cast<uint32_t>(*p - '0');
        if (value > kMaxVersionPart) return false;
        ++p;
    }
    if (p == text) return false;
    out = static_cast<uint16_t>(value);
    end = p;
    return true;
}

bool parseVersion(const char* text, Version& out) {
    if (text == nullptr) return false;
    if (*text == 'v') ++text;
    Version version = {};
    const char* p = text;
    if (!parseVersionPart(p, version.major, p) || *p++ != '.') return false;
    if (!parseVersionPart(p, version.minor, p) || *p++ != '.') return false;
    if (!parseVersionPart(p, version.patch, p) || *p != '\0') return false;
    out = version;
    return true;
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ManifestError parseManifest(const char* json, size_t length, Manifest& out) {
    JsonDocument doc;
    if (deserializeJson(doc, json, length) != DeserializationError::Ok ||
        !doc.is<JsonObject>()) {
        return ManifestError::Json;
    }
    const char* board = doc["board"].as<const char*>();
    if (board == nullptr || strcmp(board, kBoard) != 0) return ManifestError::Board;

    Manifest manifest = {};
    const char* version = doc["version"].as<const char*>();
    if (version == nullptr || *version == 'v' || strlen(version) >= kVersionTextSize ||
        !parseVersion(version, manifest.version)) {
        return ManifestError::Version;
    }
    strcpy(manifest.versionText, version);

    if (!doc["size"].is<uint32_t>()) return ManifestError::Size;
    manifest.size = doc["size"].as<uint32_t>();
    if (manifest.size < kMinImageSize || manifest.size > kMaxImageSize) {
        return ManifestError::Size;
    }

    const char* sha = doc["sha256"].as<const char*>();
    if (sha == nullptr || strlen(sha) != 2 * kSha256Size) return ManifestError::Sha256;
    for (size_t i = 0; i < kSha256Size; ++i) {
        const int high = hexDigit(sha[2 * i]);
        const int low = hexDigit(sha[2 * i + 1]);
        if (high < 0 || low < 0) return ManifestError::Sha256;
        manifest.sha256[i] = static_cast<uint8_t>(high * 16 + low);
    }
    out = manifest;
    return ManifestError::Ok;
}

const char* manifestErrorText(ManifestError error) {
    switch (error) {
        case ManifestError::Ok: return "ok";
        case ManifestError::Json: return "манифест не разобран";
        case ManifestError::Board: return "релиз не для Opta";
        case ManifestError::Version: return "неверный формат версии";
        case ManifestError::Size: return "недопустимый размер прошивки";
        case ManifestError::Sha256: return "неверная контрольная сумма в манифесте";
    }
    return "?";
}

const char* failureText(Failure failure) {
    switch (failure) {
        case Failure::None: return "нет ошибки";
        case Failure::NoNetwork: return "нет связи с интернетом";
        case Failure::Connect: return "не удалось соединиться с GitHub";
        case Failure::Http: return "GitHub ответил ошибкой";
        case Failure::NotFound: return "релиз не найден";
        case Failure::BadResponse: return "ответ GitHub не получен полностью";
        case Failure::Manifest: return "манифест релиза некорректен";
        case Failure::Size: return "размер файла не совпал с манифестом";
        case Failure::Image: return "файл не является прошивкой Opta";
        case Failure::Checksum: return "контрольная сумма не совпала";
        case Failure::Storage: return "не удалось записать образ на QSPI-flash";
        case Failure::Bootloader: return "загрузчик не поддерживает обновление";
    }
    return "?";
}

bool firmwarePath(const char* versionText, char* out, size_t outSize) {
    const int written = snprintf(out, outSize, "%s%s%s", kFirmwarePathPrefix, versionText,
        kFirmwareFileName);
    return written > 0 && static_cast<size_t>(written) < outSize;
}

bool splitHttpsUrl(const char* url, char* host, size_t hostSize, char* path, size_t pathSize) {
    static const char kScheme[] = "https://";
    if (strncmp(url, kScheme, sizeof(kScheme) - 1) != 0) return false;
    const char* hostStart = url + sizeof(kScheme) - 1;
    const char* hostEnd = hostStart;
    while (*hostEnd != '\0' && *hostEnd != '/' && *hostEnd != ':' && *hostEnd != '?') ++hostEnd;
    const size_t hostLength = static_cast<size_t>(hostEnd - hostStart);
    if (hostLength == 0 || hostLength >= hostSize) return false;

    const char* rest = hostEnd;
    if (*rest == ':') {
        if (strncmp(rest, ":443", 4) != 0) return false;
        rest += 4;
        if (*rest != '\0' && *rest != '/' && *rest != '?') return false;
    }
    // Путь пуст — запрашивается корень; query без пути — "/?..."
    const bool needsSlash = *rest != '/';
    const size_t restLength = strlen(rest);
    if (restLength + (needsSlash ? 1 : 0) >= pathSize) return false;

    memcpy(host, hostStart, hostLength);
    host[hostLength] = '\0';
    size_t pathLength = 0;
    if (needsSlash) path[pathLength++] = '/';
    memcpy(path + pathLength, rest, restLength + 1);
    return true;
}

static char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool headerValue(const char* line, const char* name, const char*& value, size_t& length) {
    const char* p = line;
    for (const char* n = name; *n != '\0'; ++n, ++p) {
        if (*p == '\0' || lowerAscii(*p) != lowerAscii(*n)) return false;
    }
    if (*p != ':') return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    const char* end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    value = p;
    length = static_cast<size_t>(end - p);
    return true;
}

static uint32_t readLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
        static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

bool plausibleVectorTable(const uint8_t first8[8]) {
    // Стек растет вниз: начальное значение — конец области, граница включительно.
    // DTCM 128 КБ, AXI SRAM 512 КБ, SRAM D2 288 КБ (cpp.linker_script.ld).
    const uint32_t stack = readLe32(first8);
    const bool stackInRam =
        (stack > 0x20000000UL && stack <= 0x20020000UL) ||
        (stack > 0x24000000UL && stack <= 0x24080000UL) ||
        (stack > 0x30000000UL && stack <= 0x30048000UL);
    const uint32_t reset = readLe32(first8 + 4);
    const bool resetInApp = (reset & 1U) != 0 && reset > kAppStart && reset < kAppEnd;
    return stackInRam && resetInApp;
}

void sha256Hex(const uint8_t digest[kSha256Size], char out[2 * kSha256Size + 1]) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < kSha256Size; ++i) {
        out[2 * i] = kHex[digest[i] >> 4];
        out[2 * i + 1] = kHex[digest[i] & 0x0F];
    }
    out[2 * kSha256Size] = '\0';
}

bool CheckSchedule::due(uint32_t nowMs, bool online) {
    if (!online) return false;
    if (!onlineSeen_) {
        onlineSeen_ = true;
        onlineSinceMs_ = nowMs;
    }
    if (!checked_) return nowMs - onlineSinceMs_ >= kFirstDelayMs;
    return nowMs - lastCheckMs_ >= (lastOk_ ? kIntervalMs : kRetryMs);
}

void CheckSchedule::markChecked(uint32_t nowMs, bool ok) {
    checked_ = true;
    lastOk_ = ok;
    lastCheckMs_ = nowMs;
}

}  // namespace fwupdate
