// Тесты lib/fwupdate: версии, манифест релиза, пути и URL GitHub, проверка образа,
// расписание автоматической проверки (D-038).
#include <string.h>
#include <unity.h>

#include <FwUpdate.h>

#include "../helpers/TestHelpers.h"

using namespace fwupdate;

void setUp() {}
void tearDown() {}

static const char kSha[] = "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF";

static ManifestError manifest(const char* json, Manifest& out) {
    return parseManifest(json, strlen(json), out);
}

static void test_parse_version() {
    Version v = {};
    TEST_ASSERT_TRUE(parseVersion("1.2.3", v));
    TEST_ASSERT_EQUAL_UINT16(1, v.major);
    TEST_ASSERT_EQUAL_UINT16(2, v.minor);
    TEST_ASSERT_EQUAL_UINT16(3, v.patch);
    TEST_ASSERT_TRUE(parseVersion("v10.0.9999", v));
    TEST_ASSERT_EQUAL_UINT16(10, v.major);
    TEST_ASSERT_EQUAL_UINT16(9999, v.patch);

    TEST_ASSERT_FALSE(parseVersion("", v));
    TEST_ASSERT_FALSE(parseVersion("1.2", v));
    TEST_ASSERT_FALSE(parseVersion("1.2.3.4", v));
    TEST_ASSERT_FALSE(parseVersion("1.2.3-rc1", v));
    TEST_ASSERT_FALSE(parseVersion("1..3", v));
    TEST_ASSERT_FALSE(parseVersion("1.2.10000", v));
    TEST_ASSERT_FALSE(parseVersion("V1.2.3", v));
    TEST_ASSERT_FALSE(parseVersion(" 1.2.3", v));
    TEST_ASSERT_FALSE(parseVersion(nullptr, v));
}

static void test_compare_versions() {
    Version a = {}, b = {};
    parseVersion("1.2.3", a);
    parseVersion("1.2.3", b);
    TEST_ASSERT_EQUAL_INT(0, compareVersions(a, b));
    parseVersion("1.2.10", b);
    TEST_ASSERT_TRUE(compareVersions(a, b) < 0);
    TEST_ASSERT_TRUE(compareVersions(b, a) > 0);
    parseVersion("1.10.0", b);
    TEST_ASSERT_TRUE(compareVersions(a, b) < 0);
    parseVersion("2.0.0", b);
    TEST_ASSERT_TRUE(compareVersions(a, b) < 0);
    parseVersion("0.9.9", b);
    TEST_ASSERT_TRUE(compareVersions(a, b) > 0);
}

