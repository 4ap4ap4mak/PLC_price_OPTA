// Тесты lib/channelconfig: значения по умолчанию, проверка, формат блоба во flash
// и миграция старых версий. Раскладка полей зафиксирована побайтно: если тест
// test_blob_layout_is_frozen упал, пользователи потеряют настройки — нужна новая
// версия формата и миграция (CLAUDE.md, правило 5).
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <unity.h>

#include <ChannelConfig.h>

#include "../helpers/TestHelpers.h"

using namespace channelconfig;

void setUp() {}
void tearDown() {}

static void test_blob_layout_is_frozen() {
    TEST_ASSERT_EQUAL_size_t(36, sizeof(ChannelConfig));
    TEST_ASSERT_EQUAL_size_t(0, offsetof(ChannelConfig, version));
    TEST_ASSERT_EQUAL_size_t(4, offsetof(ChannelConfig, safetyCeilingEurKwh));
    TEST_ASSERT_EQUAL_size_t(8, offsetof(ChannelConfig, alwaysOnBelowPriceEurKwh));
    TEST_ASSERT_EQUAL_size_t(12, offsetof(ChannelConfig, nightPrepEnabled));
    TEST_ASSERT_EQUAL_size_t(14, offsetof(ChannelConfig, nightStartMinuteOfDay));
    TEST_ASSERT_EQUAL_size_t(16, offsetof(ChannelConfig, nightEndMinuteOfDay));
    TEST_ASSERT_EQUAL_size_t(18, offsetof(ChannelConfig, nightCheapHours));
    TEST_ASSERT_EQUAL_size_t(19, offsetof(ChannelConfig, unusedBlockEnabled));
    TEST_ASSERT_EQUAL_size_t(20, offsetof(ChannelConfig, forceOnHours));
    TEST_ASSERT_EQUAL_size_t(21, offsetof(ChannelConfig, holidayEnabled));
    TEST_ASSERT_EQUAL_size_t(24, offsetof(ChannelConfig, holidayReturnEpoch));
    TEST_ASSERT_EQUAL_size_t(28, offsetof(ChannelConfig, holidayOnHours));
    TEST_ASSERT_EQUAL_size_t(29, offsetof(ChannelConfig, smartThresholdEnabled));
    TEST_ASSERT_EQUAL_size_t(30, offsetof(ChannelConfig, smartThresholdHours));
    TEST_ASSERT_EQUAL_size_t(31, offsetof(ChannelConfig, wakeupEnabled));
    TEST_ASSERT_EQUAL_size_t(32, offsetof(ChannelConfig, wakeupHours));
    TEST_ASSERT_EQUAL_size_t(34, offsetof(ChannelConfig, wakeupTimeMinuteOfDay));
    TEST_ASSERT_EQUAL_size_t(32, kLegacySingleBlobSize);
    TEST_ASSERT_EQUAL_size_t(128, kLegacyMultiBlobSize);
    TEST_ASSERT_EQUAL_size_t(144, kBlobSize);
}

static void test_defaults() {
    ChannelConfig config;
    memset(&config, 0xAB, sizeof(config));
    setDefaults(config);
    TEST_ASSERT_EQUAL_UINT16(5, config.version);
    TEST_ASSERT_EQUAL_FLOAT(0.3f, config.safetyCeilingEurKwh);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, config.alwaysOnBelowPriceEurKwh);
    TEST_ASSERT_FALSE(config.nightPrepEnabled);
    TEST_ASSERT_EQUAL_UINT16(23 * 60, config.nightStartMinuteOfDay);
    TEST_ASSERT_EQUAL_UINT16(7 * 60, config.nightEndMinuteOfDay);
    TEST_ASSERT_EQUAL_UINT8(4, config.nightCheapHours);
    TEST_ASSERT_EQUAL_UINT8(3, config.forceOnHours);
    TEST_ASSERT_EQUAL_UINT8(3, config.holidayOnHours);
    TEST_ASSERT_EQUAL_UINT32(0, config.holidayReturnEpoch);
    TEST_ASSERT_EQUAL_UINT8(4, config.smartThresholdHours);
    TEST_ASSERT_EQUAL_UINT8(3, config.wakeupHours);
    TEST_ASSERT_EQUAL_UINT16(7 * 60, config.wakeupTimeMinuteOfDay);
    TEST_ASSERT_TRUE(isValid(config));
}

