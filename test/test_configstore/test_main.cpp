// Тесты lib/configstore: загрузка настроек каналов из KVStore со всеми исходами
// (как loadDeviceConfigs до шага 2d), запись блоба, учетные данные Wi-Fi.
// KVStore подменен фейком в памяти с управляемыми ошибками.
#include <unity.h>

#include <string.h>

#include <ChannelConfig.h>
#include <ConfigStore.h>
#include <WifiCredentialStore.h>

#include "../helpers/FakeKv.h"
#include "../helpers/TestHelpers.h"

using channelconfig::ChannelConfig;
using channelconfig::kBlobSize;
using channelconfig::kChannelCount;
using channelconfig::LegacyChannelConfig;
using configstore::ConfigStore;
using configstore::LoadOutcome;
using configstore::LoadReport;
using configstore::WifiCredentials;
using configstore::WifiCredentialStore;
using testing::FakeKv;

void setUp() {}
void tearDown() {}

static void fillDefaults(ChannelConfig configs[kChannelCount]) {
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        channelconfig::setDefaults(configs[i]);
    }
}

// Конфигурации с отличиями от значений по умолчанию, чтобы видеть, что именно загружено.
static void fillCustom(ChannelConfig configs[kChannelCount]) {
    fillDefaults(configs);
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        configs[i].alwaysOnBelowPriceEurKwh = 0.01f * static_cast<float>(i + 1);
        configs[i].forceOnHours = static_cast<uint8_t>(i + 2);
    }
}

static void assertAllDefaults(const ChannelConfig configs[kChannelCount]) {
    ChannelConfig expected[kChannelCount];
    fillDefaults(expected);
    TEST_ASSERT_EQUAL_MEMORY(expected, configs, kBlobSize);
}

static void assertStoredDefaults(const FakeKv& kv) {
    TEST_ASSERT_EQUAL_STRING(configstore::kDeviceConfigKey, kv.key);
    TEST_ASSERT_EQUAL_UINT32(kBlobSize, kv.size);
    assertAllDefaults(reinterpret_cast<const ChannelConfig*>(kv.data));
}

static void test_missing_key_creates_defaults() {
    FakeKv kv;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    fillCustom(out);
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::Created, report.outcome);
    TEST_ASSERT_EQUAL_INT(FakeKv::kNotFound, report.kvResult);
    TEST_ASSERT_TRUE(configstore::loadSucceeded(report.outcome));
    assertAllDefaults(out);
    assertStoredDefaults(kv);
}

static void test_missing_key_save_failed() {
    FakeKv kv;
    kv.failSet = true;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::CreateFailed, report.outcome);
    TEST_ASSERT_FALSE(configstore::loadSucceeded(report.outcome));
}

static void test_current_blob_loaded_without_write() {
    FakeKv kv;
    ChannelConfig stored[kChannelCount];
    fillCustom(stored);
    kv.put(configstore::kDeviceConfigKey, stored, kBlobSize);
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::Loaded, report.outcome);
    TEST_ASSERT_EQUAL_UINT8(0, report.invalidMask);
    TEST_ASSERT_EQUAL_MEMORY(stored, out, kBlobSize);
    TEST_ASSERT_EQUAL_INT(0, kv.setCalls);
}

static void test_invalid_channel_reset_and_rewritten() {
    FakeKv kv;
    ChannelConfig stored[kChannelCount];
    fillCustom(stored);
    stored[2].nightCheapHours = 200;  // вне допустимого диапазона
    kv.put(configstore::kDeviceConfigKey, stored, kBlobSize);
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::Migrated, report.outcome);
    TEST_ASSERT_EQUAL_UINT8(1U << 2, report.invalidMask);
    ChannelConfig defaults;
    channelconfig::setDefaults(defaults);
    TEST_ASSERT_EQUAL_MEMORY(&defaults, &out[2], sizeof(ChannelConfig));
    TEST_ASSERT_EQUAL_MEMORY(&stored[1], &out[1], sizeof(ChannelConfig));
    TEST_ASSERT_EQUAL_INT(1, kv.setCalls);
    TEST_ASSERT_EQUAL_MEMORY(out, kv.data, kBlobSize);
}

