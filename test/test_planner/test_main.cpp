// Тесты lib/planner (шаг 2b): единый план по 15-минутным интервалам, фиксация выбора
// по всему окну, смена настроек посреди окна, сдвиг к концу окна, окно отпуска 10 ч.
// Решения пользователя — docs/DECISIONS.md D-012…D-019, поведение — docs/SCENARIOS.md.
// Тест test_bug_ki004_* фиксирует баг непрерывного блока, который уйдет на шаге 2c.
//
// Опорные сутки: вторник 20.10.2026, летнее время (UTC+3). Прогноз — 3 суток
// с местной полуночи 19.10 до 23:45 21.10 (288 интервалов) по цене kBase.
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

static const float kBase = 0.20f;

static PriceSeries forecast;
static float slotPrices[kMaxSlots];
static uint32_t seriesStart;
static size_t seriesCount;

// Ряд из count интервалов от start (местное время через riga()) по kBase.
static void resetPrices(uint32_t start = riga(2026, 10, 19, 0), size_t count = 288) {
    seriesStart = start;
    seriesCount = count;
    for (size_t i = 0; i < kMaxSlots; ++i) slotPrices[i] = kBase;
}

// Цена price для quarters интервалов подряд, начиная с момента at.
static void setPrice(uint32_t at, float price, size_t quarters = 1) {
    size_t first = (at - seriesStart) / kSlotSeconds;
    for (size_t i = first; i < first + quarters && i < kMaxSlots; ++i) slotPrices[i] = price;
}

static float priceAt(uint32_t at) {
    return slotPrices[(at - seriesStart) / kSlotSeconds];
}

// Собрать прогноз из первых count интервалов (по умолчанию — весь ряд).
static void commit(size_t count = 0) {
    forecast.clear();
    size_t total = count == 0 ? seriesCount : count;
    for (size_t i = 0; i < total; ++i) {
        forecast.append(seriesStart + static_cast<uint32_t>(i) * kSlotSeconds, slotPrices[i]);
    }
}

// Канал со своим состоянием планировщика; fresh() — как после загрузки.
static PlannerState state;
static BlockLock lock;
static ChannelPlan plan;

static void fresh() {
    memset(&state, 0, sizeof(state));
    lock = {0, 0};
}

static Decision decideAt(const ChannelConfig& config, uint32_t now, float currentPrice = kBase) {
    buildPlan(config, forecast, now, state, lock, plan);
    return decideNow(config, plan, forecast, now, currentPrice);
}

static bool planOnAt(uint32_t slotStart) {
    int index = forecast.indexAt(slotStart);
    TEST_ASSERT_TRUE(index >= 0);
    return slotOn(plan.mask[index]);
}

static uint8_t maskAt(uint32_t slotStart) {
    int index = forecast.indexAt(slotStart);
    TEST_ASSERT_TRUE(index >= 0);
    return plan.mask[index];
}

