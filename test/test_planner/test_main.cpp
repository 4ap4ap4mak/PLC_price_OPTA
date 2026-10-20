// Характеризационные тесты lib/planner: фиксируют ТЕКУЩЕЕ поведение монолита,
// включая известные баги. Тесты с префиксом test_bug_ описывают баги из
// docs/KNOWN_ISSUES.md: при их исправлении (шаг 2b/2c) такой тест должен быть
// переписан под новое поведение, а не удален молча.
//
// Опорные сутки: вторник 20.10.2026, летнее время (UTC+3). Прогноз — 48 часов
// с местной полуночи 20.10 до 23:59 21.10.
#include <string.h>
#include <unity.h>

#include <Planner.h>

#include "../helpers/TestHelpers.h"

using namespace planner;
using channelconfig::ChannelConfig;
using testing::quietConfig;
using testing::riga;

void setUp() {}
void tearDown() {}

static PriceSeries forecast;
static const float kBase = 0.20f;

// 48 часов по kBase; cheap — пары {номер часа от полуночи 20.10, цена}.
struct HourPrice {
    int hour;
    float price;
};
static void makeForecast(const HourPrice* cheap, size_t cheapCount, size_t hourCount = 48) {
    float prices[48];
    for (size_t i = 0; i < 48; ++i) prices[i] = kBase;
    for (size_t i = 0; i < cheapCount; ++i) prices[cheap[i].hour] = cheap[i].price;
    testing::fillHours(forecast, riga(2026, 10, 20, 0), prices, hourCount);
}

static bool decide(const ChannelConfig& config, uint32_t now, DecisionTrace& trace,
    float currentPrice = kBase, bool forecastAvailable = true) {
    BlockLock lock = {0, 0};
    return decideScenarios(config, currentPrice, forecastAvailable, forecast, now, lock, trace);
}

// --- Ступени решения ---------------------------------------------------------

static void test_ceiling_and_holiday_expiry_helpers() {
    ChannelConfig config = quietConfig();
    config.safetyCeilingEurKwh = 0.30f;
    TEST_ASSERT_FALSE(ceilingExceeded(config, 0.30f));
    TEST_ASSERT_TRUE(ceilingExceeded(config, 0.3001f));

    const uint32_t returnAt = riga(2026, 10, 21, 10);
    config.holidayEnabled = true;
    config.holidayReturnEpoch = returnAt;
    TEST_ASSERT_FALSE(holidayExpired(config, returnAt - 1));
    TEST_ASSERT_TRUE(holidayExpired(config, returnAt));
    config.holidayEnabled = false;
    TEST_ASSERT_FALSE(holidayExpired(config, returnAt + 100));
}

static void test_threshold_only() {
    ChannelConfig config = quietConfig();
    config.alwaysOnBelowPriceEurKwh = 0.05f;
    DecisionTrace trace;
    // Порог не требует прогноза
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 12), trace, 0.05f, false));
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 12), trace, 0.0501f, false));
    ASSERT_ENUM(HolidayStatus::Inactive, trace.holiday);
    ASSERT_ENUM(ScenarioStatus::Disabled, trace.smart);
    ASSERT_ENUM(ScenarioStatus::Disabled, trace.wakeup);
    ASSERT_ENUM(ScenarioStatus::Disabled, trace.night);
    ASSERT_ENUM(ScenarioStatus::Disabled, trace.block);
}

// --- Умный порог -------------------------------------------------------------

static void test_smart_selects_cheapest_hours_of_today() {
    const HourPrice cheap[] = {{3, 0.01f}, {14, 0.02f}, {24 + 5, 0.001f}};
    makeForecast(cheap, 3);
    ChannelConfig config = quietConfig();
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = 2;
    DecisionTrace trace;

    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 3, 10), trace));
    ASSERT_ENUM(ScenarioStatus::Planned, trace.smart);
    TEST_ASSERT_FALSE(trace.smartClamped);
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 14, 45), trace));
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 10, 0), trace));
    ASSERT_ENUM(ScenarioStatus::Planned, trace.smart);
    // Дешевый час завтрашних суток в выбор сегодняшнего дня не входит
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 21, 5, 0), trace));  // а завтра — входит
}