static void test_migrated_save_failed_keeps_configs() {
    FakeKv kv;
    ChannelConfig stored[kChannelCount];
    fillCustom(stored);
    stored[0].nightCheapHours = 200;
    kv.put(configstore::kDeviceConfigKey, stored, kBlobSize);
    kv.failSet = true;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::MigratedSaveFailed, report.outcome);
    TEST_ASSERT_EQUAL_UINT8(1U, report.invalidMask);
    // D-037: перенесенные настройки остаются в RAM, хоть их и не удалось перезаписать
    TEST_ASSERT_TRUE(configstore::loadSucceeded(report.outcome));
    TEST_ASSERT_EQUAL_MEMORY(&stored[1], &out[1], sizeof(ChannelConfig));
    TEST_ASSERT_FALSE(store.writeLocked());
}

static void test_legacy_single_v3_migrated_to_current_size() {
    FakeKv kv;
    LegacyChannelConfig legacy = {};
    legacy.version = 3;
    legacy.safetyCeilingEurKwh = 0.5f;
    legacy.alwaysOnBelowPriceEurKwh = 0.03f;
    legacy.nightStartMinuteOfDay = 23 * 60;
    legacy.nightEndMinuteOfDay = 7 * 60;
    legacy.nightCheapHours = 4;
    legacy.consecutiveBlockHours = 3;
    legacy.holidayOnHours = 2;
    kv.put(configstore::kDeviceConfigKey, &legacy, sizeof(legacy));
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::Migrated, report.outcome);
    TEST_ASSERT_EQUAL_UINT32(kBlobSize, kv.size);
    TEST_ASSERT_EQUAL_UINT16(channelconfig::kConfigVersion, out[0].version);
    TEST_ASSERT_EQUAL_FLOAT(0.03f, out[0].alwaysOnBelowPriceEurKwh);
    TEST_ASSERT_EQUAL_MEMORY(out, kv.data, kBlobSize);
}

static void test_legacy_single_unknown_version_is_corrupt() {
    FakeKv kv;
    LegacyChannelConfig legacy = {};
    legacy.version = 9;
    kv.put(configstore::kDeviceConfigKey, &legacy, sizeof(legacy));
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    fillCustom(out);
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::Corrupt, report.outcome);
    TEST_ASSERT_TRUE(configstore::loadSucceeded(report.outcome));
    assertAllDefaults(out);
    assertStoredDefaults(kv);
}

static void test_corrupt_save_failed() {
    FakeKv kv;
    LegacyChannelConfig legacy = {};
    legacy.version = 9;
    kv.put(configstore::kDeviceConfigKey, &legacy, sizeof(legacy));
    kv.failSet = true;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    ASSERT_ENUM(LoadOutcome::CorruptSaveFailed, store.load(out).outcome);
}

// D-037: нечитаемый блоб любого формата не перезаписывается — настройки могут
// прочитаться после перезапуска; в RAM значения по умолчанию, запись заблокирована.
static void test_legacy_single_read_failure_not_overwritten() {
    FakeKv kv;
    LegacyChannelConfig legacy = {};
    legacy.version = 3;
    kv.put(configstore::kDeviceConfigKey, &legacy, sizeof(legacy));
    kv.failGet = true;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    ASSERT_ENUM(LoadOutcome::ReadFailed, store.load(out).outcome);
    assertAllDefaults(out);
    TEST_ASSERT_EQUAL_INT(0, kv.setCalls);
    TEST_ASSERT_TRUE(store.writeLocked());
}

static void test_legacy_multi_read_failure_not_overwritten() {
    FakeKv kv;
    uint8_t blob[channelconfig::kLegacyMultiBlobSize] = {};
    kv.put(configstore::kDeviceConfigKey, blob, sizeof(blob));
    kv.failGet = true;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::ReadFailed, report.outcome);
    TEST_ASSERT_EQUAL_INT(FakeKv::kReadError, report.kvResult);
    TEST_ASSERT_FALSE(configstore::loadSucceeded(report.outcome));
    TEST_ASSERT_EQUAL_INT(0, kv.setCalls);
}

static void test_current_read_failure_not_overwritten() {
    FakeKv kv;
    ChannelConfig stored[kChannelCount];
    fillCustom(stored);
    kv.put(configstore::kDeviceConfigKey, stored, kBlobSize);
    kv.failGet = true;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    ASSERT_ENUM(LoadOutcome::ReadFailed, store.load(out).outcome);
    assertAllDefaults(out);
    TEST_ASSERT_EQUAL_INT(0, kv.setCalls);

    // Запись заблокирована: сохранение настроек не затирает нечитаемый блоб
    ChannelConfig changed[kChannelCount];
    fillDefaults(changed);
    TEST_ASSERT_FALSE(store.save(changed));
    TEST_ASSERT_EQUAL_INT(0, kv.setCalls);
    TEST_ASSERT_EQUAL_MEMORY(stored, kv.data, kBlobSize);

    // Явная команда пользователя снимает блокировку
    TEST_ASSERT_TRUE(store.overwrite(changed));
    TEST_ASSERT_FALSE(store.writeLocked());
    TEST_ASSERT_EQUAL_INT(1, kv.setCalls);
    assertStoredDefaults(kv);
}