// Прожить интервалы [from, to): решение через минуту после начала каждого интервала
// по его цене. Возвращает, сколько интервалов реле было включено.
static size_t simulate(const ChannelConfig& config, uint32_t from, uint32_t to) {
    size_t on = 0;
    for (uint32_t t = from; t < to; t += kSlotSeconds) {
        if (decideAt(config, t + 60, priceAt(t)).on) ++on;
    }
    return on;
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

static void test_threshold_works_without_forecast() {
    forecast.clear();
    fresh();
    ChannelConfig config = quietConfig();
    config.alwaysOnBelowPriceEurKwh = 0.05f;
    Decision decision = decideAt(config, riga(2026, 10, 20, 12), 0.05f);
    TEST_ASSERT_TRUE(decision.on);
    ASSERT_ENUM(Reason::Threshold, decision.reason);
    decision = decideAt(config, riga(2026, 10, 20, 12), 0.0501f);
    TEST_ASSERT_FALSE(decision.on);
    ASSERT_ENUM(Reason::None, decision.reason);
    ASSERT_ENUM(HolidayStatus::Inactive, plan.trace.holiday);
    ASSERT_ENUM(ScenarioStatus::Disabled, plan.trace.smart);
    ASSERT_ENUM(ScenarioStatus::Disabled, plan.trace.wakeup);
    ASSERT_ENUM(ScenarioStatus::Disabled, plan.trace.night);
    ASSERT_ENUM(ScenarioStatus::Disabled, plan.trace.block);
}

static void test_ceiling_uses_current_price() {
    resetPrices();
    commit();
    fresh();
    ChannelConfig config = quietConfig();
    config.safetyCeilingEurKwh = 0.30f;
    config.alwaysOnBelowPriceEurKwh = 0.25f;
    Decision decision = decideAt(config, riga(2026, 10, 20, 12), 0.31f);
    TEST_ASSERT_FALSE(decision.on);
    ASSERT_ENUM(Reason::Ceiling, decision.reason);
}

static void test_slot_mask_helpers() {
    TEST_ASSERT_FALSE(slotOn(0));
    TEST_ASSERT_TRUE(slotOn(slot::kNight));
    TEST_ASSERT_FALSE(slotOn(slot::kNight | slot::kCeiling));
    TEST_ASSERT_FALSE(slotOn(slot::kHolidayPeriod | slot::kThreshold));
    TEST_ASSERT_TRUE(slotOn(slot::kHolidayPeriod | slot::kHoliday | slot::kThreshold));
    ASSERT_ENUM(Reason::None, slotReason(0));
    ASSERT_ENUM(Reason::Threshold, slotReason(slot::kThreshold | slot::kSmart));
    ASSERT_ENUM(Reason::Ceiling, slotReason(slot::kSmart | slot::kCeiling));
    ASSERT_ENUM(Reason::HolidayOff, slotReason(slot::kHolidayPeriod | slot::kSmart));
    ASSERT_ENUM(Reason::Holiday, slotReason(slot::kHolidayPeriod | slot::kHoliday));
    TEST_ASSERT_TRUE(strlen(reasonName(Reason::Night)) > 0);
}

// --- Умный порог -------------------------------------------------------------

static ChannelConfig smartConfig(uint8_t hours) {
    ChannelConfig config = quietConfig();
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = hours;
    return config;
}

// D-014: выбираются отдельные 15-минутные интервалы, а не часы по средней цене.
static void test_smart_selects_cheapest_quarters() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 3, 0), 0.01f);
    setPrice(riga(2026, 10, 20, 3, 15), 0.30f, 3);  // среднее часа 03 выше kBase
    setPrice(riga(2026, 10, 20, 9, 15), 0.03f);
    setPrice(riga(2026, 10, 20, 14, 30), 0.02f);
    setPrice(riga(2026, 10, 20, 22, 45), 0.04f);
    setPrice(riga(2026, 10, 21, 5, 0), 0.001f);     // завтра: в сегодняшний выбор не входит
    commit();
    fresh();
    ChannelConfig config = smartConfig(1);

    Decision decision = decideAt(config, riga(2026, 10, 20, 3, 5), 0.01f);
    TEST_ASSERT_TRUE(decision.on);
    ASSERT_ENUM(Reason::Smart, decision.reason);
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.smart);
    TEST_ASSERT_FALSE(plan.trace.smartClamped);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 9, 15)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 14, 30)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 22, 45)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 3, 15)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 0, 0)));
    // Завтрашние сутки на графике — свой выбор по всему окну
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 5, 0)));

    fresh();
    TEST_ASSERT_EQUAL_size_t(4, simulate(config, riga(2026, 10, 20, 0), riga(2026, 10, 21, 0)));
}

// D-016: умный порог не сдвигается; при равной цене — более ранний интервал.
static void test_smart_ties_prefer_earlier_without_shift() {
    resetPrices();
    commit();
    fresh();
    decideAt(smartConfig(1), riga(2026, 10, 20, 12));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 0, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 0, 45)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 1, 0)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 23, 45)));
}

