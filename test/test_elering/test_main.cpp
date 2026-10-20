// Тесты lib/elering: разбор ответов API на реальных фикстурах и путь запроса прогноза.
#include <math.h>
#include <string.h>
#include <unity.h>

#include <Elering.h>

#include "../fixtures/EleringFixtures.h"
#include "../helpers/TestHelpers.h"

using namespace elering;
using testing::utc;

void setUp() {}
void tearDown() {}

static Error current(const char* json, uint32_t now, CurrentPrice& out) {
    const char* jsonError = nullptr;
    return parseCurrent(json, strlen(json), now, out, jsonError);
}

static void test_current_real_response() {
    CurrentPrice price = {};
    const uint32_t now = 1792512900UL + 600;  // 10 минут после начала интервала
    ASSERT_ENUM(Error::Ok, current(kCurrentResponse, now, price));
    TEST_ASSERT_EQUAL(1792512900L, price.timestamp);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.14704f, price.eurKwh);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 147.04f, price.eurMwh);
}

static void test_current_negative_price() {
    CurrentPrice price = {};
    ASSERT_ENUM(Error::Ok, current(
        "{\"success\":true,\"data\":[{\"timestamp\":1792512900,\"price\":-3.5}]}", 0, price));
    TEST_ASSERT_FLOAT_WITHIN(1e-7f, -0.0035f, price.eurKwh);
}

static void test_current_freshness() {
    const char* json = "{\"success\":true,\"data\":[{\"timestamp\":1792512900,\"price\":10}]}";
    CurrentPrice price = {};
    // now == 0: часы не синхронизированы, проверка пропускается
    ASSERT_ENUM(Error::Ok, current(json, 0, price));
    ASSERT_ENUM(Error::Ok, current(json, 1792512900UL + 3599, price));
    ASSERT_ENUM(Error::Stale, current(json, 1792512900UL + 3600, price));
    ASSERT_ENUM(Error::Ok, current(json, 1792512900UL, price));
    ASSERT_ENUM(Error::Stale, current(json, 1792512900UL - 301, price));
    TEST_ASSERT_EQUAL(1792512900L, price.timestamp);  // заполнен для лога
}

// KI-015: допуск «цена впереди часов на 5 минут» не работает — разность now - timestamp
// в uint32_t переполняется, и любая цена из будущего считается устаревшей.
static void test_bug_ki015_future_price_rejected() {
    const char* json = "{\"success\":true,\"data\":[{\"timestamp\":1792512900,\"price\":10}]}";
    CurrentPrice price = {};
    ASSERT_ENUM(Error::Stale, current(json, 1792512900UL - 1, price));
    ASSERT_ENUM(Error::Stale, current(json, 1792512900UL - 300, price));
}

static void test_current_rejects() {
    CurrentPrice price = {};
    const char* jsonError = nullptr;
    const char* broken = "{\"success\":true,";
    ASSERT_ENUM(Error::Json, parseCurrent(broken, strlen(broken), 0, price, jsonError));
    TEST_ASSERT_NOT_NULL(jsonError);
    TEST_ASSERT_TRUE(strlen(jsonError) > 0);

    ASSERT_ENUM(Error::Format, current("{\"success\":false,\"data\":[{\"timestamp\":1792512900,\"price\":1}]}", 0, price));
    ASSERT_ENUM(Error::Format, current("{\"success\":true,\"data\":[]}", 0, price));
    ASSERT_ENUM(Error::Format, current("{\"success\":true}", 0, price));
    ASSERT_ENUM(Error::NoPrice, current("{\"success\":true,\"data\":[{\"timestamp\":1792512900}]}", 0, price));
    ASSERT_ENUM(Error::NoPrice, current("{\"success\":true,\"data\":[{\"timestamp\":1792512900,\"price\":\"12\"}]}", 0, price));
    ASSERT_ENUM(Error::NoPrice, current("{\"success\":true,\"data\":[{\"price\":12}]}", 0, price));
    ASSERT_ENUM(Error::OutOfRange, current("{\"success\":true,\"data\":[{\"timestamp\":1792512900,\"price\":10001}]}", 0, price));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 10001.0f, price.eurMwh);
    ASSERT_ENUM(Error::OutOfRange, current("{\"success\":true,\"data\":[{\"timestamp\":1792512900,\"price\":-1001}]}", 0, price));
    ASSERT_ENUM(Error::BadTimestamp, current("{\"success\":true,\"data\":[{\"timestamp\":1735689599,\"price\":10}]}", 0, price));
    ASSERT_ENUM(Error::Ok, current("{\"success\":true,\"data\":[{\"timestamp\":1735689600,\"price\":10}]}", 0, price));
}

static void test_json_object_bounds() {
    size_t start = 0;
    size_t end = 0;
    const char* body = "garbage{\"a\":{\"b\":1}}tail";
    TEST_ASSERT_TRUE(jsonObjectBounds(body, strlen(body), start, end));
    TEST_ASSERT_EQUAL_size_t(7, start);
    TEST_ASSERT_EQUAL_size_t(20, end);
    TEST_ASSERT_FALSE(jsonObjectBounds("no json", 7, start, end));
    TEST_ASSERT_FALSE(jsonObjectBounds("}{", 2, start, end));
}

static prices::PriceSeries series;

static Error forecast(const char* json) {
    const char* jsonError = nullptr;
    return parseForecast(json, strlen(json), series, jsonError);
}

static void test_forecast_real_two_days() {
    ASSERT_ENUM(Error::Ok, forecast(kForecastResponse));
    TEST_ASSERT_EQUAL_size_t(192, series.size());
    // Берется только зона LV, хотя в ответе есть EE, FI и LT
    TEST_ASSERT_EQUAL_UINT32(1792357200UL, series[0].start);  // 19.10.2026 00:00 EEST
    TEST_ASSERT_EQUAL_UINT32(1792529100UL, series[191].start);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.02647f, series[191].eurKwh);
    for (size_t i = 1; i < series.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT32(series[i - 1].start + 900, series[i].start);
    }
}