static void test_is_valid_rejects() {
    ChannelConfig base;
    setDefaults(base);
    ChannelConfig c;

    c = base; c.version = 4;                              TEST_ASSERT_FALSE(isValid(c));
    c = base; c.safetyCeilingEurKwh = NAN;                TEST_ASSERT_FALSE(isValid(c));
    c = base; c.alwaysOnBelowPriceEurKwh = INFINITY;      TEST_ASSERT_FALSE(isValid(c));
    c = base; c.alwaysOnBelowPriceEurKwh = 0.3f;          TEST_ASSERT_FALSE(isValid(c));
    c = base; c.nightStartMinuteOfDay = 1440;             TEST_ASSERT_FALSE(isValid(c));
    c = base; c.nightEndMinuteOfDay = c.nightStartMinuteOfDay; TEST_ASSERT_FALSE(isValid(c));
    c = base; c.nightCheapHours = 0;                      TEST_ASSERT_FALSE(isValid(c));
    c = base; c.nightCheapHours = 25;                     TEST_ASSERT_FALSE(isValid(c));
    c = base; c.forceOnHours = 0;                TEST_ASSERT_FALSE(isValid(c));
    c = base; c.holidayOnHours = 25;                      TEST_ASSERT_FALSE(isValid(c));
    c = base; c.holidayEnabled = true;                    TEST_ASSERT_FALSE(isValid(c));  // нет даты возвращения
    c = base; c.smartThresholdHours = 0;                  TEST_ASSERT_FALSE(isValid(c));
    c = base; c.wakeupHours = 25;                         TEST_ASSERT_FALSE(isValid(c));
    c = base; c.wakeupTimeMinuteOfDay = 1440;             TEST_ASSERT_FALSE(isValid(c));
}

static void test_is_valid_night_window() {
    ChannelConfig c;
    setDefaults(c);  // окно 23:00–07:00 = 8 часов
    c.nightPrepEnabled = true;
    c.nightCheapHours = 8;
    TEST_ASSERT_TRUE(isValid(c));
    c.nightCheapHours = 9;
    TEST_ASSERT_FALSE(isValid(c));
    c.nightPrepEnabled = false;  // проверка окна только для включенного сценария
    TEST_ASSERT_TRUE(isValid(c));
    c.nightPrepEnabled = true;
    c.nightStartMinuteOfDay = 60;
    c.nightEndMinuteOfDay = 150;  // 1:30 — целых часов в окне 1
    c.nightCheapHours = 1;
    TEST_ASSERT_TRUE(isValid(c));
    c.nightCheapHours = 2;
    TEST_ASSERT_FALSE(isValid(c));
}

static void test_blob_kind() {
    ASSERT_ENUM(BlobKind::LegacySingle, blobKind(32));
    ASSERT_ENUM(BlobKind::LegacyMulti, blobKind(128));
    ASSERT_ENUM(BlobKind::Current, blobKind(144));
    ASSERT_ENUM(BlobKind::Unknown, blobKind(0));
    ASSERT_ENUM(BlobKind::Unknown, blobKind(36));
}

static void fillMarker(ChannelConfig out[kChannelCount]) {
    memset(out, 0x5A, sizeof(ChannelConfig) * kChannelCount);
}