static void test_smart_clamps_on_23_hour_day() {
    // 29.03.2026 — переход на летнее время, в сутках 92 интервала
    resetPrices(riga(2026, 3, 28, 0), 96 + 92 + 96);
    commit();
    fresh();
    decideAt(smartConfig(24), riga(2026, 3, 29, 12));
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.smart);
    TEST_ASSERT_TRUE(plan.trace.smartClamped);
    TEST_ASSERT_EQUAL_UINT16(96, plan.trace.smartRequested);
    TEST_ASSERT_EQUAL_UINT16(92, plan.trace.smartAvailable);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 3, 29, 0, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 3, 29, 23, 45)));
}

// D-012: пока прогноз не покрывает сутки, выбора нет; когда покроет — выбор по
// всему окну, прошедшие дешевые интервалы засчитываются.
static void test_smart_waits_for_whole_day_then_uses_whole_window() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 3, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 20, 18, 0), 0.05f, 4);
    commit(96 + 48);  // до 20.10 12:00
    fresh();
    ChannelConfig config = smartConfig(1);
    Decision decision = decideAt(config, riga(2026, 10, 20, 3, 5), 0.01f);
    TEST_ASSERT_FALSE(decision.on);
    ASSERT_ENUM(ScenarioStatus::Waiting, plan.trace.smart);

    commit();
    decideAt(config, riga(2026, 10, 20, 6, 0));
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.smart);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 3, 0)));
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 20, 18, 10), 0.05f).on);
}

// D-012: после перезагрузки выбор тот же, что был бы без нее.
static void test_smart_reboot_keeps_whole_day_selection() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 3, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 20, 18, 0), 0.05f, 4);
    commit();
    fresh();
    ChannelConfig config = smartConfig(1);
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 20, 18, 10), 0.05f).on);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 3, 0)));
}

// D-013: смена N посреди суток засчитывает прошедшие выбранные интервалы.
static void test_smart_hours_changed_mid_day() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 3, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 20, 5, 0), 0.02f, 4);
    setPrice(riga(2026, 10, 20, 18, 0), 0.05f, 4);
    setPrice(riga(2026, 10, 20, 20, 0), 0.06f, 4);
    commit();
    fresh();
    decideAt(smartConfig(1), riga(2026, 10, 20, 2, 0));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 3, 0)));

    // 1 → 2 часа в 15:00: 03:00 засчитаны, еще 4 интервала — из оставшихся
    decideAt(smartConfig(2), riga(2026, 10, 20, 15, 0));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 3, 0)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 5, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 18, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 18, 45)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 20, 0)));

    // 2 → 1 час в 16:00: уже отработан 1 час — больше не включается
    TEST_ASSERT_FALSE(decideAt(smartConfig(1), riga(2026, 10, 20, 16, 0)).on);
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 18, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 3, 0)));
}

// D-013: сценарий, включенный посреди суток, берет N из оставшихся интервалов.
static void test_smart_enabled_mid_day_takes_remaining() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 3, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 20, 18, 0), 0.05f, 4);
    commit();
    fresh();
    decideAt(quietConfig(), riga(2026, 10, 20, 1, 0));
    decideAt(smartConfig(1), riga(2026, 10, 20, 15, 0));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 3, 0)));
    TEST_ASSERT_TRUE(decideAt(smartConfig(1), riga(2026, 10, 20, 18, 10), 0.05f).on);
}

// --- Ночь --------------------------------------------------------------------

static ChannelConfig nightConfig(uint8_t hours) {
    ChannelConfig config = quietConfig();
    config.nightPrepEnabled = true;  // окно по умолчанию 23:00–07:00
    config.nightCheapHours = hours;
    return config;
}

static void test_night_windows() {
    ChannelConfig config = nightConfig(2);
    Window window;
    TEST_ASSERT_TRUE(activeNightWindow(config, riga(2026, 10, 21, 3), window));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 23), window.start);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 21, 7), window.end);
    // Вне окна — ближайшее следующее
    TEST_ASSERT_TRUE(activeNightWindow(config, riga(2026, 10, 21, 7), window));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 21, 23), window.start);
    // Окно внутри суток 01:00–05:00
    config.nightStartMinuteOfDay = 60;
    config.nightEndMinuteOfDay = 300;
    TEST_ASSERT_TRUE(activeNightWindow(config, riga(2026, 10, 21, 4, 59), window));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 21, 1), window.start);
    TEST_ASSERT_TRUE(activeNightWindow(config, riga(2026, 10, 21, 5), window));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 22, 1), window.start);
    config.nightEndMinuteOfDay = 60;
    TEST_ASSERT_FALSE(activeNightWindow(config, riga(2026, 10, 21, 5), window));
}