static void test_smart_clamps_to_available_hours() {
    makeForecast(nullptr, 0, 10);  // только 00:00–09:59 сегодня
    ChannelConfig config = quietConfig();
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = 12;
    DecisionTrace trace;
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 5, 0), trace));
    TEST_ASSERT_TRUE(trace.smartClamped);
    TEST_ASSERT_EQUAL_size_t(12, trace.smartRequestedHours);
    TEST_ASSERT_EQUAL_size_t(10, trace.smartAvailableHours);
}

static void test_smart_without_forecast_or_hours() {
    makeForecast(nullptr, 0);
    ChannelConfig config = quietConfig();
    config.smartThresholdEnabled = true;
    DecisionTrace trace;
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 5), trace, kBase, false));
    ASSERT_ENUM(ScenarioStatus::NoForecast, trace.smart);
    // Прогноз только на другие сутки: план не строится
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 23, 5), trace));
    ASSERT_ENUM(ScenarioStatus::NoPlan, trace.smart);
}

// --- Пробуждение -------------------------------------------------------------

static void test_wakeup_window_before_wakeup_time() {
    ChannelConfig config = quietConfig();
    config.wakeupEnabled = true;
    config.wakeupHours = 3;
    config.wakeupTimeMinuteOfDay = 7 * 60;
    DecisionTrace trace;
    // Прогноз не нужен
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 3, 59), trace, kBase, false));
    ASSERT_ENUM(ScenarioStatus::Planned, trace.wakeup);
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 4, 0), trace, kBase, false));
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 6, 59), trace, kBase, false));
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 7, 0), trace, kBase, false));
}

static void test_wakeup_ends_at_start_of_wakeup_hour() {
    // Пробуждение в 07:30: окно обрезается по часу — 04:00–07:00
    ChannelConfig config = quietConfig();
    config.wakeupEnabled = true;
    config.wakeupHours = 3;
    config.wakeupTimeMinuteOfDay = 7 * 60 + 30;
    DecisionTrace trace;
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 4, 0), trace, kBase, false));
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 7, 10), trace, kBase, false));
}

static void test_next_wakeup_epoch() {
    uint32_t wakeup = 0;
    TEST_ASSERT_TRUE(getNextWakeupEpoch(7 * 60, riga(2026, 10, 20, 6, 0), wakeup));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 7, 0), wakeup);
    TEST_ASSERT_TRUE(getNextWakeupEpoch(7 * 60, riga(2026, 10, 20, 7, 0), wakeup));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 21, 7, 0), wakeup);
    // Несуществующее время 03:30 в день перехода на летнее → следующие сутки
    TEST_ASSERT_TRUE(getNextWakeupEpoch(3 * 60 + 30, riga(2026, 3, 29, 1, 0), wakeup));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 3, 30, 3, 30), wakeup);
}

// --- Ночь --------------------------------------------------------------------

static ChannelConfig nightConfig(uint8_t hours) {
    ChannelConfig config = quietConfig();
    config.nightPrepEnabled = true;  // окно по умолчанию 23:00–07:00
    config.nightCheapHours = hours;
    return config;
}

static void test_night_window_key() {
    ChannelConfig config = nightConfig(2);
    const int64_t oct20 = rigatime::daysFromCivil(2026, 10, 20);
    TEST_ASSERT_EQUAL_INT64(oct20, nightWindowKey(riga(2026, 10, 20, 23), config));
    TEST_ASSERT_EQUAL_INT64(oct20, nightWindowKey(riga(2026, 10, 21, 0), config));
    TEST_ASSERT_EQUAL_INT64(oct20, nightWindowKey(riga(2026, 10, 21, 6), config));
    TEST_ASSERT_EQUAL_INT64(-1, nightWindowKey(riga(2026, 10, 21, 7), config));
    TEST_ASSERT_EQUAL_INT64(-1, nightWindowKey(riga(2026, 10, 20, 22), config));
    // Окно внутри суток 01:00–05:00
    config.nightStartMinuteOfDay = 60;
    config.nightEndMinuteOfDay = 300;
    TEST_ASSERT_EQUAL_INT64(oct20, nightWindowKey(riga(2026, 10, 20, 4), config));
    TEST_ASSERT_EQUAL_INT64(-1, nightWindowKey(riga(2026, 10, 20, 5), config));
}

