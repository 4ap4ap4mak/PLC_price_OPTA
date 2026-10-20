// Характеризационные тесты lib/rigatime: календарь, DST Europe/Riga, текстовые форматы.
// Эталонные epoch посчитаны независимо (Python zoneinfo).
#include <string.h>
#include <unity.h>

#include <RigaTime.h>

#include "../helpers/TestHelpers.h"

using namespace rigatime;
using testing::utc;

void setUp() {}
void tearDown() {}

static void test_days_from_civil_known_dates() {
    TEST_ASSERT_EQUAL_INT64(0, daysFromCivil(1970, 1, 1));
    TEST_ASSERT_EQUAL_INT64(11017, daysFromCivil(2000, 3, 1));
    TEST_ASSERT_EQUAL_INT64(20746, daysFromCivil(2026, 10, 20));
    TEST_ASSERT_EQUAL_INT64(-1, daysFromCivil(1969, 12, 31));
}

static void test_civil_from_days_round_trip() {
    for (int64_t days = -800; days < 30000; days += 7) {
        int year;
        unsigned month;
        unsigned day;
        civilFromDays(days, year, month, day);
        TEST_ASSERT_EQUAL_INT64(days, daysFromCivil(year, month, day));
    }
    int year;
    unsigned month;
    unsigned day;
    civilFromDays(20746, year, month, day);
    TEST_ASSERT_EQUAL_INT(2026, year);
    TEST_ASSERT_EQUAL_UINT(10, month);
    TEST_ASSERT_EQUAL_UINT(20, day);
}

static void test_last_sunday_of_month() {
    TEST_ASSERT_EQUAL_UINT(29, lastSundayOfMonth(2026, 3));
    TEST_ASSERT_EQUAL_UINT(25, lastSundayOfMonth(2026, 10));
    TEST_ASSERT_EQUAL_UINT(31, lastSundayOfMonth(2024, 3));
    TEST_ASSERT_EQUAL_UINT(27, lastSundayOfMonth(2024, 10));
    TEST_ASSERT_EQUAL_UINT(25, lastSundayOfMonth(2024, 2));  // високосный: 29.02.2024 — четверг
}

static void test_dst_boundaries_2026() {
    const uint32_t springForward = 1774746000UL;  // 2026-03-29 01:00 UTC
    const uint32_t fallBack = 1792890000UL;       // 2026-10-25 01:00 UTC
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 3, 29, 1), springForward);
    TEST_ASSERT_FALSE(isDaylightSaving(springForward - 1));
    TEST_ASSERT_TRUE(isDaylightSaving(springForward));
    TEST_ASSERT_TRUE(isDaylightSaving(fallBack - 1));
    TEST_ASSERT_FALSE(isDaylightSaving(fallBack));
    TEST_ASSERT_FALSE(isDaylightSaving(utc(2026, 1, 15, 12)));
    TEST_ASSERT_TRUE(isDaylightSaving(utc(2026, 7, 15, 12)));
}

static void test_to_local_offsets() {
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 1, 15, 14), toLocal(utc(2026, 1, 15, 12)));
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 7, 15, 15), toLocal(utc(2026, 7, 15, 12)));
    // Перед переходом на зимнее время: 00:59 UTC = 03:59 местного
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 10, 25, 3, 59), toLocal(utc(2026, 10, 25, 0, 59)));
    // После: 01:00 UTC = 03:00 местного — час повторяется
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 10, 25, 3, 0), toLocal(utc(2026, 10, 25, 1, 0)));
}

static void test_local_hour_and_day() {
    uint8_t hour;
    int64_t day;
    // 2026-10-19 21:30 UTC = 2026-10-20 00:30 EEST
    localHourAndDay(utc(2026, 10, 19, 21, 30), hour, day);
    TEST_ASSERT_EQUAL_UINT8(0, hour);
    TEST_ASSERT_EQUAL_INT64(20746, day);
    localHourAndDay(utc(2026, 10, 20, 20, 59), hour, day);
    TEST_ASSERT_EQUAL_UINT8(23, hour);
    TEST_ASSERT_EQUAL_INT64(20746, day);
}