// KI-002 исправлен: за окно реле работает ровно N часов, выбор не «плывет».
static void test_night_takes_exactly_n_hours() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 1, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 21, 3, 0), 0.05f, 4);
    setPrice(riga(2026, 10, 21, 5, 0), 0.03f, 4);
    commit();
    fresh();
    ChannelConfig config = nightConfig(1);
    TEST_ASSERT_EQUAL_size_t(4, simulate(config, riga(2026, 10, 20, 22), riga(2026, 10, 21, 8)));
    TEST_ASSERT_TRUE(decideAt(config, riga(2026, 10, 21, 1, 30), 0.01f).on);
    ASSERT_ENUM(Reason::Night, decideAt(config, riga(2026, 10, 21, 1, 30), 0.01f).reason);
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 21, 5, 30), 0.03f).on);
}

// KI-003 исправлен: выбранные интервалы в конце окна отрабатываются.
static void test_night_end_of_window_stays_on() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 6, 0), 0.005f, 4);
    setPrice(riga(2026, 10, 21, 1, 0), 0.01f, 4);
    commit();
    fresh();
    ChannelConfig config = nightConfig(2);
    TEST_ASSERT_EQUAL_size_t(8, simulate(config, riga(2026, 10, 20, 22), riga(2026, 10, 21, 8)));
    TEST_ASSERT_TRUE(decideAt(config, riga(2026, 10, 21, 6, 50), 0.005f).on);
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.night);
}

// D-016: интервал переносится к концу окна, если там дороже не больше чем на 1 цент.
static void test_night_shifts_toward_end_within_one_cent() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 2, 0), 0.050f, 4);
    setPrice(riga(2026, 10, 21, 6, 0), 0.058f, 4);
    commit();
    fresh();
    decideAt(nightConfig(1), riga(2026, 10, 20, 22));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 6, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 6, 45)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 21, 2, 0)));

    setPrice(riga(2026, 10, 21, 6, 0), 0.065f, 4);  // дороже на 1,5 цента — не переносится
    commit();
    fresh();
    decideAt(nightConfig(1), riga(2026, 10, 20, 22));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 2, 0)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 21, 6, 0)));
}

// Разница считается от исходной цены: цепочка переносов не накапливает больше цента.
static void test_shift_does_not_chain() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 1, 0), 0.050f);
    setPrice(riga(2026, 10, 21, 3, 0), 0.058f);
    setPrice(riga(2026, 10, 21, 5, 0), 0.066f);
    commit();
    bool chosen[kMaxSlots] = {};
    size_t available = 0;
    const Window window = {riga(2026, 10, 20, 23), riga(2026, 10, 21, 7)};
    TEST_ASSERT_EQUAL_size_t(1, selectCheapest(forecast, window, window.start, 1, true,
        chosen, available));
    TEST_ASSERT_EQUAL_size_t(32, available);
    TEST_ASSERT_TRUE(chosen[forecast.indexAt(riga(2026, 10, 21, 3, 0))]);
    TEST_ASSERT_FALSE(chosen[forecast.indexAt(riga(2026, 10, 21, 5, 0))]);
    TEST_ASSERT_FALSE(chosen[forecast.indexAt(riga(2026, 10, 21, 1, 0))]);
}

// При равных ценах ночь уходит к концу окна; в окно входят только целые интервалы.
static void test_night_quarter_boundaries_and_ties() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 23, 0), 0.001f);   // 23:00–23:15 не целиком в окне
    setPrice(riga(2026, 10, 21, 6, 45), 0.001f);   // 06:45–07:00 выходит за 06:50
    commit();
    fresh();
    ChannelConfig config = nightConfig(1);
    config.nightStartMinuteOfDay = 23 * 60 + 10;
    config.nightEndMinuteOfDay = 6 * 60 + 50;
    decideAt(config, riga(2026, 10, 20, 22));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 23, 0)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 21, 6, 45)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 6, 30)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 5, 45)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 21, 5, 30)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 23, 15)));
}

