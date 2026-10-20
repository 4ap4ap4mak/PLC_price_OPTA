// Тесты lib/prices: ряд 15-минутных слотов и агрегация в часы.
#include <unity.h>

#include <PriceSeries.h>

#include "../helpers/TestHelpers.h"

using prices::kMaxSlots;
using prices::PriceHour;
using prices::PriceSeries;
using testing::utc;

void setUp() {}
void tearDown() {}

static PriceSeries series;

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

// Особенность: группируются только соседние слоты; порядок не проверяется.
static void test_build_hours_groups_only_adjacent_slots() {
    const uint32_t start = utc(2026, 10, 20, 10);
    series.clear();
    series.append(start, 0.1f);
    series.append(start + 3600, 0.2f);
    series.append(start + 900, 0.3f);
    PriceHour hours[4];
    TEST_ASSERT_EQUAL_size_t(3, series.buildHours(hours, 4));
    TEST_ASSERT_EQUAL_UINT32(start, hours[2].start);
    TEST_ASSERT_EQUAL_UINT8(1, hours[2].quarters);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_append_until_full);
    RUN_TEST(test_build_hours_averages_quarters);
    RUN_TEST(test_build_hours_respects_capacity);
    RUN_TEST(test_build_hours_groups_only_adjacent_slots);
    return UNITY_END();
}