static void test_night_selects_cheapest_hours_of_next_window() {
    const HourPrice cheap[] = {{24 + 1, 0.01f}, {24 + 5, 0.02f}, {24 + 3, 0.03f}, {2, 0.0f}};
    makeForecast(cheap, 4);
    ChannelConfig config = nightConfig(2);
    DecisionTrace trace;
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 23, 30), trace));
    ASSERT_ENUM(ScenarioStatus::Planned, trace.night);
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 21, 1, 30), trace));
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 21, 2, 30), trace));
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 21, 5, 30), trace));
}

// KI-002: выбор пересчитывается из оставшихся часов, реле работает больше N часов.
static void test_bug_ki002_night_takes_extra_hours() {
    const HourPrice cheap[] = {{24 + 1, 0.01f}, {24 + 5, 0.02f}, {24 + 3, 0.03f}};
    makeForecast(cheap, 3);
    ChannelConfig config = nightConfig(2);
    DecisionTrace trace;
    // По всему окну выбраны 01 и 05, но в 03:30 из оставшихся выбираются 05 и 03
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 21, 3, 30), trace));
}

// KI-003: в конце окна кандидатов меньше N — план не строится, реле выключено.
static void test_bug_ki003_night_cut_at_end_of_window() {
    const HourPrice cheap[] = {{24 + 6, 0.005f}, {24 + 1, 0.01f}};
    makeForecast(cheap, 2);
    ChannelConfig config = nightConfig(2);
    DecisionTrace trace;
    // 06:00 — самый дешевый час окна, но к 06:30 остался один кандидат из двух
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 21, 6, 30), trace));
    ASSERT_ENUM(ScenarioStatus::NoPlan, trace.night);
}

// --- Непрерывный блок --------------------------------------------------------

static ChannelConfig blockConfig() {
    ChannelConfig config = quietConfig();
    config.consecutiveBlockEnabled = true;
    config.consecutiveBlockHours = 2;
    return config;
}

static void test_block_locks_cheapest_window() {
    const HourPrice cheap[] = {{13, 0.01f}, {14, 0.01f}};
    makeForecast(cheap, 2);
    ChannelConfig config = blockConfig();
    BlockLock lock = {0, 0};
    DecisionTrace trace;
    TEST_ASSERT_FALSE(decideScenarios(config, kBase, true, forecast, riga(2026, 10, 20, 10), lock, trace));
    ASSERT_ENUM(ScenarioStatus::Planned, trace.block);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 13), lock.start);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 15), lock.end);

    // Блок зафиксирован: новые цены его не двигают
    const HourPrice moved[] = {{18, 0.0f}, {19, 0.0f}};
    makeForecast(moved, 2);
    TEST_ASSERT_TRUE(decideScenarios(config, kBase, true, forecast, riga(2026, 10, 20, 13, 30), lock, trace));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 13), lock.start);
}

// KI-004: после окончания блока сразу выбирается новый от текущего часа.
static void test_bug_ki004_block_restarts_after_end() {
    const HourPrice cheap[] = {{18, 0.0f}, {19, 0.0f}};
    makeForecast(cheap, 2);
    ChannelConfig config = blockConfig();
    BlockLock lock = {riga(2026, 10, 20, 13), riga(2026, 10, 20, 15)};
    DecisionTrace trace;
    decideScenarios(config, kBase, true, forecast, riga(2026, 10, 20, 15, 0), lock, trace);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 18), lock.start);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 20), lock.end);
}