static void test_decode_current_ok() {
    ChannelConfig stored[kChannelCount];
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        setDefaults(stored[i]);
        stored[i].smartThresholdEnabled = i % 2 == 0;
        stored[i].safetyCeilingEurKwh = 0.5f + i;
    }
    uint8_t blob[kBlobSize];
    memcpy(blob, stored, sizeof(blob));

    ChannelConfig out[kChannelCount];
    uint8_t mask = 0xFF;
    ASSERT_ENUM(DecodeResult::Ok, decode(blob, sizeof(blob), out, mask));
    TEST_ASSERT_EQUAL_UINT8(0, mask);
    TEST_ASSERT_EQUAL_MEMORY(stored, out, sizeof(stored));
}

static void test_decode_current_resets_invalid_channel() {
    ChannelConfig stored[kChannelCount];
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        setDefaults(stored[i]);
        stored[i].wakeupHours = 5;
    }
    stored[2].alwaysOnBelowPriceEurKwh = 1.0f;  // порог выше аварийного предела
    uint8_t blob[kBlobSize];
    memcpy(blob, stored, sizeof(blob));

    ChannelConfig out[kChannelCount];
    uint8_t mask = 0;
    ASSERT_ENUM(DecodeResult::Migrated, decode(blob, sizeof(blob), out, mask));
    TEST_ASSERT_EQUAL_UINT8(1U << 2, mask);
    TEST_ASSERT_EQUAL_UINT8(5, out[1].wakeupHours);
    TEST_ASSERT_EQUAL_UINT8(3, out[2].wakeupHours);  // сброшен на значения по умолчанию
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[2].alwaysOnBelowPriceEurKwh);
}

static LegacyChannelConfig legacyConfig(uint16_t version, float ceiling, float threshold) {
    LegacyChannelConfig legacy;
    memset(&legacy, 0, sizeof(legacy));
    legacy.version = version;
    legacy.safetyCeilingEurKwh = ceiling;
    legacy.alwaysOnBelowPriceEurKwh = threshold;
    legacy.nightPrepEnabled = true;
    legacy.nightStartMinuteOfDay = 22 * 60;
    legacy.nightEndMinuteOfDay = 6 * 60;
    legacy.nightCheapHours = 3;
    legacy.consecutiveBlockHours = 2;
    legacy.holidayOnHours = 5;
    return legacy;
}

static void test_decode_legacy_v2_scales_prices() {
    LegacyChannelConfig legacy = legacyConfig(2, 250.0f, 40.0f);  // v2: EUR/MWh
    uint8_t blob[kLegacySingleBlobSize];
    memcpy(blob, &legacy, sizeof(blob));

    ChannelConfig out[kChannelCount];
    uint8_t mask = 0;
    ASSERT_ENUM(DecodeResult::Migrated, decode(blob, sizeof(blob), out, mask));
    TEST_ASSERT_EQUAL_UINT8(0, mask);
    TEST_ASSERT_EQUAL_UINT16(5, out[0].version);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, out[0].safetyCeilingEurKwh);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.04f, out[0].alwaysOnBelowPriceEurKwh);
    TEST_ASSERT_TRUE(out[0].nightPrepEnabled);
    TEST_ASSERT_EQUAL_UINT16(22 * 60, out[0].nightStartMinuteOfDay);
    TEST_ASSERT_EQUAL_UINT8(3, out[0].nightCheapHours);
    TEST_ASSERT_EQUAL_UINT8(5, out[0].holidayOnHours);
    // Новых в v5 полей в v2 нет — они по умолчанию
    TEST_ASSERT_EQUAL_UINT8(4, out[0].smartThresholdHours);
    TEST_ASSERT_EQUAL_UINT16(7 * 60, out[0].wakeupTimeMinuteOfDay);
    // Остальные каналы — по умолчанию
    ChannelConfig defaults;
    setDefaults(defaults);
    for (uint8_t i = 1; i < kChannelCount; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&defaults, &out[i], sizeof(defaults));
    }
}