static void test_forecast_real_dst_day_has_25_hours() {
    ASSERT_ENUM(Error::Ok, forecast(kForecastDstResponse));
    TEST_ASSERT_EQUAL_size_t(196, series.size());
    TEST_ASSERT_EQUAL_UINT32(1761426000UL, series[0].start);  // 26.10.2025 00:00 EEST
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.02078f, series[0].eurKwh);
    prices::PriceHour hours[60];
    TEST_ASSERT_EQUAL_size_t(49, series.buildHours(hours, 60));
}

static void test_forecast_empty_and_broken() {
    series.append(1, 1.0f);
    ASSERT_ENUM(Error::Empty, forecast(kForecastFutureResponse));
    TEST_ASSERT_EQUAL_size_t(0, series.size());
    ASSERT_ENUM(Error::Format, forecast("{\"success\":true,\"data\":{\"ee\":[]}}"));
    ASSERT_ENUM(Error::Format, forecast("{\"success\":false,\"data\":{\"lv\":[]}}"));
    ASSERT_ENUM(Error::Json, forecast("not json"));
    TEST_ASSERT_EQUAL_size_t(0, series.size());
}

static void test_forecast_skips_bad_items() {
    ASSERT_ENUM(Error::Ok, forecast(
        "{\"success\":true,\"data\":{\"lv\":["
        "{\"timestamp\":0,\"price\":1},"
        "{\"price\":2},"
        "{\"timestamp\":1792357200,\"price\":50},"
        "{\"timestamp\":1792358100,\"price\":-20}]}}"));
    TEST_ASSERT_EQUAL_size_t(2, series.size());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.02f, series[1].eurKwh);
}

static void test_forecast_stops_at_capacity() {
    static char json[16000];
    size_t length = 0;
    length += snprintf(json + length, sizeof(json) - length, "{\"success\":true,\"data\":{\"lv\":[");
    for (int i = 0; i < 320; ++i) {
        length += snprintf(json + length, sizeof(json) - length, "%s{\"timestamp\":%u,\"price\":1}",
            i == 0 ? "" : ",", 1792357200U + i * 900U);
    }
    snprintf(json + length, sizeof(json) - length, "]}}");
    ASSERT_ENUM(Error::Ok, forecast(json));
    TEST_ASSERT_EQUAL_size_t(prices::kMaxSlots, series.size());
}

static void test_forecast_path() {
    char path[96];
    // 20.10.2026 13:00 EEST → местные 20.10 00:00 … 21.10 23:45
    TEST_ASSERT_TRUE(forecastPath(utc(2026, 10, 20, 10), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(
        "/api/nps/price?start=2026-10-19T21:00:00.000Z&end=2026-10-21T20:45:00.000Z", path);
    // Зимой смещение +2
    TEST_ASSERT_TRUE(forecastPath(utc(2026, 1, 15, 10), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(
        "/api/nps/price?start=2026-01-14T22:00:00.000Z&end=2026-01-16T21:45:00.000Z", path);
    // Период захватывает переход на зимнее время 25.10.2026
    TEST_ASSERT_TRUE(forecastPath(utc(2026, 10, 24, 10), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(
        "/api/nps/price?start=2026-10-23T21:00:00.000Z&end=2026-10-25T21:45:00.000Z", path);
    TEST_ASSERT_FALSE(forecastPath(utc(2026, 10, 20, 10), path, 40));
}

static void test_day_prices_path() {
    char path[96];
    // Вчерашние сутки 19.10.2026 (летнее время)
    TEST_ASSERT_TRUE(dayPricesPath(rigatime::daysFromCivil(2026, 10, 19), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(
        "/api/nps/price?start=2026-10-18T21:00:00.000Z&end=2026-10-19T20:45:00.000Z", path);
    // Сутки перехода на зимнее время — 25 часов
    TEST_ASSERT_TRUE(dayPricesPath(rigatime::daysFromCivil(2026, 10, 25), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(
        "/api/nps/price?start=2026-10-24T21:00:00.000Z&end=2026-10-25T21:45:00.000Z", path);
}

// Слот не позже предыдущего пропускается: ряд должен идти строго по времени.
static void test_forecast_skips_out_of_order() {
    ASSERT_ENUM(Error::Ok, forecast(
        "{\"success\":true,\"data\":{\"lv\":["
        "{\"timestamp\":1792358100,\"price\":10},"
        "{\"timestamp\":1792357200,\"price\":20},"
        "{\"timestamp\":1792359000,\"price\":30}]}}"));
    TEST_ASSERT_EQUAL_size_t(2, series.size());
    TEST_ASSERT_EQUAL_UINT32(1792359000UL, series[1].start);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_current_real_response);
    RUN_TEST(test_current_negative_price);
    RUN_TEST(test_current_freshness);
    RUN_TEST(test_bug_ki015_future_price_rejected);
    RUN_TEST(test_current_rejects);
    RUN_TEST(test_json_object_bounds);
    RUN_TEST(test_forecast_real_two_days);
    RUN_TEST(test_forecast_real_dst_day_has_25_hours);
    RUN_TEST(test_forecast_empty_and_broken);
    RUN_TEST(test_forecast_skips_bad_items);
    RUN_TEST(test_forecast_stops_at_capacity);
    RUN_TEST(test_forecast_path);
    RUN_TEST(test_day_prices_path);
    RUN_TEST(test_forecast_skips_out_of_order);
    return UNITY_END();
}