// --- Отпуск ------------------------------------------------------------------

static ChannelConfig holidayConfig() {
    ChannelConfig config = quietConfig();
    config.holidayEnabled = true;
    config.holidayReturnEpoch = riga(2026, 10, 21, 10);
    config.holidayOnHours = 2;
    return config;
}

static void test_holiday_selects_cheapest_hours_before_return() {
    const HourPrice cheap[] = {{24 + 2, 0.01f}, {24 + 4, 0.02f}, {24 + 12, 0.0f}};
    makeForecast(cheap, 3);
    ChannelConfig config = holidayConfig();
    config.alwaysOnBelowPriceEurKwh = 0.5f;  // отпуск важнее порога
    config.smartThresholdEnabled = true;     // и остальных сценариев
    DecisionTrace trace;
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 12), trace, 0.1f));
    ASSERT_ENUM(HolidayStatus::Selected, trace.holiday);
    ASSERT_ENUM(ScenarioStatus::Disabled, trace.smart);
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 21, 2, 15), trace, 0.1f));
    // KI-002: в 03:15 выбор пересчитан из оставшихся часов — 04 и 03, реле включено
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 21, 3, 15), trace, 0.1f));
}

static void test_holiday_without_coverage_is_off() {
    makeForecast(nullptr, 0, 24);  // прогноз кончается раньше возвращения
    ChannelConfig config = holidayConfig();
    DecisionTrace trace;
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 12), trace, -1.0f));
    ASSERT_ENUM(HolidayStatus::NotCovered, trace.holiday);
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 12), trace, -1.0f, false));
    ASSERT_ENUM(HolidayStatus::NoForecast, trace.holiday);
}

// KI-003 для отпуска: в последний час перед возвращением кандидатов меньше N.
static void test_bug_ki003_holiday_off_just_before_return() {
    const HourPrice cheap[] = {{24 + 9, 0.0f}};
    makeForecast(cheap, 1);
    ChannelConfig config = holidayConfig();
    DecisionTrace trace;
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 21, 9, 30), trace));
    ASSERT_ENUM(HolidayStatus::NotCovered, trace.holiday);
}

// --- Комбинация сценариев ----------------------------------------------------

static void test_scenarios_are_combined_with_or() {
    const HourPrice cheap[] = {{3, 0.01f}};
    makeForecast(cheap, 1);
    ChannelConfig config = quietConfig();
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = 1;
    config.wakeupEnabled = true;
    config.wakeupHours = 1;
    config.wakeupTimeMinuteOfDay = 7 * 60;
    config.nightPrepEnabled = true;
    config.nightCheapHours = 1;
    DecisionTrace trace;
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 3, 30), trace));   // умный порог
    TEST_ASSERT_TRUE(decide(config, riga(2026, 10, 20, 6, 30), trace));   // пробуждение
    TEST_ASSERT_FALSE(decide(config, riga(2026, 10, 20, 12, 0), trace));
    ASSERT_ENUM(ScenarioStatus::Planned, trace.smart);
    ASSERT_ENUM(ScenarioStatus::Planned, trace.wakeup);
    ASSERT_ENUM(ScenarioStatus::Planned, trace.night);
    ASSERT_ENUM(ScenarioStatus::Disabled, trace.block);
}

// --- График ------------------------------------------------------------------

static uint32_t todayLocal() {
    return rigatime::toLocal(riga(2026, 10, 20, 12)) / 86400UL;
}

static size_t countOn(const bool* slots) {
    size_t count = 0;
    for (size_t i = 0; i < prices::kMaxSlots; ++i) count += slots[i] ? 1 : 0;
    return count;
}

static void test_chart_empty_forecast_leaves_slots() {
    forecast.clear();
    bool slots[prices::kMaxSlots];
    memset(slots, 1, sizeof(slots));
    BlockLock lock = {0, 0};
    buildChartSlots(quietConfig(), forecast, true, riga(2026, 10, 20, 12), todayLocal(),
        false, false, lock, slots);
    TEST_ASSERT_EQUAL_size_t(prices::kMaxSlots, countOn(slots));
}