static void test_night_autumn_dst_has_nine_hours() {
    // Ночь 24→25.10.2026: час 03:00 повторяется, в окне 23:00–07:00 — 9 часов
    resetPrices(riga(2026, 10, 24, 0), 96 + 100 + 96);
    commit();
    fresh();
    ChannelConfig config = nightConfig(8);
    Window window;
    TEST_ASSERT_TRUE(activeNightWindow(config, riga(2026, 10, 24, 22), window));
    TEST_ASSERT_EQUAL_UINT32(9 * 3600, window.end - window.start);
    config.nightCheapHours = 9;
    decideAt(config, riga(2026, 10, 24, 22));
    TEST_ASSERT_FALSE(plan.trace.nightClamped);
    TEST_ASSERT_EQUAL_UINT16(36, plan.trace.nightAvailable);
    fresh();
    TEST_ASSERT_EQUAL_size_t(36,
        simulate(config, riga(2026, 10, 24, 22), riga(2026, 10, 25, 8)));
}

static void test_night_spring_dst_has_seven_hours() {
    // Ночь 28→29.03.2026: часа 03:00 нет, в окне 7 часов, N = 8 — берутся все
    resetPrices(riga(2026, 3, 28, 0), 96 + 92 + 96);
    commit();
    fresh();
    decideAt(nightConfig(8), riga(2026, 3, 28, 22));
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.night);
    TEST_ASSERT_TRUE(plan.trace.nightClamped);
    TEST_ASSERT_EQUAL_UINT16(32, plan.trace.nightRequested);
    TEST_ASSERT_EQUAL_UINT16(28, plan.trace.nightAvailable);
}

// D-012: перезагрузка после полуночи восстанавливает выбор по всему окну —
// вчерашняя часть окна есть в прогнозе.
static void test_night_reboot_after_midnight_restores_plan() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 23, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 21, 4, 0), 0.05f, 4);
    commit();
    fresh();
    ChannelConfig config = nightConfig(1);
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 21, 4, 10), 0.05f).on);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 23, 0)));
}

// D-012: пока нет цен на завтра, ночное окно не планируется.
static void test_night_waits_for_tomorrow_prices() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 23, 0), 0.01f, 4);
    commit(192);  // 19.10–20.10
    fresh();
    ChannelConfig config = nightConfig(1);
    decideAt(config, riga(2026, 10, 20, 18));
    ASSERT_ENUM(ScenarioStatus::Waiting, plan.trace.night);
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 23, 0)));
    commit();
    decideAt(config, riga(2026, 10, 20, 19));
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.night);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 23, 0)));
}

// D-013: окно поменяли посреди ночи — засчитывается отработанное внутри нового окна.
static void test_night_window_changed_mid_window() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 1, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 21, 5, 0), 0.03f, 4);
    commit();
    fresh();
    ChannelConfig config = nightConfig(1);
    decideAt(config, riga(2026, 10, 20, 22));
    config.nightStartMinuteOfDay = 60;  // 01:00–07:00
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 21, 5, 10), 0.03f).on);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 1, 0)));
    // А при N = 2 добирается еще час из оставшихся
    config.nightCheapHours = 2;
    TEST_ASSERT_TRUE(decideAt(config, riga(2026, 10, 21, 5, 10), 0.03f).on);
}

// --- Отпуск ------------------------------------------------------------------

static ChannelConfig holidayConfig(uint8_t hours = 2) {
    ChannelConfig config = quietConfig();
    config.holidayEnabled = true;
    config.holidayReturnEpoch = riga(2026, 10, 21, 10);  // окно 21.10 00:00–10:00
    config.holidayOnHours = hours;
    return config;
}

