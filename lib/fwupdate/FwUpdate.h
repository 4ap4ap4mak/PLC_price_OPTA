// Обновление прошивки с GitHub Releases (D-038): версии, манифест релиза, пути
// скачивания, разбор URL перенаправления, проверка образа и расписание проверок.
// Чистый модуль L0: без Arduino и сети, не логирует. Зависит от ArduinoJson 7.
//
// Релиз — тег vX.Y.Z с двумя файлами: firmware.bin и manifest.json
// {"board":"opta","version":"X.Y.Z","size":N,"sha256":"<64 hex>"}. Устройство берет
// манифест последнего релиза, сравнивает версию со своей и по кнопке скачивает
// firmware.bin того же тега. Выпуск релиза — scripts/release.sh (docs/DEVELOPMENT.md).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fwupdate {

constexpr const char* kReleaseHost = "github.com";
constexpr int kPort = 443;
// Последний релиз (не черновик и не pre-release); GitHub перенаправляет на хранилище файлов
constexpr const char* kManifestPath =
    "/4ap4ap4mak/PLC_price_OPTA/releases/latest/download/manifest.json";
// Файл релиза конкретного тега: префикс + X.Y.Z + "/firmware.bin"
constexpr const char* kFirmwarePathPrefix = "/4ap4ap4mak/PLC_price_OPTA/releases/download/v";
constexpr const char* kBoard = "opta";

// Окно приложения во внутренней flash (cpp.linker_script.ld): загрузчик Arduino —
// первые 256 КБ, приложение ядра M7 — до конца первого банка.
constexpr uint32_t kAppStart = 0x08040000UL;
constexpr uint32_t kAppEnd = 0x08100000UL;
constexpr uint32_t kMaxImageSize = kAppEnd - kAppStart;
// Меньше — точно не прошивка этого проекта (сейчас ~520 КБ)
constexpr uint32_t kMinImageSize = 64UL * 1024UL;

constexpr size_t kVersionTextSize = 16;
constexpr size_t kMaxManifestBody = 1024;
constexpr size_t kSha256Size = 32;
// Путь firmware.bin: /<репозиторий>/releases/download/vX.Y.Z/firmware.bin
constexpr size_t kFirmwarePathSize = 96;

struct Version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
};

// "X.Y.Z" или "vX.Y.Z", части 0…9999, без суффиксов (-rc и т. п.).
bool parseVersion(const char* text, Version& out);
// <0 — a старше b, 0 — равны, >0 — a новее b.
int compareVersions(const Version& a, const Version& b);

struct Manifest {
    Version version;
    char versionText[kVersionTextSize];  // как в манифесте, без 'v'
    uint32_t size;                       // байт в firmware.bin
    uint8_t sha256[kSha256Size];
};

enum class ManifestError : uint8_t {
    Ok,
    Json,     // не JSON или не объект
    Board,    // нет board или не "opta"
    Version,  // нет version или формат не X.Y.Z
    Size,     // размер вне kMinImageSize…kMaxImageSize
    Sha256    // не 64 шестнадцатеричных символа
};

ManifestError parseManifest(const char* json, size_t length, Manifest& out);
// Короткое описание ошибки для лога и страницы.
const char* manifestErrorText(ManifestError error);

// Почему не удалась проверка или загрузка — для страницы и консоли.
enum class Failure : uint8_t {
    None,
    NoNetwork,     // нет связи с интернетом
    Connect,       // TLS-соединение не установлено
    Http,          // код ответа не 200 и не перенаправление, лишние перенаправления
    NotFound,      // 404: релизов еще нет
    BadResponse,   // заголовки или тело не по формату, таймаут
    Manifest,      // манифест не прошел parseManifest
    Size,          // длина ответа не совпала с манифестом
    Image,         // начало файла — не прошивка Opta
    Checksum,      // SHA-256 не совпал
    Storage,       // раздел OTA на QSPI недоступен или ошибка записи
    Bootloader     // загрузчик не умеет ставить прошивку из QSPI
};

const char* failureText(Failure failure);

// Путь firmware.bin релиза версии versionText (без 'v'). false — не поместился.
bool firmwarePath(const char* versionText, char* out, size_t outSize);

// "https://host[:443]/path?query" → host и path (с query). Только https и порт 443;
// false — другой адрес или не поместился в буферы.
bool splitHttpsUrl(const char* url, char* host, size_t hostSize, char* path, size_t pathSize);

// Строка заголовка "Name: value": если имя совпадает с name без учета регистра —
// value указывает на значение без пробелов по краям.
bool headerValue(const char* line, const char* name, const char*& value, size_t& length);

// Первые 8 байт образа — таблица векторов Cortex-M7: начальный стек в ОЗУ, вектор
// сброса — Thumb-адрес внутри окна приложения. Отсекает не тот файл (HTML ошибки,
// прошивку другой платы или ядра M4).
bool plausibleVectorTable(const uint8_t first8[8]);

// Версия загрузчика Arduino по образу его области flash (KI-030). image — size байт
// начиная с адреса base (у Opta — 0x08000000, 256 КБ). Новые загрузчики (MCUboot,
// 23+) хранят версию в блоке данных: kBootloaderDataOffset, признак 0xA0, затем версия.
// У загрузчика 22 этого блока нет (там стертая flash): версия — константа в коде,
// которая передается в snprintf строки DFU "@Bootloader version %d"; ищется инструкция
// Thumb `movs r3, #N` рядом с загрузкой адреса этой строки. 0 — не найдена.
constexpr uint32_t kBootloaderBase = 0x08000000UL;
constexpr uint32_t kBootloaderSize = kAppStart - kBootloaderBase;
constexpr uint32_t kBootloaderDataOffset = 0x1F000UL;
constexpr uint8_t kBootloaderDataMagic = 0xA0;
uint8_t bootloaderVersion(const uint8_t* image, size_t size, uint32_t base);

// Дайджест → 64 строчных hex-символа и '\0'.
void sha256Hex(const uint8_t digest[kSha256Size], char out[2 * kSha256Size + 1]);

// Автоматическая проверка новой версии (D-038): первая — через kFirstDelayMs после
// появления связи, затем раз в сутки; после неудачи — повтор через час. Время —
// millis(), сравнения переполнению не подвержены. Ручная проверка тоже отмечается,
// чтобы не идти в сеть второй раз подряд.
class CheckSchedule {
public:
    static constexpr uint32_t kFirstDelayMs = 2UL * 60UL * 1000UL;
    static constexpr uint32_t kIntervalMs = 24UL * 60UL * 60UL * 1000UL;
    static constexpr uint32_t kRetryMs = 60UL * 60UL * 1000UL;

    // Пора проверить. online — есть связь с интернетом.
    bool due(uint32_t nowMs, bool online);
    // Проверка завершилась (ok — манифест получен).
    void markChecked(uint32_t nowMs, bool ok);

private:
    bool onlineSeen_ = false;
    uint32_t onlineSinceMs_ = 0;
    bool checked_ = false;
    bool lastOk_ = false;
    uint32_t lastCheckMs_ = 0;
};

}  // namespace fwupdate