static void test_chart_manual_mode() {
    makeForecast(nullptr, 0);
    bool slots[prices::kMaxSlots];
    BlockLock lock = {0, 0};
    buildChartSlots(quietConfig(), forecast, true, riga(2026, 10, 20, 12), todayLocal(),
        true, true, lock, slots);
    TEST_ASSERT_EQUAL_size_t(96, countOn(slots));  // все слоты выбранных суток
    TEST_ASSERT_TRUE(slots[0]);
    TEST_ASSERT_FALSE(slots[96]);
    buildChartSlots(quietConfig(), forecast, true, riga(2026, 10, 20, 12), todayLocal(),
        true, false, lock, slots);
    TEST_ASSERT_EQUAL_size_t(0, countOn(slots));
}

static void test_chart_threshold_and_smart_for_tomorrow() {
    const HourPrice cheap[] = {{3, 0.01f}, {24 + 5, 0.02f}, {24 + 6, 0.03f}};
    makeForecast(cheap, 3);
    ChannelConfig config = quietConfig();
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = 2;
    bool slots[prices::kMaxSlots];
    BlockLock lock = {0, 0};
    // Завтрашний график показывает самые дешевые часы завтрашних суток
    buildChartSlots(config, forecast, true, riga(2026, 10, 20, 12), todayLocal() + 1,
        false, false, lock, slots);
    TEST_ASSERT_EQUAL_size_t(8, countOn(slots));
    TEST_ASSERT_TRUE(slots[(24 + 5) * 4]);
    TEST_ASSERT_TRUE(slots[(24 + 6) * 4 + 3]);
    TEST_ASSERT_FALSE(slots[3 * 4]);

    // Порог отмечает отдельные слоты по их цене
    config.smartThresholdEnabled = false;
    config.alwaysOnBelowPriceEurKwh = 0.015f;
    buildChartSlots(config, forecast, true, riga(2026, 10, 20, 12), todayLocal(),
        false, false, lock, slots);
    TEST_ASSERT_EQUAL_size_t(4, countOn(slots));
    TEST_ASSERT_TRUE(slots[3 * 4]);
}

// KI-005: график снимает слоты по цене слота выше аварийного предела, а решение
// сравнивает с пределом только текущую цену из /current.
static void test_bug_ki005_chart_ceiling_uses_slot_price() {
    float hourPrices[48];
    for (float& p : hourPrices) p = kBase;
    testing::fillHours(forecast, riga(2026, 10, 20, 0), hourPrices, 48);
    ChannelConfig config = quietConfig();
    config.safetyCeilingEurKwh = 0.30f;
    config.wakeupEnabled = true;
    config.wakeupHours = 1;
    config.wakeupTimeMinuteOfDay = 14 * 60;
    // Подменить цену одного квартала 13:15 на высокую: пересобрать ряд
    prices::PriceSeries spiked;
    for (size_t i = 0; i < forecast.size(); ++i) {
        float price = i == 13 * 4 + 1 ? 0.50f : forecast[i].eurKwh;
        spiked.append(forecast[i].start, price);
    }
    bool slots[prices::kMaxSlots];
    BlockLock lock = {0, 0};
    buildChartSlots(config, spiked, true, riga(2026, 10, 20, 12), todayLocal(),
        false, false, lock, slots);
    TEST_ASSERT_TRUE(slots[13 * 4]);
    TEST_ASSERT_FALSE(slots[13 * 4 + 1]);
    TEST_ASSERT_TRUE(slots[13 * 4 + 2]);

    // Решение в 13:15 при текущей цене ниже предела — включено
    DecisionTrace trace;
    BlockLock decisionLock = {0, 0};
    TEST_ASSERT_TRUE(decideScenarios(config, 0.25f, true, spiked, riga(2026, 10, 20, 13, 20),
        decisionLock, trace));
}