// D-015: выбор только из последних 10 часов; до окна и в невыбранных интервалах
// реле выключено, даже если сработал бы порог.
static void test_holiday_uses_last_ten_hours() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 22, 0), 0.0f, 4);
    setPrice(riga(2026, 10, 21, 2, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 21, 6, 0), 0.05f, 4);
    commit();
    fresh();
    ChannelConfig config = holidayConfig();
    config.alwaysOnBelowPriceEurKwh = 0.03f;  // отпуск важнее порога
    config.smartThresholdEnabled = true;     // и остальных сценариев

    Decision decision = decideAt(config, riga(2026, 10, 20, 22, 10), 0.0f);
    TEST_ASSERT_FALSE(decision.on);
    ASSERT_ENUM(Reason::HolidayOff, decision.reason);
    ASSERT_ENUM(HolidayStatus::BeforeWindow, plan.trace.holiday);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 2, 0)));  // выбор уже виден на графике

    decision = decideAt(config, riga(2026, 10, 21, 2, 10), 0.01f);
    TEST_ASSERT_TRUE(decision.on);
    ASSERT_ENUM(Reason::Holiday, decision.reason);
    ASSERT_ENUM(HolidayStatus::Selected, plan.trace.holiday);
    TEST_ASSERT_TRUE(decideAt(config, riga(2026, 10, 21, 6, 10), 0.05f).on);
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 21, 4, 10)).on);
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 22, 0)));
}

static void test_holiday_shifts_toward_return() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 2, 0), 0.010f, 4);
    setPrice(riga(2026, 10, 21, 9, 0), 0.018f, 4);
    commit();
    fresh();
    decideAt(holidayConfig(1), riga(2026, 10, 20, 20));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 9, 0)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 21, 2, 0)));
}

// KI-003 исправлен: последний интервал перед возвращением отрабатывается.
static void test_holiday_last_interval_before_return_stays_on() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 9, 0), 0.0f, 4);
    commit();
    fresh();
    ChannelConfig config = holidayConfig(1);
    TEST_ASSERT_TRUE(decideAt(config, riga(2026, 10, 21, 9, 50), 0.0f).on);
}

static void test_holiday_clamps_to_window() {
    resetPrices();
    commit();
    fresh();
    decideAt(holidayConfig(12), riga(2026, 10, 21, 1));
    TEST_ASSERT_TRUE(plan.trace.holidayClamped);
    TEST_ASSERT_EQUAL_UINT16(48, plan.trace.holidayRequested);
    TEST_ASSERT_EQUAL_UINT16(40, plan.trace.holidayAvailable);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 0, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 9, 45)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 23, 45)));
}

static void test_holiday_waits_for_coverage() {
    resetPrices();
    commit(96 + 96 + 24);  // до 21.10 06:00, а возвращение в 10:00
    fresh();
    Decision decision = decideAt(holidayConfig(), riga(2026, 10, 21, 1), -1.0f);
    TEST_ASSERT_FALSE(decision.on);
    ASSERT_ENUM(HolidayStatus::Waiting, plan.trace.holiday);
}

// После возвращения на графике снова действуют обычные сценарии.
static void test_holiday_period_ends_at_return() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 9, 45), 0.0f);
    setPrice(riga(2026, 10, 21, 10, 0), 0.0f);
    commit();
    fresh();
    ChannelConfig config = holidayConfig(1);
    config.alwaysOnBelowPriceEurKwh = 0.01f;
    decideAt(config, riga(2026, 10, 20, 12));
    TEST_ASSERT_NOT_EQUAL(0, maskAt(riga(2026, 10, 21, 9, 45)) & slot::kHolidayPeriod);
    TEST_ASSERT_EQUAL(0, maskAt(riga(2026, 10, 21, 10, 0)) & slot::kHolidayPeriod);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 10, 0)));
}

// D-013: отпуск включили посреди окна — N из оставшихся интервалов.
static void test_holiday_enabled_mid_window() {
    resetPrices();
    setPrice(riga(2026, 10, 21, 2, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 21, 7, 0), 0.05f, 4);
    commit();
    fresh();
    decideAt(quietConfig(), riga(2026, 10, 21, 1));
    TEST_ASSERT_FALSE(decideAt(holidayConfig(1), riga(2026, 10, 21, 5)).on);
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 21, 2, 0)));
    TEST_ASSERT_TRUE(decideAt(holidayConfig(1), riga(2026, 10, 21, 7, 10), 0.05f).on);
}

