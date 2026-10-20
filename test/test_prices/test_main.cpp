// Тесты lib/prices: ряд 15-минутных слотов, поиск по времени, склейка рядов,
// агрегация в часы.
#include <unity.h>

#include <PriceSeries.h>

#include "../helpers/TestHelpers.h"

using prices::kMaxSlots;
using prices::kSlotSeconds;
using prices::PriceHour;
using prices::PriceSeries;
using testing::utc;

void setUp() {}
void tearDown() {}

static PriceSeries series;
static PriceSeries other;

static void fill(PriceSeries& out, uint32_t start, size_t count, float price) {
    out.clear();
    for (size_t i = 0; i < count; ++i) {
        out.append(start + static_cast<uint32_t>(i) * kSlotSeconds, price);
    }
}

static void test_append_until_full() {
    series.clear();
    for (size_t i = 0; i < kMaxSlots; ++i) {
        TEST_ASSERT_TRUE(series.append(1000 + static_cast<uint32_t>(i) * 900, 0.1f));
    }
    TEST_ASSERT_FALSE(series.append(999999, 0.2f));
    TEST_ASSERT_EQUAL_size_t(kMaxSlots, series.size());
    TEST_ASSERT_EQUAL_UINT32(1000, series[0].start);
    series.clear();
    TEST_ASSERT_EQUAL_size_t(0, series.size());
}

// Поиск держится на порядке: слот не позже последнего не добавляется.
static void test_append_rejects_out_of_order() {
    const uint32_t start = utc(2026, 10, 20, 10);
    series.clear();
    TEST_ASSERT_TRUE(series.append(start, 0.1f));
    TEST_ASSERT_TRUE(series.append(start + 3600, 0.2f));
    TEST_ASSERT_FALSE(series.append(start + 900, 0.3f));
    TEST_ASSERT_FALSE(series.append(start + 3600, 0.3f));
    TEST_ASSERT_EQUAL_size_t(2, series.size());
}

static void test_index_lookup_and_coverage() {
    const uint32_t start = utc(2026, 10, 20, 10);
    series.clear();
    TEST_ASSERT_EQUAL_INT(-1, series.indexAt(start));
    TEST_ASSERT_EQUAL_UINT32(0, series.coverageEnd());
    series.append(start, 0.1f);
    series.append(start + 900, 0.2f);
    series.append(start + 2700, 0.3f);  // пропуск 10:30–10:45
    TEST_ASSERT_EQUAL_INT(-1, series.indexAt(start - 1));
    TEST_ASSERT_EQUAL_INT(0, series.indexAt(start));
    TEST_ASSERT_EQUAL_INT(0, series.indexAt(start + 899));
    TEST_ASSERT_EQUAL_INT(1, series.indexAt(start + 900));
    TEST_ASSERT_EQUAL_INT(-1, series.indexAt(start + 1800));
    TEST_ASSERT_EQUAL_INT(2, series.indexAt(start + 3599));
    TEST_ASSERT_EQUAL_INT(-1, series.indexAt(start + 3600));
    TEST_ASSERT_EQUAL_size_t(0, series.lowerBound(start));
    TEST_ASSERT_EQUAL_size_t(1, series.lowerBound(start + 1));
    TEST_ASSERT_EQUAL_size_t(2, series.lowerBound(start + 1800));
    TEST_ASSERT_EQUAL_size_t(3, series.lowerBound(start + 3600));
    TEST_ASSERT_EQUAL_UINT32(start + 3600, series.coverageEnd());
}