// Ошибка getInfo, отличная от «ключа нет», — тоже не повод записывать значения по умолчанию.
static void test_info_read_failure_not_overwritten() {
    FakeKv kv;
    ChannelConfig stored[kChannelCount];
    fillCustom(stored);
    kv.put(configstore::kDeviceConfigKey, stored, kBlobSize);
    kv.failInfoTimes = ConfigStore::kReadAttempts;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::ReadFailed, report.outcome);
    TEST_ASSERT_EQUAL_INT(FakeKv::kReadError, report.kvResult);
    TEST_ASSERT_FALSE(configstore::loadSucceeded(report.outcome));
    assertAllDefaults(out);
    TEST_ASSERT_EQUAL_INT(0, kv.setCalls);
    TEST_ASSERT_TRUE(store.writeLocked());
}

// Разовая ошибка чтения: повтор читает сохраненные настройки.
static void test_transient_read_errors_retried() {
    FakeKv kv;
    ChannelConfig stored[kChannelCount];
    fillCustom(stored);
    kv.put(configstore::kDeviceConfigKey, stored, kBlobSize);
    kv.failInfoTimes = ConfigStore::kReadAttempts - 1;
    kv.failGetTimes = ConfigStore::kReadAttempts - 1;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    ASSERT_ENUM(LoadOutcome::Loaded, store.load(out).outcome);
    TEST_ASSERT_EQUAL_MEMORY(stored, out, kBlobSize);
    TEST_ASSERT_FALSE(store.writeLocked());
}

static void test_short_read_treated_as_read_failure() {
    FakeKv kv;
    ChannelConfig stored[kChannelCount];
    fillCustom(stored);
    kv.put(configstore::kDeviceConfigKey, stored, kBlobSize);
    kv.shortGet = true;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    ASSERT_ENUM(LoadOutcome::ReadFailed, store.load(out).outcome);
    TEST_ASSERT_EQUAL_INT(0, kv.setCalls);
}

static void test_unknown_size_replaced_with_defaults() {
    FakeKv kv;
    uint8_t blob[50] = {};
    kv.put(configstore::kDeviceConfigKey, blob, sizeof(blob));
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    fillCustom(out);
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::UnknownSize, report.outcome);
    TEST_ASSERT_TRUE(configstore::loadSucceeded(report.outcome));
    assertAllDefaults(out);
    assertStoredDefaults(kv);
}

static void test_unknown_size_save_failed() {
    FakeKv kv;
    uint8_t blob[50] = {};
    kv.put(configstore::kDeviceConfigKey, blob, sizeof(blob));
    kv.failSet = true;
    ConfigStore store(kv);
    ChannelConfig out[kChannelCount];
    LoadReport report = store.load(out);
    ASSERT_ENUM(LoadOutcome::UnknownSizeSaveFailed, report.outcome);
    TEST_ASSERT_FALSE(configstore::loadSucceeded(report.outcome));
}

static void test_save_then_load_roundtrip() {
    FakeKv kv;
    ConfigStore store(kv);
    ChannelConfig saved[kChannelCount];
    fillCustom(saved);
    TEST_ASSERT_TRUE(store.save(saved));
    TEST_ASSERT_EQUAL_STRING("/kv/device_config", kv.key);
    TEST_ASSERT_EQUAL_UINT32(kBlobSize, kv.size);
    ChannelConfig out[kChannelCount];
    ASSERT_ENUM(LoadOutcome::Loaded, store.load(out).outcome);
    TEST_ASSERT_EQUAL_MEMORY(saved, out, kBlobSize);
}

static void test_save_failure_reported() {
    FakeKv kv;
    kv.failSet = true;
    ConfigStore store(kv);
    ChannelConfig configs[kChannelCount];
    fillDefaults(configs);
    TEST_ASSERT_FALSE(store.save(configs));
}

static WifiCredentials makeCredentials(const char* ssid, const char* password) {
    WifiCredentials credentials = {};
    strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    return credentials;
}