// --- Пробуждение -------------------------------------------------------------

// D-017: окно [T − N ч, T) с точностью до минуты.
static void test_wakeup_exact_window() {
    forecast.clear();
    fresh();
    ChannelConfig config = quietConfig();
    config.wakeupEnabled = true;
    config.wakeupHours = 3;
    config.wakeupTimeMinuteOfDay = 7 * 60 + 30;
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 20, 4, 29)).on);
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.wakeup);
    Decision decision = decideAt(config, riga(2026, 10, 20, 4, 30));
    TEST_ASSERT_TRUE(decision.on);
    ASSERT_ENUM(Reason::Wakeup, decision.reason);
    TEST_ASSERT_TRUE(decideAt(config, riga(2026, 10, 20, 7, 29)).on);
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 20, 7, 30)).on);
}

static void test_wakeup_on_chart() {
    resetPrices();
    commit();
    fresh();
    ChannelConfig config = quietConfig();
    config.wakeupEnabled = true;
    config.wakeupHours = 3;
    config.wakeupTimeMinuteOfDay = 7 * 60 + 10;
    decideAt(config, riga(2026, 10, 20, 12));
    // Окно 04:10–07:10 задевает интервалы 04:00 и 07:00 частично — они закрашены
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 3, 45)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 4, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 7, 0)));
    TEST_ASSERT_FALSE(planOnAt(riga(2026, 10, 20, 7, 15)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 5, 0)));   // и в другие сутки
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

// --- Непрерывный блок (до шага 2c) -------------------------------------------

static ChannelConfig blockConfig() {
    ChannelConfig config = quietConfig();
    config.unusedBlockEnabled = true;
    config.forceOnHours = 2;
    return config;
}

static void test_block_locks_cheapest_window() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 13, 0), 0.01f, 8);
    commit();
    fresh();
    ChannelConfig config = blockConfig();
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 20, 10)).on);
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.block);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 13), lock.start);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 15), lock.end);
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 14, 45)));

    // Блок зафиксирован: новые цены его не двигают
    resetPrices();
    setPrice(riga(2026, 10, 20, 18, 0), 0.0f, 8);
    commit();
    Decision decision = decideAt(config, riga(2026, 10, 20, 13, 30));
    TEST_ASSERT_TRUE(decision.on);
    ASSERT_ENUM(Reason::Block, decision.reason);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 13), lock.start);
}

// KI-004: после окончания блока сразу выбирается новый от текущего часа.
static void test_bug_ki004_block_restarts_after_end() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 18, 0), 0.0f, 8);
    commit();
    fresh();
    lock = {riga(2026, 10, 20, 13), riga(2026, 10, 20, 15)};
    decideAt(blockConfig(), riga(2026, 10, 20, 15, 0));
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 18), lock.start);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 20, 20), lock.end);
}

// --- Единый план -------------------------------------------------------------

// KI-005 исправлен: решение и график берутся из одного плана. Для каждого интервала
// суток решение по его цене совпадает с закраской на графике.
static void test_plan_matches_decision() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 2, 0), 0.02f, 3);
    setPrice(riga(2026, 10, 20, 13, 15), 0.50f);   // выше аварийного предела
    setPrice(riga(2026, 10, 20, 13, 30), 0.01f);   // ниже порога
    setPrice(riga(2026, 10, 20, 23, 30), 0.03f, 2);
    setPrice(riga(2026, 10, 21, 4, 0), 0.025f, 4);
    commit();
    fresh();
    ChannelConfig config = quietConfig();
    config.safetyCeilingEurKwh = 0.30f;
    config.alwaysOnBelowPriceEurKwh = 0.015f;
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = 1;
    config.nightPrepEnabled = true;
    config.nightCheapHours = 1;
    config.wakeupEnabled = true;
    config.wakeupHours = 1;
    config.wakeupTimeMinuteOfDay = 14 * 60;
    for (uint32_t t = riga(2026, 10, 20, 0); t < riga(2026, 10, 21, 8); t += kSlotSeconds) {
        Decision decision = decideAt(config, t + 60, priceAt(t));
        TEST_ASSERT_EQUAL_MESSAGE(decision.on, planOnAt(t), "решение и график разошлись");
    }
}