// Новый прогноз «сегодня–завтра» заменяет ряд, вчерашние слоты сохраняются.
static void test_replace_keeping_older() {
    const uint32_t yesterday = utc(2026, 10, 18, 21);
    const uint32_t today = yesterday + 86400;
    fill(series, yesterday, 192, 0.1f);           // вчера + сегодня
    fill(other, today, 192, 0.2f);                // сегодня + завтра
    series.replaceKeepingOlder(yesterday, other);
    TEST_ASSERT_EQUAL_size_t(288, series.size());
    TEST_ASSERT_EQUAL_UINT32(yesterday, series[0].start);
    TEST_ASSERT_EQUAL_FLOAT(0.1f, series[95].eurKwh);
    TEST_ASSERT_EQUAL_UINT32(today, series[96].start);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, series[96].eurKwh);

    // На следующие сутки позавчерашние слоты отбрасываются
    fill(other, today + 86400, 192, 0.3f);
    series.replaceKeepingOlder(today, other);
    TEST_ASSERT_EQUAL_size_t(288, series.size());
    TEST_ASSERT_EQUAL_UINT32(today, series[0].start);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, series[0].eurKwh);
    TEST_ASSERT_EQUAL_FLOAT(0.3f, series[96].eurKwh);

    // Пустой старый ряд — просто копия нового
    series.clear();
    series.replaceKeepingOlder(yesterday, other);
    TEST_ASSERT_EQUAL_size_t(192, series.size());
}

// Цены за вчера, загруженные отдельно после перезагрузки, ставятся в начало.
static void test_prepend_older() {
    const uint32_t yesterday = utc(2026, 10, 18, 21);
    const uint32_t today = yesterday + 86400;
    fill(series, today, 192, 0.2f);
    fill(other, yesterday, 100, 0.1f);  // 4 слота залезают на сегодня — не берутся
    series.prependOlder(other);
    TEST_ASSERT_EQUAL_size_t(288, series.size());
    TEST_ASSERT_EQUAL_UINT32(yesterday, series[0].start);
    TEST_ASSERT_EQUAL_FLOAT(0.1f, series[95].eurKwh);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, series[96].eurKwh);
    // Не больше емкости: берутся последние вчерашние слоты, без разрыва перед сегодняшними
    fill(series, today, kMaxSlots - 10, 0.2f);
    series.prependOlder(other);
    TEST_ASSERT_EQUAL_size_t(kMaxSlots, series.size());
    TEST_ASSERT_EQUAL_UINT32(today - 10 * kSlotSeconds, series[0].start);
    TEST_ASSERT_EQUAL_UINT32(today, series[10].start);
}

static void test_build_hours_averages_quarters() {
    const uint32_t start = utc(2026, 10, 20, 10);
    series.clear();
    series.append(start, 0.10f);
    series.append(start + 900, 0.20f);
    series.append(start + 1800, 0.30f);
    series.append(start + 2700, 0.40f);
    series.append(start + 3600, -0.05f);
    series.append(start + 4500, -0.15f);

    PriceHour hours[4];
    TEST_ASSERT_EQUAL_size_t(2, series.buildHours(hours, 4));
    TEST_ASSERT_EQUAL_UINT32(start, hours[0].start);
    TEST_ASSERT_EQUAL_UINT8(4, hours[0].quarters);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, hours[0].avgEurKwh);
    TEST_ASSERT_EQUAL_UINT32(start + 3600, hours[1].start);
    TEST_ASSERT_EQUAL_UINT8(2, hours[1].quarters);  // неполный час
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.10f, hours[1].avgEurKwh);
}

static void test_build_hours_respects_capacity() {
    const float prices[3] = {0.1f, 0.2f, 0.3f};
    testing::fillHours(series, utc(2026, 10, 20), prices, 3);
    PriceHour hours[2];
    TEST_ASSERT_EQUAL_size_t(2, series.buildHours(hours, 2));
    TEST_ASSERT_EQUAL_UINT8(4, hours[1].quarters);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_append_until_full);
    RUN_TEST(test_append_rejects_out_of_order);
    RUN_TEST(test_index_lookup_and_coverage);
    RUN_TEST(test_replace_keeping_older);
    RUN_TEST(test_prepend_older);
    RUN_TEST(test_build_hours_averages_quarters);
    RUN_TEST(test_build_hours_respects_capacity);
    return UNITY_END();
}