static void test_parse_time_of_day() {
    uint16_t minute = 0;
    TEST_ASSERT_TRUE(parseTimeOfDay("07:30", 5, minute));
    TEST_ASSERT_EQUAL_UINT16(450, minute);
    TEST_ASSERT_TRUE(parseTimeOfDay("00:00", 5, minute));
    TEST_ASSERT_EQUAL_UINT16(0, minute);
    TEST_ASSERT_TRUE(parseTimeOfDay("23:59", 5, minute));
    TEST_ASSERT_EQUAL_UINT16(1439, minute);
    TEST_ASSERT_FALSE(parseTimeOfDay("24:00", 5, minute));
    TEST_ASSERT_FALSE(parseTimeOfDay("12:60", 5, minute));
    TEST_ASSERT_FALSE(parseTimeOfDay("7:30", 4, minute));
    TEST_ASSERT_FALSE(parseTimeOfDay("07-30", 5, minute));
    TEST_ASSERT_FALSE(parseTimeOfDay("0a:00", 5, minute));
    TEST_ASSERT_FALSE(parseTimeOfDay("07:300", 6, minute));
    TEST_ASSERT_FALSE(parseTimeOfDay("", 0, minute));
}

static void test_format_time_of_day() {
    char text[8];
    formatTimeOfDay(450, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("07:30", text);
    formatTimeOfDay(0, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("00:00", text);
    formatTimeOfDay(1439, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("23:59", text);
}

static bool parse(const char* text, uint32_t& epoch) {
    return parseLocalDateTime(text, strlen(text), epoch);
}

static void test_parse_local_date_time() {
    uint32_t epoch = 0;
    TEST_ASSERT_TRUE(parse("2026-10-20T07:30", epoch));
    TEST_ASSERT_EQUAL_UINT32(1792470600UL, epoch);  // 04:30 UTC
    TEST_ASSERT_TRUE(parse("2026-01-15T12:00", epoch));
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 1, 15, 10), epoch);
}

static void test_parse_local_date_time_dst_gap_and_overlap() {
    uint32_t epoch = 0;
    // 29.03.2026: местные 03:00–03:59 не существуют
    TEST_ASSERT_FALSE(parse("2026-03-29T03:30", epoch));
    TEST_ASSERT_TRUE(parse("2026-03-29T02:59", epoch));
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 3, 29, 0, 59), epoch);
    TEST_ASSERT_TRUE(parse("2026-03-29T04:00", epoch));
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 3, 29, 1, 0), epoch);
    // 25.10.2026: местные 03:00–03:59 бывают дважды, выбирается второй раз (зимнее время)
    TEST_ASSERT_TRUE(parse("2026-10-25T03:30", epoch));
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 10, 25, 1, 30), epoch);
}

static void test_parse_local_date_time_rejects() {
    uint32_t epoch = 0;
    TEST_ASSERT_FALSE(parse("2026-02-30T10:00", epoch));
    TEST_ASSERT_FALSE(parse("2023-12-31T10:00", epoch));
    TEST_ASSERT_FALSE(parse("2039-01-01T10:00", epoch));
    TEST_ASSERT_FALSE(parse("2026-13-01T10:00", epoch));
    TEST_ASSERT_FALSE(parse("2026-10-20T24:00", epoch));
    TEST_ASSERT_FALSE(parse("2026-10-20 07:30", epoch));
    TEST_ASSERT_FALSE(parse("2026-10-20T07:30:00", epoch));
    TEST_ASSERT_FALSE(parse("", epoch));
}

// Особенность монолита: поля разбираются как atol, хвост после цифр игнорируется.
static void test_parse_local_date_time_atol_quirk() {
    uint32_t epoch = 0;
    TEST_ASSERT_TRUE(parse("2026-1a-20T07:30", epoch));
    TEST_ASSERT_EQUAL_UINT32(utc(2026, 1, 20, 5, 30), epoch);  // 20.01 07:30 зимнего времени
}

static void test_format_local_date_time() {
    char text[17];
    formatLocalDateTime(1792470600UL, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("2026-10-20T07:30", text);
    formatLocalDateTime(utc(2026, 10, 25, 1, 30), text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("2026-10-25T03:30", text);
    for (uint32_t t = utc(2026, 3, 28); t < utc(2026, 3, 30); t += 900) {
        formatLocalDateTime(t, text, sizeof(text));
        uint32_t back = 0;
        TEST_ASSERT_TRUE(parse(text, back));
        TEST_ASSERT_EQUAL_UINT32(t, back);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_days_from_civil_known_dates);
    RUN_TEST(test_civil_from_days_round_trip);
    RUN_TEST(test_last_sunday_of_month);
    RUN_TEST(test_dst_boundaries_2026);
    RUN_TEST(test_to_local_offsets);
    RUN_TEST(test_local_hour_and_day);
    RUN_TEST(test_parse_time_of_day);
    RUN_TEST(test_format_time_of_day);
    RUN_TEST(test_parse_local_date_time);
    RUN_TEST(test_parse_local_date_time_dst_gap_and_overlap);
    RUN_TEST(test_parse_local_date_time_rejects);
    RUN_TEST(test_parse_local_date_time_atol_quirk);
    RUN_TEST(test_format_local_date_time);
    return UNITY_END();
}