static void test_manifest_ok() {
    Manifest m = {};
    char json[256];
    snprintf(json, sizeof(json),
        "{\"board\":\"opta\",\"version\":\"1.0.1\",\"size\":534636,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Ok, manifest(json, m));
    TEST_ASSERT_EQUAL_STRING("1.0.1", m.versionText);
    TEST_ASSERT_EQUAL_UINT16(1, m.version.patch);
    TEST_ASSERT_EQUAL_UINT32(534636UL, m.size);
    TEST_ASSERT_EQUAL_HEX8(0x00, m.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0x11, m.sha256[1]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, m.sha256[30]);  // верхний регистр тоже принимается
    TEST_ASSERT_EQUAL_HEX8(0xFF, m.sha256[31]);

    char hex[65];
    sha256Hex(m.sha256, hex);
    TEST_ASSERT_EQUAL_STRING(
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", hex);
}

static void test_manifest_rejects() {
    Manifest m = {};
    char json[256];
    ASSERT_ENUM(ManifestError::Json, manifest("<html>Not Found</html>", m));
    ASSERT_ENUM(ManifestError::Json, manifest("[1,2]", m));

    snprintf(json, sizeof(json),
        "{\"board\":\"portenta\",\"version\":\"1.0.1\",\"size\":534636,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Board, manifest(json, m));
    snprintf(json, sizeof(json),
        "{\"version\":\"1.0.1\",\"size\":534636,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Board, manifest(json, m));

    snprintf(json, sizeof(json),
        "{\"board\":\"opta\",\"version\":\"v1.0.1\",\"size\":534636,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Version, manifest(json, m));
    snprintf(json, sizeof(json),
        "{\"board\":\"opta\",\"version\":\"1.0\",\"size\":534636,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Version, manifest(json, m));
    snprintf(json, sizeof(json),
        "{\"board\":\"opta\",\"version\":101,\"size\":534636,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Version, manifest(json, m));

    snprintf(json, sizeof(json),
        "{\"board\":\"opta\",\"version\":\"1.0.1\",\"size\":1000,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Size, manifest(json, m));
    snprintf(json, sizeof(json),
        "{\"board\":\"opta\",\"version\":\"1.0.1\",\"size\":786433,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Size, manifest(json, m));
    snprintf(json, sizeof(json),
        "{\"board\":\"opta\",\"version\":\"1.0.1\",\"size\":\"534636\",\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Size, manifest(json, m));
    snprintf(json, sizeof(json),
        "{\"board\":\"opta\",\"version\":\"1.0.1\",\"size\":786432,\"sha256\":\"%s\"}", kSha);
    ASSERT_ENUM(ManifestError::Ok, manifest(json, m));

    ASSERT_ENUM(ManifestError::Sha256, manifest(
        "{\"board\":\"opta\",\"version\":\"1.0.1\",\"size\":534636,\"sha256\":\"0011\"}", m));
    ASSERT_ENUM(ManifestError::Sha256, manifest(
        "{\"board\":\"opta\",\"version\":\"1.0.1\",\"size\":534636,\"sha256\":"
        "\"zz112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}", m));
}

static void test_firmware_path() {
    char path[kFirmwarePathSize];
    TEST_ASSERT_TRUE(firmwarePath("1.0.1", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/4ap4ap4mak/PLC_price_OPTA/releases/download/v1.0.1/firmware.bin",
        path);
    char small[20];
    TEST_ASSERT_FALSE(firmwarePath("1.0.1", small, sizeof(small)));
}

static void test_split_https_url() {
    char host[64];
    char path[128];
    TEST_ASSERT_TRUE(splitHttpsUrl(
        "https://release-assets.githubusercontent.com/github-production/1/2?sp=r&sig=a%2Fb",
        host, sizeof(host), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("release-assets.githubusercontent.com", host);
    TEST_ASSERT_EQUAL_STRING("/github-production/1/2?sp=r&sig=a%2Fb", path);

    TEST_ASSERT_TRUE(splitHttpsUrl("https://github.com", host, sizeof(host), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("github.com", host);
    TEST_ASSERT_EQUAL_STRING("/", path);
    TEST_ASSERT_TRUE(splitHttpsUrl("https://github.com:443/a", host, sizeof(host), path,
        sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/a", path);
    TEST_ASSERT_TRUE(splitHttpsUrl("https://github.com?x=1", host, sizeof(host), path,
        sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/?x=1", path);

    TEST_ASSERT_FALSE(splitHttpsUrl("http://github.com/a", host, sizeof(host), path, sizeof(path)));
    TEST_ASSERT_FALSE(splitHttpsUrl("/relative/path", host, sizeof(host), path, sizeof(path)));
    TEST_ASSERT_FALSE(splitHttpsUrl("https:///a", host, sizeof(host), path, sizeof(path)));
    TEST_ASSERT_FALSE(splitHttpsUrl("https://github.com:8443/a", host, sizeof(host), path,
        sizeof(path)));
    TEST_ASSERT_FALSE(splitHttpsUrl("https://github.com:4430/a", host, sizeof(host), path,
        sizeof(path)));
    char tinyHost[6];
    TEST_ASSERT_FALSE(splitHttpsUrl("https://github.com/a", tinyHost, sizeof(tinyHost), path,
        sizeof(path)));
    char tinyPath[3];
    TEST_ASSERT_FALSE(splitHttpsUrl("https://github.com/abc", host, sizeof(host), tinyPath,
        sizeof(tinyPath)));
}

static void test_header_value() {
    const char* value = nullptr;
    size_t length = 0;
    TEST_ASSERT_TRUE(headerValue("Location:  https://x/y \r", "location", value, length));
    TEST_ASSERT_EQUAL_size_t(11, length);
    TEST_ASSERT_EQUAL_INT(0, strncmp(value, "https://x/y", length));
    TEST_ASSERT_TRUE(headerValue("content-length: 534636", "Content-Length", value, length));
    TEST_ASSERT_EQUAL_INT(0, strncmp(value, "534636", length));
    TEST_ASSERT_TRUE(headerValue("X-Empty:", "x-empty", value, length));
    TEST_ASSERT_EQUAL_size_t(0, length);

    TEST_ASSERT_FALSE(headerValue("Content-Type: text/html", "Content-Length", value, length));
    TEST_ASSERT_FALSE(headerValue("Location https://x", "Location", value, length));
    TEST_ASSERT_FALSE(headerValue("Loc", "Location", value, length));
}

static void setVectors(uint8_t out[8], uint32_t stack, uint32_t reset) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(stack >> (8 * i));
        out[4 + i] = static_cast<uint8_t>(reset >> (8 * i));
    }
}

static void test_vector_table() {
    uint8_t v[8];
    // Как в текущей сборке: стек — конец AXI SRAM, сброс — Thumb-адрес в приложении
    setVectors(v, 0x24080000UL, 0x08098915UL);
    TEST_ASSERT_TRUE(plausibleVectorTable(v));
    setVectors(v, 0x20020000UL, 0x08040201UL);
    TEST_ASSERT_TRUE(plausibleVectorTable(v));

    setVectors(v, 0x24080000UL, 0x08098914UL);  // не Thumb
    TEST_ASSERT_FALSE(plausibleVectorTable(v));
    setVectors(v, 0x24080000UL, 0x08020001UL);  // внутри загрузчика
    TEST_ASSERT_FALSE(plausibleVectorTable(v));
    setVectors(v, 0x24080000UL, 0x08100001UL);  // второй банк (ядро M4)
    TEST_ASSERT_FALSE(plausibleVectorTable(v));
    setVectors(v, 0x10000000UL, 0x08098915UL);  // стек вне ОЗУ
    TEST_ASSERT_FALSE(plausibleVectorTable(v));
    setVectors(v, 0x24080004UL, 0x08098915UL);
    TEST_ASSERT_FALSE(plausibleVectorTable(v));
    memcpy(v, "<!DOCTYP", 8);  // страница ошибки вместо прошивки
    TEST_ASSERT_FALSE(plausibleVectorTable(v));
}

static void test_check_schedule_first_and_daily() {
    CheckSchedule schedule;
    const uint32_t t0 = 1000;
    TEST_ASSERT_FALSE(schedule.due(t0, false));
    TEST_ASSERT_FALSE(schedule.due(t0 + CheckSchedule::kFirstDelayMs, false));
    // Отсчет — с появления связи
    TEST_ASSERT_FALSE(schedule.due(t0 + 10000, true));
    TEST_ASSERT_FALSE(schedule.due(t0 + 10000 + CheckSchedule::kFirstDelayMs - 1, true));
    TEST_ASSERT_TRUE(schedule.due(t0 + 10000 + CheckSchedule::kFirstDelayMs, true));

    const uint32_t checkedAt = t0 + 200000;
    schedule.markChecked(checkedAt, true);
    TEST_ASSERT_FALSE(schedule.due(checkedAt + CheckSchedule::kRetryMs, true));
    TEST_ASSERT_FALSE(schedule.due(checkedAt + CheckSchedule::kIntervalMs - 1, true));
    TEST_ASSERT_FALSE(schedule.due(checkedAt + CheckSchedule::kIntervalMs, false));
    TEST_ASSERT_TRUE(schedule.due(checkedAt + CheckSchedule::kIntervalMs, true));
}

static void test_check_schedule_retry_and_wrap() {
    CheckSchedule schedule;
    // Связь появилась перед переполнением millis()
    const uint32_t t0 = 0xFFFFFFFFUL - 30000UL;
    TEST_ASSERT_FALSE(schedule.due(t0, true));
    TEST_ASSERT_TRUE(schedule.due(t0 + CheckSchedule::kFirstDelayMs, true));
    const uint32_t failedAt = t0 + CheckSchedule::kFirstDelayMs;
    schedule.markChecked(failedAt, false);
    TEST_ASSERT_FALSE(schedule.due(failedAt + CheckSchedule::kRetryMs - 1, true));
    TEST_ASSERT_TRUE(schedule.due(failedAt + CheckSchedule::kRetryMs, true));
}

// KI-030: загрузчик 22 на реальной Opta — блока данных нет, версия в коде (раскладка
// снята с устройства через DFU 2026-10-26: строка 0x0801ABF6, код 0x08002332)
static uint8_t bootloaderImage[0x20000];

void test_bootloader_version_v22_from_code() {
    memset(bootloaderImage, 0xFF, sizeof(bootloaderImage));
    const char text[] = "@Bootloader version %d /0x00000000/0*4Kg";
    memcpy(bootloaderImage + 0x1ABF6, text, sizeof(text));
    // movs r3, #22; ldr r2, [pc, #20]; movs r1, #48
    const uint8_t code[] = {0x16, 0x23, 0x05, 0x4A, 0x30, 0x21};
    memcpy(bootloaderImage + 0x2332, code, sizeof(code));
    const uint8_t literal[] = {0xF6, 0xAB, 0x01, 0x08};
    memcpy(bootloaderImage + 0x234C, literal, sizeof(literal));
    TEST_ASSERT_EQUAL_UINT8(22, fwupdate::bootloaderVersion(bootloaderImage,
        sizeof(bootloaderImage), fwupdate::kBootloaderBase));
}

void test_bootloader_version_from_data_block() {
    memset(bootloaderImage, 0xFF, sizeof(bootloaderImage));
    bootloaderImage[fwupdate::kBootloaderDataOffset] = 0xA0;  // как у загрузчика 25
    bootloaderImage[fwupdate::kBootloaderDataOffset + 1] = 25;
    TEST_ASSERT_EQUAL_UINT8(25, fwupdate::bootloaderVersion(bootloaderImage,
        sizeof(bootloaderImage), fwupdate::kBootloaderBase));
}

void test_bootloader_version_unknown() {
    memset(bootloaderImage, 0xFF, sizeof(bootloaderImage));
    TEST_ASSERT_EQUAL_UINT8(0, fwupdate::bootloaderVersion(bootloaderImage,
        sizeof(bootloaderImage), fwupdate::kBootloaderBase));
    // Строка есть, но ссылки на нее нет
    const char text[] = "@Bootloader version %d";
    memcpy(bootloaderImage + 0x100, text, sizeof(text));
    TEST_ASSERT_EQUAL_UINT8(0, fwupdate::bootloaderVersion(bootloaderImage,
        sizeof(bootloaderImage), fwupdate::kBootloaderBase));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_version);
    RUN_TEST(test_compare_versions);
    RUN_TEST(test_manifest_ok);
    RUN_TEST(test_manifest_rejects);
    RUN_TEST(test_firmware_path);
    RUN_TEST(test_split_https_url);
    RUN_TEST(test_header_value);
    RUN_TEST(test_vector_table);
    RUN_TEST(test_check_schedule_first_and_daily);
    RUN_TEST(test_check_schedule_retry_and_wrap);
    RUN_TEST(test_bootloader_version_v22_from_code);
    RUN_TEST(test_bootloader_version_from_data_block);
    RUN_TEST(test_bootloader_version_unknown);
    return UNITY_END();
}