static void test_chart_uses_locked_block() {
    makeForecast(nullptr, 0);
    ChannelConfig config = blockConfig();
    BlockLock lock = {riga(2026, 10, 20, 16), riga(2026, 10, 20, 18)};
    bool slots[prices::kMaxSlots];
    buildChartSlots(config, forecast, true, riga(2026, 10, 20, 12), todayLocal(),
        false, false, lock, slots);
    TEST_ASSERT_EQUAL_size_t(8, countOn(slots));
    TEST_ASSERT_TRUE(slots[16 * 4]);
    TEST_ASSERT_TRUE(slots[17 * 4 + 3]);
}

// --- Название режима ---------------------------------------------------------

static void test_mode_name() {
    ChannelConfig config = quietConfig();
    config.safetyCeilingEurKwh = 0.3f;
    config.alwaysOnBelowPriceEurKwh = 0.05f;
    ModeInputs in = {false, true, true, 0.1f, true, riga(2026, 10, 20, 12)};
    TEST_ASSERT_EQUAL_STRING("По цене", modeName(config, in));
    in.latestPriceEurKwh = 0.04f;
    TEST_ASSERT_EQUAL_STRING("Порог включения", modeName(config, in));
    in.latestPriceEurKwh = 0.4f;
    TEST_ASSERT_EQUAL_STRING("Аварийное отключение по верхнему пределу", modeName(config, in));
    in.latestPriceEurKwh = 0.1f;
    config.nightPrepEnabled = true;
    TEST_ASSERT_EQUAL_STRING("Включение ночью", modeName(config, in));
    config.smartThresholdEnabled = true;
    TEST_ASSERT_EQUAL_STRING("Умный порог включения", modeName(config, in));
    config.holidayEnabled = true;
    config.holidayReturnEpoch = riga(2026, 10, 21, 10);
    TEST_ASSERT_EQUAL_STRING("Отпуск", modeName(config, in));
    in.hasLatestPrice = false;
    TEST_ASSERT_EQUAL_STRING("Нет актуальной цены: реле выключено", modeName(config, in));
    in.hadAnyPrice = false;
    TEST_ASSERT_EQUAL_STRING("Ожидание первой цены", modeName(config, in));
    in.manual = true;
    TEST_ASSERT_EQUAL_STRING("Ручной", modeName(config, in));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ceiling_and_holiday_expiry_helpers);
    RUN_TEST(test_threshold_only);
    RUN_TEST(test_smart_selects_cheapest_hours_of_today);
    RUN_TEST(test_smart_clamps_to_available_hours);
    RUN_TEST(test_smart_without_forecast_or_hours);
    RUN_TEST(test_wakeup_window_before_wakeup_time);
    RUN_TEST(test_wakeup_ends_at_start_of_wakeup_hour);
    RUN_TEST(test_next_wakeup_epoch);
    RUN_TEST(test_night_window_key);
    RUN_TEST(test_night_selects_cheapest_hours_of_next_window);
    RUN_TEST(test_bug_ki002_night_takes_extra_hours);
    RUN_TEST(test_bug_ki003_night_cut_at_end_of_window);
    RUN_TEST(test_block_locks_cheapest_window);
    RUN_TEST(test_bug_ki004_block_restarts_after_end);
    RUN_TEST(test_holiday_selects_cheapest_hours_before_return);
    RUN_TEST(test_holiday_without_coverage_is_off);
    RUN_TEST(test_bug_ki003_holiday_off_just_before_return);
    RUN_TEST(test_scenarios_are_combined_with_or);
    RUN_TEST(test_chart_empty_forecast_leaves_slots);
    RUN_TEST(test_chart_manual_mode);
    RUN_TEST(test_chart_threshold_and_smart_for_tomorrow);
    RUN_TEST(test_bug_ki005_chart_ceiling_uses_slot_price);
    RUN_TEST(test_chart_uses_locked_block);
    RUN_TEST(test_mode_name);
    return UNITY_END();
}