static void test_wifi_roundtrip() {
    FakeKv kv;
    WifiCredentialStore store(kv);
    WifiCredentials saved = makeCredentials("HomeNet", "secret123");
    TEST_ASSERT_TRUE(store.save(saved));
    TEST_ASSERT_EQUAL_STRING("/kv/wifi", kv.key);
    TEST_ASSERT_EQUAL_UINT32(sizeof(WifiCredentials), kv.size);
    WifiCredentials loaded = {};
    TEST_ASSERT_TRUE(store.load(loaded));
    TEST_ASSERT_EQUAL_STRING("HomeNet", loaded.ssid);
    TEST_ASSERT_EQUAL_STRING("secret123", loaded.password);
}

static void test_wifi_missing_or_unreadable() {
    FakeKv kv;
    WifiCredentialStore store(kv);
    WifiCredentials loaded = {};
    TEST_ASSERT_FALSE(store.load(loaded));
    WifiCredentials saved = makeCredentials("HomeNet", "secret123");
    kv.put(configstore::kWifiCredentialsKey, &saved, sizeof(saved));
    kv.failGet = true;
    TEST_ASSERT_FALSE(store.load(loaded));
}

static void test_wifi_rejects_wrong_size() {
    FakeKv kv;
    WifiCredentialStore store(kv);
    WifiCredentials saved = makeCredentials("HomeNet", "secret123");
    kv.put(configstore::kWifiCredentialsKey, &saved, sizeof(saved) - 1);
    WifiCredentials loaded = {};
    TEST_ASSERT_FALSE(store.load(loaded));
}

static void test_wifi_rejects_empty_ssid() {
    FakeKv kv;
    WifiCredentialStore store(kv);
    WifiCredentials saved = makeCredentials("", "secret123");
    kv.put(configstore::kWifiCredentialsKey, &saved, sizeof(saved));
    WifiCredentials loaded = {};
    TEST_ASSERT_FALSE(store.load(loaded));
}

static void test_wifi_rejects_unterminated_strings() {
    FakeKv kv;
    WifiCredentialStore store(kv);
    WifiCredentials saved = makeCredentials("HomeNet", "secret123");
    memset(saved.ssid, 'a', sizeof(saved.ssid));
    kv.put(configstore::kWifiCredentialsKey, &saved, sizeof(saved));
    WifiCredentials loaded = {};
    TEST_ASSERT_FALSE(store.load(loaded));

    saved = makeCredentials("HomeNet", "secret123");
    memset(saved.password, 'b', sizeof(saved.password));
    kv.put(configstore::kWifiCredentialsKey, &saved, sizeof(saved));
    TEST_ASSERT_FALSE(store.load(loaded));
}

// Пароль может быть пустым (открытая сеть)
static void test_wifi_accepts_empty_password() {
    FakeKv kv;
    WifiCredentialStore store(kv);
    WifiCredentials saved = makeCredentials("OpenNet", "");
    kv.put(configstore::kWifiCredentialsKey, &saved, sizeof(saved));
    WifiCredentials loaded = {};
    TEST_ASSERT_TRUE(store.load(loaded));
    TEST_ASSERT_EQUAL_STRING("OpenNet", loaded.ssid);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_missing_key_creates_defaults);
    RUN_TEST(test_missing_key_save_failed);
    RUN_TEST(test_current_blob_loaded_without_write);
    RUN_TEST(test_invalid_channel_reset_and_rewritten);
    RUN_TEST(test_migrated_save_failed_keeps_configs);
    RUN_TEST(test_legacy_single_v3_migrated_to_current_size);
    RUN_TEST(test_legacy_single_unknown_version_is_corrupt);
    RUN_TEST(test_corrupt_save_failed);
    RUN_TEST(test_legacy_single_read_failure_not_overwritten);
    RUN_TEST(test_legacy_multi_read_failure_not_overwritten);
    RUN_TEST(test_current_read_failure_not_overwritten);
    RUN_TEST(test_info_read_failure_not_overwritten);
    RUN_TEST(test_transient_read_errors_retried);
    RUN_TEST(test_short_read_treated_as_read_failure);
    RUN_TEST(test_unknown_size_replaced_with_defaults);
    RUN_TEST(test_unknown_size_save_failed);
    RUN_TEST(test_save_then_load_roundtrip);
    RUN_TEST(test_save_failure_reported);
    RUN_TEST(test_wifi_roundtrip);
    RUN_TEST(test_wifi_missing_or_unreadable);
    RUN_TEST(test_wifi_rejects_wrong_size);
    RUN_TEST(test_wifi_rejects_empty_ssid);
    RUN_TEST(test_wifi_rejects_unterminated_strings);
    RUN_TEST(test_wifi_accepts_empty_password);
    return UNITY_END();
}