static void test_plan_covers_yesterday_and_tomorrow() {
    resetPrices();
    setPrice(riga(2026, 10, 19, 10, 0), 0.01f, 4);
    setPrice(riga(2026, 10, 21, 15, 0), 0.01f, 4);
    commit();
    fresh();
    decideAt(smartConfig(1), riga(2026, 10, 20, 12));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 19, 10, 0)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 21, 15, 45)));
    TEST_ASSERT_TRUE(planOnAt(riga(2026, 10, 20, 0, 0)));  // сегодня — равные цены
}

static void test_scenarios_are_combined_with_or() {
    resetPrices();
    setPrice(riga(2026, 10, 20, 3, 0), 0.01f, 4);
    commit();
    fresh();
    ChannelConfig config = quietConfig();
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = 1;
    config.wakeupEnabled = true;
    config.wakeupHours = 1;
    config.wakeupTimeMinuteOfDay = 7 * 60;
    config.nightPrepEnabled = true;
    config.nightCheapHours = 1;
    TEST_ASSERT_TRUE(decideAt(config, riga(2026, 10, 20, 3, 30), 0.01f).on);  // умный порог
    TEST_ASSERT_TRUE(decideAt(config, riga(2026, 10, 20, 6, 30)).on);         // пробуждение
    TEST_ASSERT_FALSE(decideAt(config, riga(2026, 10, 20, 12, 0)).on);
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.smart);
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.wakeup);
    ASSERT_ENUM(ScenarioStatus::Planned, plan.trace.night);
    ASSERT_ENUM(ScenarioStatus::Disabled, plan.trace.block);
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
    RUN_TEST(test_threshold_works_without_forecast);
    RUN_TEST(test_ceiling_uses_current_price);
    RUN_TEST(test_slot_mask_helpers);
    RUN_TEST(test_smart_selects_cheapest_quarters);
    RUN_TEST(test_smart_ties_prefer_earlier_without_shift);
    RUN_TEST(test_smart_clamps_on_23_hour_day);
    RUN_TEST(test_smart_waits_for_whole_day_then_uses_whole_window);
    RUN_TEST(test_smart_reboot_keeps_whole_day_selection);
    RUN_TEST(test_smart_hours_changed_mid_day);
    RUN_TEST(test_smart_enabled_mid_day_takes_remaining);
    RUN_TEST(test_night_windows);
    RUN_TEST(test_night_takes_exactly_n_hours);
    RUN_TEST(test_night_end_of_window_stays_on);
    RUN_TEST(test_night_shifts_toward_end_within_one_cent);
    RUN_TEST(test_shift_does_not_chain);
    RUN_TEST(test_night_quarter_boundaries_and_ties);
    RUN_TEST(test_night_autumn_dst_has_nine_hours);
    RUN_TEST(test_night_spring_dst_has_seven_hours);
    RUN_TEST(test_night_reboot_after_midnight_restores_plan);
    RUN_TEST(test_night_waits_for_tomorrow_prices);
    RUN_TEST(test_night_window_changed_mid_window);
    RUN_TEST(test_holiday_uses_last_ten_hours);
    RUN_TEST(test_holiday_shifts_toward_return);
    RUN_TEST(test_holiday_last_interval_before_return_stays_on);
    RUN_TEST(test_holiday_clamps_to_window);
    RUN_TEST(test_holiday_waits_for_coverage);
    RUN_TEST(test_holiday_period_ends_at_return);
    RUN_TEST(test_holiday_enabled_mid_window);
    RUN_TEST(test_wakeup_exact_window);
    RUN_TEST(test_wakeup_on_chart);
    RUN_TEST(test_next_wakeup_epoch);
    RUN_TEST(test_block_locks_cheapest_window);
    RUN_TEST(test_bug_ki004_block_restarts_after_end);
    RUN_TEST(test_plan_matches_decision);
    RUN_TEST(test_plan_covers_yesterday_and_tomorrow);
    RUN_TEST(test_scenarios_are_combined_with_or);
    RUN_TEST(test_mode_name);
    return UNITY_END();
}