static void test_decode_legacy_v3_keeps_prices() {
    LegacyChannelConfig legacy = legacyConfig(3, 0.25f, 0.04f);
    uint8_t blob[kLegacySingleBlobSize];
    memcpy(blob, &legacy, sizeof(blob));
    ChannelConfig out[kChannelCount];
    uint8_t mask = 0;
    ASSERT_ENUM(DecodeResult::Migrated, decode(blob, sizeof(blob), out, mask));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, out[0].safetyCeilingEurKwh);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.04f, out[0].alwaysOnBelowPriceEurKwh);
}

static void test_decode_legacy_unknown_version_is_corrupt() {
    LegacyChannelConfig legacy = legacyConfig(7, 0.25f, 0.04f);
    uint8_t blob[kLegacySingleBlobSize];
    memcpy(blob, &legacy, sizeof(blob));
    ChannelConfig out[kChannelCount];
    fillMarker(out);
    ChannelConfig before[kChannelCount];
    memcpy(before, out, sizeof(before));
    uint8_t mask = 0;
    ASSERT_ENUM(DecodeResult::LegacyCorrupt, decode(blob, sizeof(blob), out, mask));
    TEST_ASSERT_EQUAL_MEMORY(before, out, sizeof(before));
}

static void test_decode_legacy_v4_four_channels() {
    LegacyChannelConfig legacy[kChannelCount];
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        legacy[i] = legacyConfig(4, 0.2f + i * 0.1f, 0.01f);
    }
    legacy[3].nightCheapHours = 0;  // некорректный канал → сброс
    uint8_t blob[kLegacyMultiBlobSize];
    memcpy(blob, legacy, sizeof(blob));

    ChannelConfig out[kChannelCount];
    uint8_t mask = 0;
    ASSERT_ENUM(DecodeResult::Migrated, decode(blob, sizeof(blob), out, mask));
    TEST_ASSERT_EQUAL_UINT8(1U << 3, mask);
    for (uint8_t i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_UINT16(5, out[i].version);
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.2f + i * 0.1f, out[i].safetyCeilingEurKwh);
        TEST_ASSERT_EQUAL_UINT8(3, out[i].nightCheapHours);
    }
    TEST_ASSERT_EQUAL_UINT8(4, out[3].nightCheapHours);
}

static void test_decode_unknown_size() {
    uint8_t blob[kBlobSize] = {};
    ChannelConfig out[kChannelCount];
    fillMarker(out);
    ChannelConfig before[kChannelCount];
    memcpy(before, out, sizeof(before));
    uint8_t mask = 0;
    ASSERT_ENUM(DecodeResult::UnknownSize, decode(blob, 100, out, mask));
    TEST_ASSERT_EQUAL_MEMORY(before, out, sizeof(before));
}

// Окно ночи — общий расчет для isValid, страницы и Home Assistant (D-044)
static void test_night_window_minutes() {
    using channelconfig::nightWindowMinutes;
    TEST_ASSERT_EQUAL_UINT16(8 * 60, nightWindowMinutes(23 * 60, 7 * 60));
    TEST_ASSERT_EQUAL_UINT16(2 * 60, nightWindowMinutes(1 * 60, 3 * 60));
    TEST_ASSERT_EQUAL_UINT16(1439, nightWindowMinutes(1, 0));
    TEST_ASSERT_EQUAL_UINT16(1, nightWindowMinutes(0, 1));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_blob_layout_is_frozen);
    RUN_TEST(test_defaults);
    RUN_TEST(test_is_valid_rejects);
    RUN_TEST(test_is_valid_night_window);
    RUN_TEST(test_blob_kind);
    RUN_TEST(test_decode_current_ok);
    RUN_TEST(test_decode_current_resets_invalid_channel);
    RUN_TEST(test_decode_legacy_v2_scales_prices);
    RUN_TEST(test_decode_legacy_v3_keeps_prices);
    RUN_TEST(test_decode_legacy_unknown_version_is_corrupt);
    RUN_TEST(test_decode_legacy_v4_four_channels);
    RUN_TEST(test_decode_unknown_size);
    RUN_TEST(test_night_window_minutes);
    return UNITY_END();
}
