// Тесты lib/controller: расписание запроса цены, failsafe, ручной режим, таймер
// принудительного включения, сохранение настроек (docs/SCENARIOS.md §1, §5–§7).
// Порты подменены фейками: часы и millis() двигает тест, выходы — массив, цены
// отдает фейковый асинхронный источник (результат — при следующем poll, или
// запрос «висит», пока тест не отпустит), лог копится в строку.
#include <unity.h>

#include <stdio.h>
#include <string>

#include <ChannelConfig.h>
#include <ConfigStore.h>
#include <Controller.h>
#include <SwitchHistory.h>

#include "../helpers/FakeKv.h"
#include "../helpers/TestHelpers.h"

using channelconfig::ChannelConfig;
using channelconfig::kChannelCount;
using controller::kPriceFailsafeTimeoutMs;
using controller::kPriceRetryIntervalMs;
using history::Cause;
using testing::riga;

namespace {

const uint32_t kHourMs = 3600UL * 1000UL;

// Как SystemClock: до синхронизации nowUtc() — секунды от запуска.
class FakeClock : public controller::Clock {
public:
    uint32_t ms = 0;

    void sync(uint32_t epoch) {
        synced_ = true;
        epoch_ = epoch;
        syncedAtMs_ = ms;
    }
    void advanceMs(uint32_t delta) { ms += delta; }
    // Перевести часы на момент epoch (вперед), millis() идет вместе с ними.
    void advanceTo(uint32_t epoch) { ms += (epoch - nowUtc()) * 1000UL; }

    bool synced() const override { return synced_; }
    uint32_t nowUtc() const override { return epoch_ + (ms - syncedAtMs_) / 1000UL; }
    uint32_t uptimeMs() const override { return ms; }

private:
    bool synced_ = false;
    uint32_t epoch_ = 0;
    uint32_t syncedAtMs_ = 0;
};

class FakeOutputs : public controller::Outputs {
public:
    bool states[kChannelCount] = {};
    int setCalls = 0;

    void set(uint8_t channel, bool on) override {
        ++setCalls;
        if (channel < kChannelCount) states[channel] = on;
    }
    bool get(uint8_t channel) const override {
        return channel < kChannelCount && states[channel];
    }
};

// Асинхронный источник: запрос завершается на первом poll(), если тест не держит
// его (hold). Результат вычисляется в момент завершения.
class FakePrices : public controller::PriceSource {
public:
    explicit FakePrices(const FakeClock& clock) : clock_(clock) {}

    bool currentOk = true;
    float price = 0.05f;
    // 0 — начало текущего 15-минутного интервала по часам в момент результата
    // (до синхронизации — 1000), иначе — это значение.
    long timestamp = 0;
    int currentCalls = 0;

    bool forecastOk = true;
    prices::PriceSeries series;
    int forecastCalls = 0;

    // true — запрос идет, poll() отвечает Busy.
    bool hold = false;

    bool busy() const { return pending_ != Pending::None; }

    bool startCurrent() override {
        if (busy()) return false;
        ++currentCalls;
        pending_ = Pending::Current;
        return true;
    }
    bool startPrices(const char*, prices::PriceSeries& out) override {
        if (busy()) return false;
        ++forecastCalls;
        pending_ = Pending::Prices;
        out_ = &out;
        return true;
    }
    controller::FetchStatus poll(float& eurKwh, long& ts) override {
        using controller::FetchStatus;
        if (!busy()) return FetchStatus::Idle;
        if (hold) return FetchStatus::Busy;
        const Pending kind = pending_;
        pending_ = Pending::None;
        if (kind == Pending::Current) {
            if (!currentOk) return FetchStatus::Failed;
            eurKwh = price;
            ts = timestamp != 0 ? timestamp : currentInterval();
            return FetchStatus::Done;
        }
        if (!forecastOk) return FetchStatus::Failed;
        *out_ = series;
        return FetchStatus::Done;
    }
    long currentInterval() const {
        if (!clock_.synced()) return 1000;
        return static_cast<long>(clock_.nowUtc() / controller::kPriceIntervalSeconds *
            controller::kPriceIntervalSeconds);
    }

private:
    enum class Pending { None, Current, Prices };
    const FakeClock& clock_;
    Pending pending_ = Pending::None;
    prices::PriceSeries* out_ = nullptr;
};

class FakeLog : public controller::LogSink {
public:
    std::string text;

    void printText(const char* value) override { text += value; }
    void printSigned(long value) override { text += std::to_string(value); }
    void printUnsigned(unsigned long value) override { text += std::to_string(value); }
    void printFloat(float value, uint8_t digits) override {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.*f", digits, static_cast<double>(value));
        text += buffer;
    }
    bool has(const char* fragment) const { return text.find(fragment) != std::string::npos; }
};

struct Rig {
    FakeClock clock;
    FakeOutputs outputs;
    testing::FakeKv kv;
    configstore::ConfigStore store{kv};
    FakePrices prices{clock};
    history::SwitchHistory history;
    FakeLog log;
    controller::Controller ctl{clock, outputs, store, prices, history, log};
    ChannelConfig configs[kChannelCount];

    Rig() {
        for (uint8_t i = 0; i < kChannelCount; ++i) configs[i] = testing::quietConfig();
    }
    void begin() { ctl.begin(configs); }
    // Довести фоновый запрос до конца, как это делают вызовы servicePriceFetch из loop().
    void settle() {
        for (int i = 0; i < 10 && ctl.priceFetchInProgress(); ++i) ctl.servicePriceFetch();
    }
    void update() {
        ctl.updatePrice();
        settle();
    }
    void schedule() {
        ctl.servicePriceSchedule();
        settle();
    }
    const history::SwitchEvent& lastEvent() const { return history.newest(0); }
};

Rig* rig = nullptr;

// Канал включается порогом при цене по умолчанию 0,05 EUR/kWh.
ChannelConfig thresholdConfig() {
    ChannelConfig config = testing::quietConfig();
    config.alwaysOnBelowPriceEurKwh = 0.10f;
    return config;
}

// 21.10.2026 12:07 по Риге: не граница 15 минут.
uint32_t middayEpoch() {
    return riga(2026, 10, 21, 12, 7);
}

}  // namespace

void setUp() {
    rig = new Rig();
}

void tearDown() {
    delete rig;
    rig = nullptr;
}

// --- Запуск ------------------------------------------------------------------------

static void test_begin_turns_all_outputs_off() {
    rig->outputs.states[2] = true;  // реле осталось включенным в RAM
    rig->clock.ms = 5000;
    rig->begin();
    TEST_ASSERT_EQUAL_INT(kChannelCount, rig->outputs.setCalls);
    for (uint8_t i = 0; i < kChannelCount; ++i) TEST_ASSERT_FALSE(rig->outputs.states[i]);
    // В историю — только реальная смена состояния
    TEST_ASSERT_EQUAL_UINT32(1, rig->history.size());
    TEST_ASSERT_EQUAL_UINT8(2, rig->lastEvent().channel);
    ASSERT_ENUM(Cause::Startup, rig->lastEvent().cause);
    TEST_ASSERT_EQUAL_UINT32(0, rig->lastEvent().epoch);
    TEST_ASSERT_EQUAL_UINT32(5, rig->lastEvent().uptimeSeconds);
    TEST_ASSERT_EQUAL_STRING("Ожидание первой цены", rig->ctl.modeName(0));
}

// --- Цена и решение ----------------------------------------------------------------

static void test_update_decides_all_channels() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    TEST_ASSERT_TRUE(rig->ctl.hasLatestPrice());
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    TEST_ASSERT_FALSE(rig->outputs.states[1]);
    ASSERT_ENUM(Cause::Threshold, rig->lastEvent().cause);
    TEST_ASSERT_EQUAL_UINT32(middayEpoch(), rig->lastEvent().epoch);
    TEST_ASSERT_TRUE(rig->log.has("[DECISION] Цена 0.05000 EUR/kWh, реле 1 ВКЛЮЧАЕТСЯ: Порог включения\r\n"));
    TEST_ASSERT_TRUE(rig->log.has("реле 2 ВЫКЛЮЧАЕТСЯ: ни один сценарий не выбрал интервал\r\n"));
    TEST_ASSERT_EQUAL_STRING("Порог включения", rig->ctl.modeName(0));
}

static void test_update_before_time_sync_accepts_price_without_forecast() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->update();
    TEST_ASSERT_TRUE(rig->ctl.hasLatestPrice());
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    TEST_ASSERT_EQUAL_INT(0, rig->prices.forecastCalls);
    TEST_ASSERT_TRUE(rig->log.has("[FORECAST] Нет синхронизированного времени; прогноз не запрошен."));
}

static void test_ceiling_turns_channel_off() {
    ChannelConfig config = thresholdConfig();
    config.safetyCeilingEurKwh = 0.04f;
    rig->configs[0] = config;
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    TEST_ASSERT_FALSE(rig->outputs.states[0]);
    TEST_ASSERT_TRUE(rig->log.has("[DECISION] Сработал аварийный предел цены; реле выключено."));
    TEST_ASSERT_TRUE(rig->log.has("реле 1 ВЫКЛЮЧАЕТСЯ: Аварийный предел цены"));
}

static void test_stale_timestamp_keeps_outputs_and_price() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    TEST_ASSERT_TRUE(rig->outputs.states[0]);

    rig->prices.price = 0.5f;  // выключило бы порог, но timestamp тот же
    rig->update();
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.05f, rig->ctl.latestPriceEurKwh());
    char expected[128];
    snprintf(expected, sizeof(expected),
        "[WARN] API пока возвращает устаревшую цену (timestamp %lu, ожидается значение больше %lu).",
        static_cast<unsigned long>(riga(2026, 10, 21, 12, 0)),
        static_cast<unsigned long>(riga(2026, 10, 21, 12, 0)));
    TEST_ASSERT_TRUE(rig->log.has(expected));
    uint32_t retry = 99;
    TEST_ASSERT_TRUE(rig->ctl.priceRetryPending(retry));
    TEST_ASSERT_EQUAL_UINT32(30, retry);
}

static void test_fetch_failure_keeps_outputs() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    rig->prices.currentOk = false;
    rig->update();
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    TEST_ASSERT_TRUE(rig->ctl.hasLatestPrice());
    TEST_ASSERT_TRUE(rig->log.has("[WARN] Не удалось получить цену. Реле сохраняет предыдущее состояние."));
}

// --- Фоновый запрос (шаг 2g, D-026) ------------------------------------------------

// Цена применяется сразу, по прежнему прогнозу, а прогноз запрашивается следом (D-030).
static void test_price_applied_before_forecast() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->prices.hold = true;
    rig->ctl.updatePrice();
    TEST_ASSERT_TRUE(rig->ctl.priceFetchInProgress());
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);
    rig->ctl.servicePriceFetch();
    TEST_ASSERT_FALSE(rig->ctl.hasLatestPrice());

    rig->prices.hold = false;
    rig->ctl.servicePriceFetch();  // цена есть, решение принято, запрошен прогноз
    rig->prices.hold = true;
    TEST_ASSERT_EQUAL_INT(1, rig->prices.forecastCalls);
    TEST_ASSERT_TRUE(rig->ctl.priceFetchInProgress());
    TEST_ASSERT_TRUE(rig->ctl.hasLatestPrice());
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    const size_t decided = rig->log.text.find("[DECISION] Цена 0.05000 EUR/kWh, реле 1 ВКЛЮЧАЕТСЯ");
    const size_t forecast = rig->log.text.find("[FORECAST] Запрос цен Elering на сегодня и завтра...");
    TEST_ASSERT_TRUE(decided != std::string::npos && forecast != std::string::npos);
    TEST_ASSERT_TRUE(decided < forecast);

    rig->prices.hold = false;
    rig->settle();
    TEST_ASSERT_FALSE(rig->ctl.priceFetchInProgress());
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
}

// Прогноз после новой цены: план пересчитывается сразу, а не на следующей минуте.
static void test_forecast_after_price_replans_immediately() {
    ChannelConfig config = testing::quietConfig();
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = 24;  // все интервалы суток
    rig->configs[0] = config;
    rig->begin();
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);
    rig->clock.sync(middayEpoch());
    rig->update();
    // Решение по цене — до прогноза: умный порог еще ждет цен
    TEST_ASSERT_TRUE(rig->log.has("реле 1 ВЫКЛЮЧАЕТСЯ"));
    TEST_ASSERT_FALSE(rig->outputs.states[0]);

    rig->ctl.servicePriceSchedule();  // следующий loop() в ту же минуту
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::Smart, rig->lastEvent().cause);
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);
}

static void test_update_while_fetching_does_not_start_second_request() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->prices.hold = true;
    rig->ctl.servicePriceSchedule();
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);

    // Повтор раз в 30 с ждет конца запроса и не пишет в лог
    rig->clock.advanceMs(2 * kPriceRetryIntervalMs);
    rig->ctl.servicePriceSchedule();
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);
    TEST_ASSERT_FALSE(rig->log.has("[TASK] Запрос цены уже идет"));

    // Команда пользователя — с сообщением
    rig->ctl.updatePrice();
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);
    TEST_ASSERT_TRUE(rig->log.has("[TASK] Запрос цены уже идет; решение будет принято по его результату."));
}

// Запрос начат до границы 12:15, ответ (цена 12:00) пришел после нее: цена
// принимается, но ожидание цены 12:15 продолжается без паузы в 30 с.
static void test_boundary_during_fetch_keeps_waiting_for_new_price() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->prices.timestamp = riga(2026, 10, 21, 11, 45);
    rig->schedule();

    rig->prices.timestamp = riga(2026, 10, 21, 12, 0);
    rig->prices.hold = true;
    rig->clock.advanceTo(riga(2026, 10, 21, 12, 14) + 50);
    rig->ctl.updatePrice();
    rig->clock.advanceTo(riga(2026, 10, 21, 12, 15) + 5);
    rig->ctl.servicePriceSchedule();  // граница, запрос еще идет
    TEST_ASSERT_EQUAL_INT(2, rig->prices.currentCalls);
    TEST_ASSERT_TRUE(rig->log.has("[SCHEDULE] Начинается проверка цены на границе 15-минутного интервала."));

    rig->prices.hold = false;
    rig->settle();
    TEST_ASSERT_TRUE(rig->ctl.hasLatestPrice());
    uint32_t retry = 99;
    TEST_ASSERT_TRUE(rig->ctl.priceRetryPending(retry));
    TEST_ASSERT_EQUAL_UINT32(0, retry);

    rig->prices.timestamp = 0;  // цена 12:15
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(3, rig->prices.currentCalls);
    TEST_ASSERT_FALSE(rig->ctl.priceRetryPending(retry));
}

// Актуальность — по часам в момент результата, а не запроса.
static void test_price_freshness_checked_when_result_arrives() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->prices.timestamp = riga(2026, 10, 21, 12, 0);
    rig->prices.hold = true;
    rig->ctl.updatePrice();
    rig->clock.advanceTo(riga(2026, 10, 21, 13, 0));
    rig->prices.hold = false;
    rig->settle();
    TEST_ASSERT_FALSE(rig->ctl.hasLatestPrice());
    TEST_ASSERT_FALSE(rig->outputs.states[0]);
    TEST_ASSERT_EQUAL_INT(0, rig->prices.forecastCalls);
    char expected[128];
    snprintf(expected, sizeof(expected),
        "[ERROR] Цена не относится к текущему времени (timestamp %lu, сейчас %lu).",
        static_cast<unsigned long>(riga(2026, 10, 21, 12, 0)),
        static_cast<unsigned long>(riga(2026, 10, 21, 13, 0)));
    TEST_ASSERT_TRUE(rig->log.has(expected));
    TEST_ASSERT_TRUE(rig->log.has("[WARN] Не удалось получить цену. Реле сохраняет предыдущее состояние."));
    uint32_t retry = 0;
    TEST_ASSERT_TRUE(rig->ctl.priceRetryPending(retry));
}

// Без часов на момент результата актуальность не проверяется, прогноз не запрашивается.
static void test_unsynced_result_skips_freshness_and_forecast() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->prices.timestamp = riga(2026, 10, 1, 0, 0);
    rig->update();
    TEST_ASSERT_TRUE(rig->ctl.hasLatestPrice());
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    TEST_ASSERT_FALSE(rig->log.has("[ERROR]"));
    TEST_ASSERT_EQUAL_INT(0, rig->prices.forecastCalls);
}

// Потерянный результат (источник ответил Idle) — неудача, а не вечное ожидание.
static void test_lost_result_counts_as_failure() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->ctl.updatePrice();
    TEST_ASSERT_TRUE(rig->ctl.priceFetchInProgress());
    float price = 0.0f;
    long ts = 0;
    rig->prices.poll(price, ts);  // результат забрал кто-то другой
    rig->ctl.servicePriceFetch();
    TEST_ASSERT_FALSE(rig->ctl.priceFetchInProgress());
    TEST_ASSERT_TRUE(rig->log.has("[WARN] Не удалось получить цену."));
}

// --- Расписание --------------------------------------------------------------------

static void test_first_schedule_requests_price_immediately() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);
    // Цена получена — до следующей границы новых запросов нет
    rig->clock.advanceMs(5 * 60 * 1000UL);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);
}

// D-034: ручной режим — рычаг, граница 15 минут и новая цена его не снимают.
static void test_boundary_requests_price_and_keeps_manual() {
    rig->configs[1] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->schedule();
    rig->ctl.setManual(1, false);
    TEST_ASSERT_TRUE(rig->ctl.manual(1));

    rig->clock.advanceTo(riga(2026, 10, 21, 12, 15) + 10);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(2, rig->prices.currentCalls);
    TEST_ASSERT_TRUE(rig->ctl.manual(1));
    TEST_ASSERT_FALSE(rig->outputs.states[1]);  // порог включил бы, но рычаг главнее
    TEST_ASSERT_TRUE(rig->log.has("[DECISION] Реле 2: активен ручной режим, остальные условия не применяются."));
    TEST_ASSERT_TRUE(rig->log.has("[SCHEDULE] Начинается проверка цены на границе 15-минутного интервала."));
}

static void test_retry_every_30_seconds_until_new_timestamp() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->prices.timestamp = riga(2026, 10, 21, 12, 0);
    rig->schedule();
    rig->clock.advanceTo(riga(2026, 10, 21, 12, 15));
    rig->schedule();  // граница: timestamp прежний
    TEST_ASSERT_EQUAL_INT(2, rig->prices.currentCalls);

    rig->clock.advanceMs(kPriceRetryIntervalMs - 1);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(2, rig->prices.currentCalls);
    rig->clock.advanceMs(1);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(3, rig->prices.currentCalls);

    rig->prices.timestamp = 0;  // API отдал цену 12:15
    rig->clock.advanceMs(kPriceRetryIntervalMs);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(4, rig->prices.currentCalls);
    uint32_t retry = 0;
    TEST_ASSERT_FALSE(rig->ctl.priceRetryPending(retry));
    rig->clock.advanceMs(kPriceRetryIntervalMs);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(4, rig->prices.currentCalls);
}

static void test_scheduled_control_switches_on_wakeup_minute() {
    ChannelConfig config = testing::quietConfig();
    config.wakeupEnabled = true;
    config.wakeupHours = 1;
    config.wakeupTimeMinuteOfDay = 7 * 60 + 10;  // включено с 06:10
    rig->configs[0] = config;
    rig->begin();
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);

    rig->clock.sync(riga(2026, 10, 21, 6, 1));
    rig->schedule();
    TEST_ASSERT_FALSE(rig->outputs.states[0]);

    rig->clock.advanceTo(riga(2026, 10, 21, 6, 9));
    rig->schedule();
    TEST_ASSERT_FALSE(rig->outputs.states[0]);

    rig->clock.advanceTo(riga(2026, 10, 21, 6, 10));
    rig->schedule();
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::Wakeup, rig->lastEvent().cause);
    TEST_ASSERT_TRUE(rig->log.has("[SCHEDULE] Переключение реле 1 по сохраненному плану цен: Включение перед пробуждением"));
}

// --- Прогноз (D-018) ---------------------------------------------------------------

static void test_forecast_failure_keeps_previous_series() {
    rig->begin();
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);
    rig->clock.sync(middayEpoch());
    rig->update();
    TEST_ASSERT_EQUAL_UINT32(192, rig->ctl.forecast().size());

    rig->prices.forecastOk = false;
    rig->clock.advanceTo(riga(2026, 10, 21, 12, 16));
    rig->update();
    TEST_ASSERT_EQUAL_UINT32(192, rig->ctl.forecast().size());
    TEST_ASSERT_TRUE(rig->log.has("[FORECAST] Используется прежний прогноз, квартальных значений: 192"));
}

static void test_yesterday_prices_requested_once_per_day() {
    rig->begin();
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);
    rig->clock.sync(middayEpoch());
    rig->update();
    TEST_ASSERT_EQUAL_INT(2, rig->prices.forecastCalls);  // сегодня–завтра + вчера
    TEST_ASSERT_TRUE(rig->log.has("[FORECAST] Запрос вчерашних цен Elering..."));

    rig->clock.advanceTo(riga(2026, 10, 21, 12, 16));
    rig->update();
    TEST_ASSERT_EQUAL_INT(3, rig->prices.forecastCalls);
}

// Цена пришла раньше часов: прогноз запрашивается сразу после синхронизации, а не
// с новой ценой через 15 минут (D-028).
static void test_forecast_requested_after_late_time_sync() {
    rig->begin();
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);
    rig->update();  // часы еще не синхронизированы
    TEST_ASSERT_EQUAL_INT(0, rig->prices.forecastCalls);
    TEST_ASSERT_EQUAL_UINT32(0, rig->ctl.forecast().size());

    rig->clock.sync(middayEpoch());
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);  // новой цены не просили
    TEST_ASSERT_TRUE(rig->prices.forecastCalls >= 1);
    TEST_ASSERT_EQUAL_UINT32(192, rig->ctl.forecast().size());
    TEST_ASSERT_TRUE(rig->log.has("[FORECAST] Прогноз не покрывает текущее время; запрос без новой цены."));
    TEST_ASSERT_FALSE(rig->ctl.priceFetchInProgress());
}

// Прогноз не получен: повтор раз в минуту, пока не покроет текущее время.
static void test_missing_forecast_retried_every_minute() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->prices.forecastOk = false;
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(1, rig->prices.forecastCalls);

    rig->clock.advanceMs(controller::kForecastRetryIntervalMs - 1);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(1, rig->prices.forecastCalls);
    rig->clock.advanceMs(1);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(2, rig->prices.forecastCalls);

    rig->prices.forecastOk = true;
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);
    rig->clock.advanceMs(controller::kForecastRetryIntervalMs);
    rig->schedule();
    const int calls = rig->prices.forecastCalls;
    TEST_ASSERT_EQUAL_UINT32(192, rig->ctl.forecast().size());
    // Прогноз есть — больше без новой цены не запрашивается
    rig->clock.advanceMs(3 * controller::kForecastRetryIntervalMs);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(calls, rig->prices.forecastCalls);
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);
}

// Прогноз без цены сразу дает план: сценарий включает реле, не дожидаясь минуты.
static void test_forecast_only_fetch_replans_immediately() {
    ChannelConfig config = testing::quietConfig();
    config.smartThresholdEnabled = true;
    config.smartThresholdHours = 24;  // все интервалы суток
    rig->configs[0] = config;
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->prices.forecastOk = false;
    rig->schedule();
    TEST_ASSERT_FALSE(rig->outputs.states[0]);

    rig->prices.forecastOk = true;
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);
    rig->clock.advanceMs(controller::kForecastRetryIntervalMs);
    rig->schedule();
    rig->ctl.servicePriceSchedule();  // следующий loop() в ту же минуту
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
}

// --- Failsafe (D-001) --------------------------------------------------------------

static void test_failsafe_turns_off_only_automatic_channels() {
    rig->configs[0] = thresholdConfig();
    rig->configs[1] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    rig->ctl.setManual(1, true);
    rig->ctl.startForceOn(2);

    rig->clock.advanceMs(kPriceFailsafeTimeoutMs - 1);
    rig->ctl.serviceFailsafe();
    TEST_ASSERT_TRUE(rig->ctl.hasLatestPrice());
    TEST_ASSERT_TRUE(rig->outputs.states[0]);

    rig->clock.advanceMs(1);
    rig->ctl.serviceFailsafe();
    TEST_ASSERT_FALSE(rig->ctl.hasLatestPrice());
    TEST_ASSERT_FALSE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::Failsafe, rig->lastEvent().cause);
    TEST_ASSERT_TRUE(rig->outputs.states[1]);  // ручной режим
    TEST_ASSERT_TRUE(rig->outputs.states[2]);  // принудительное включение
    TEST_ASSERT_TRUE(rig->log.has("[FAILSAFE] Новой цены нет больше часа; автоматические реле выключаются."));
    TEST_ASSERT_EQUAL_STRING("Нет актуальной цены: реле выключено", rig->ctl.modeName(0));
    TEST_ASSERT_TRUE(rig->ctl.hadAnyPrice());
    uint32_t retry = 0;
    TEST_ASSERT_TRUE(rig->ctl.priceRetryPending(retry));
}

static void test_failsafe_without_price_does_nothing() {
    rig->begin();
    rig->clock.advanceMs(2 * kHourMs);
    rig->ctl.serviceFailsafe();
    TEST_ASSERT_FALSE(rig->log.has("[FAILSAFE]"));
}

// --- Ручной режим и принудительное включение (D-006, D-007) ------------------------

static void test_manual_on_and_announce_order() {
    rig->begin();
    rig->ctl.startForceOn(0);
    rig->log.text.clear();
    rig->ctl.setManual(0, false, "\n[BUTTON] нажата");
    TEST_ASSERT_FALSE(rig->ctl.forceOnActive(0));
    TEST_ASSERT_TRUE(rig->ctl.manual(0));
    TEST_ASSERT_FALSE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::Manual, rig->lastEvent().cause);
    TEST_ASSERT_EQUAL_STRING(
        "[FORCE] Реле 1: принудительное включение отменено ручным режимом.\r\n"
        "\n[BUTTON] нажата\r\n",
        rig->log.text.c_str());
    TEST_ASSERT_EQUAL_STRING("Ручной", rig->ctl.modeName(0));
}

// D-034: в ручном режиме принудительное включение отклоняется, выход не меняется.
static void test_force_on_rejected_in_manual() {
    rig->begin();
    rig->ctl.setManual(0, false);
    TEST_ASSERT_FALSE(rig->ctl.startForceOn(0));
    TEST_ASSERT_TRUE(rig->ctl.manual(0));
    TEST_ASSERT_FALSE(rig->ctl.forceOnActive(0));
    TEST_ASSERT_FALSE(rig->outputs.states[0]);
    TEST_ASSERT_TRUE(rig->log.has("[FORCE] Реле 1: включен ручной режим; принудительное включение недоступно, пока реле не вернут автоматике."));
    TEST_ASSERT_EQUAL_STRING("Ручной", rig->ctl.modeName(0));
}

static void test_force_on_starts_without_manual() {
    rig->begin();
    TEST_ASSERT_TRUE(rig->ctl.startForceOn(0));
    TEST_ASSERT_TRUE(rig->ctl.forceOnActive(0));
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::ForceOn, rig->lastEvent().cause);
    TEST_ASSERT_TRUE(rig->log.has("[FORCE] Реле 1: принудительное включение на 3 ч, цена не учитывается."));
    TEST_ASSERT_EQUAL_STRING("Принудительное включение", rig->ctl.modeName(0));
}

// D-034: рычаг держится часами — и без цены, и после failsafe.
static void test_manual_survives_hours_and_failsafe() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    rig->ctl.setManual(0, false);
    for (int hour = 0; hour < 5; ++hour) {
        rig->clock.advanceMs(kHourMs);
        rig->ctl.serviceFailsafe();
        rig->schedule();
    }
    TEST_ASSERT_TRUE(rig->ctl.manual(0));
    TEST_ASSERT_FALSE(rig->outputs.states[0]);
}

// Вернуть автоматике: решение сразу по последней цене, без запроса в сеть.
static void test_release_manual_decides_immediately() {
    rig->configs[0] = thresholdConfig();
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    rig->ctl.setManual(0, false);
    const int calls = rig->prices.currentCalls;
    rig->ctl.releaseManual(0);
    TEST_ASSERT_FALSE(rig->ctl.manual(0));
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::Threshold, rig->lastEvent().cause);
    TEST_ASSERT_EQUAL_INT(calls, rig->prices.currentCalls);
    TEST_ASSERT_TRUE(rig->log.has("[MANUAL] Реле 1: ручной режим снят пользователем."));
    TEST_ASSERT_TRUE(rig->log.has("[MANUAL] Реле 1: автоматика оставляет реле включенным: Порог включения"));
}

static void test_release_manual_without_price_turns_off() {
    rig->begin();
    rig->ctl.setManual(2, true);
    rig->ctl.releaseManual(2);
    TEST_ASSERT_FALSE(rig->ctl.manual(2));
    TEST_ASSERT_FALSE(rig->outputs.states[2]);
    ASSERT_ENUM(Cause::NoPrice, rig->lastEvent().cause);
    TEST_ASSERT_TRUE(rig->log.has("[MANUAL] Реле 3: актуальной цены нет; реле выключается до решения автоматики."));
}

static void test_release_without_manual_only_logs() {
    rig->begin();
    const int calls = rig->outputs.setCalls;
    rig->ctl.releaseManual(1);
    TEST_ASSERT_EQUAL_INT(calls, rig->outputs.setCalls);
    TEST_ASSERT_TRUE(rig->log.has("[MANUAL] Реле 2: ручной режим не включен."));
}

// Перезагрузку при зависшем запросе откладывает любое идущее принудительное включение.
static void test_any_force_on_active() {
    rig->begin();
    TEST_ASSERT_FALSE(rig->ctl.anyForceOnActive());
    rig->ctl.startForceOn(3);
    TEST_ASSERT_TRUE(rig->ctl.anyForceOnActive());
    rig->ctl.setManual(3, false);  // ручной режим отменяет принудительное включение
    TEST_ASSERT_FALSE(rig->ctl.anyForceOnActive());
    rig->ctl.startForceOn(1);
    rig->clock.advanceMs(3 * kHourMs);
    rig->ctl.serviceForceOn();
    TEST_ASSERT_FALSE(rig->ctl.anyForceOnActive());
}

static void test_force_on_restart_counts_from_zero() {
    rig->begin();
    rig->ctl.startForceOn(0);
    rig->clock.advanceMs(kHourMs);
    rig->ctl.startForceOn(0);
    TEST_ASSERT_EQUAL_UINT32(3 * kHourMs, rig->ctl.forceOnRemainingMs(0));
    TEST_ASSERT_TRUE(rig->log.has("принудительное включение перезапущено на 3 ч"));
}

static void test_force_on_ignores_price_update_and_boundary() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->ctl.startForceOn(0);
    rig->schedule();
    rig->clock.advanceTo(riga(2026, 10, 21, 12, 15));
    rig->schedule();
    TEST_ASSERT_TRUE(rig->ctl.forceOnActive(0));
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    TEST_ASSERT_TRUE(rig->log.has("[DECISION] Реле 1: идет принудительное включение, цена не учитывается."));
}

static void test_force_on_expires_without_price_turns_off() {
    rig->configs[0] = thresholdConfig();
    rig->configs[0].forceOnHours = 2;
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    rig->ctl.startForceOn(0);

    // Failsafe через час не трогает принудительное включение
    rig->clock.advanceMs(kPriceFailsafeTimeoutMs);
    rig->ctl.serviceFailsafe();
    rig->ctl.serviceForceOn();
    TEST_ASSERT_TRUE(rig->outputs.states[0]);

    rig->clock.advanceMs(kHourMs - 1);
    rig->ctl.serviceForceOn();
    TEST_ASSERT_TRUE(rig->ctl.forceOnActive(0));
    rig->clock.advanceMs(1);
    rig->ctl.serviceForceOn();
    TEST_ASSERT_FALSE(rig->ctl.forceOnActive(0));
    TEST_ASSERT_FALSE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::NoPrice, rig->lastEvent().cause);
    TEST_ASSERT_TRUE(rig->log.has("[FORCE] Реле 1: время принудительного включения вышло."));
    TEST_ASSERT_TRUE(rig->log.has("актуальной цены нет; реле выключается до решения автоматики."));
}

static void test_force_on_expires_to_automation_decision() {
    rig->configs[0] = thresholdConfig();
    rig->configs[0].forceOnHours = 1;
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    const size_t events = rig->history.size();
    rig->ctl.startForceOn(0);
    rig->clock.advanceMs(kHourMs);
    rig->ctl.serviceForceOn();
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    TEST_ASSERT_TRUE(rig->log.has("[FORCE] Реле 1: автоматика оставляет реле включенным: Порог включения"));
    // Выход все время был включен: в истории новых событий нет
    TEST_ASSERT_EQUAL_UINT32(events, rig->history.size());
}

static void test_stop_force_on_returns_to_automation() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    rig->ctl.startForceOn(0);
    rig->ctl.stopForceOn(0);
    TEST_ASSERT_FALSE(rig->ctl.forceOnActive(0));
    TEST_ASSERT_FALSE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::NoScenario, rig->lastEvent().cause);
    TEST_ASSERT_TRUE(rig->log.has("принудительное включение остановлено пользователем."));
    TEST_ASSERT_TRUE(rig->log.has("автоматика выключает реле: ни один сценарий не выбрал интервал"));
}

static void test_stop_force_on_when_idle_only_logs() {
    rig->begin();
    const int calls = rig->outputs.setCalls;
    rig->ctl.stopForceOn(3);
    TEST_ASSERT_EQUAL_INT(calls, rig->outputs.setCalls);
    TEST_ASSERT_TRUE(rig->log.has("[FORCE] Реле 4: принудительное включение не идет."));
}

static void test_force_on_survives_millis_overflow() {
    rig->clock.ms = 0xFFFFFFFFUL - kHourMs / 2;
    rig->configs[0].forceOnHours = 1;
    rig->begin();
    rig->ctl.startForceOn(0);
    rig->clock.advanceMs(kHourMs - 1);
    TEST_ASSERT_TRUE(rig->clock.ms < kHourMs);  // millis() переполнился
    rig->ctl.serviceForceOn();
    TEST_ASSERT_TRUE(rig->ctl.forceOnActive(0));
    rig->clock.advanceMs(1);
    rig->ctl.serviceForceOn();
    TEST_ASSERT_FALSE(rig->ctl.forceOnActive(0));
}

// --- Настройки ---------------------------------------------------------------------

static void test_save_config_failure_keeps_ram() {
    rig->begin();
    ChannelConfig updated = thresholdConfig();
    rig->kv.failSet = true;
    TEST_ASSERT_FALSE(rig->ctl.saveConfig(1, updated));
    TEST_ASSERT_EQUAL_FLOAT(-10.0f, rig->ctl.config(1).alwaysOnBelowPriceEurKwh);

    rig->kv.failSet = false;
    TEST_ASSERT_TRUE(rig->ctl.saveConfig(1, updated));
    TEST_ASSERT_EQUAL_FLOAT(0.10f, rig->ctl.config(1).alwaysOnBelowPriceEurKwh);
    ChannelConfig loaded[kChannelCount];
    rig->store.load(loaded);
    TEST_ASSERT_EQUAL_FLOAT(0.10f, loaded[1].alwaysOnBelowPriceEurKwh);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, loaded[0].safetyCeilingEurKwh);
}

// KI-011: после сохранения настроек решение — по уже загруженному прогнозу, без запроса.
static void test_apply_config_uses_loaded_forecast() {
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    const int calls = rig->prices.forecastCalls;
    ChannelConfig updated = thresholdConfig();
    updated.smartThresholdEnabled = true;
    TEST_ASSERT_TRUE(rig->ctl.saveConfig(0, updated));
    rig->ctl.applyConfiguredControl(0);
    TEST_ASSERT_EQUAL_INT(calls, rig->prices.forecastCalls);
    TEST_ASSERT_EQUAL_INT(1, rig->prices.currentCalls);
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::Threshold, rig->lastEvent().cause);
}

static ChannelConfig expiredHolidayConfig() {
    ChannelConfig config = testing::quietConfig();
    config.holidayEnabled = true;
    config.holidayReturnEpoch = middayEpoch() - 60;
    return config;
}

// KI-013: решение не пишет flash; прошедший отпуск на решение не влияет.
static void test_decision_does_not_save_expired_holiday() {
    rig->configs[0] = expiredHolidayConfig();
    rig->configs[0].alwaysOnBelowPriceEurKwh = 0.10f;
    rig->begin();
    rig->clock.sync(middayEpoch());
    rig->update();
    TEST_ASSERT_EQUAL_INT(0, rig->kv.setCalls);
    TEST_ASSERT_TRUE(rig->ctl.config(0).holidayEnabled);
    TEST_ASSERT_TRUE(rig->outputs.states[0]);
    ASSERT_ENUM(Cause::Threshold, rig->lastEvent().cause);
    TEST_ASSERT_EQUAL_STRING("Порог включения", rig->ctl.modeName(0));
}

// KI-013: отпуск снимается отдельным шагом расписания один раз, в том числе в ручном
// режиме и без цены.
static void test_schedule_expires_holiday_once() {
    rig->configs[2] = expiredHolidayConfig();
    rig->begin();
    rig->ctl.setManual(2, true);
    rig->prices.currentOk = false;
    rig->clock.sync(middayEpoch());
    rig->schedule();
    TEST_ASSERT_FALSE(rig->ctl.config(2).holidayEnabled);
    TEST_ASSERT_EQUAL_INT(1, rig->kv.setCalls);
    TEST_ASSERT_TRUE(rig->log.has("[HOLIDAY] Реле 3: дата возвращения наступила; режим отпуска завершен.\r\n"));
    ChannelConfig loaded[kChannelCount];
    rig->store.load(loaded);
    TEST_ASSERT_FALSE(loaded[2].holidayEnabled);

    rig->clock.advanceMs(60 * 1000UL);
    rig->schedule();
    TEST_ASSERT_EQUAL_INT(1, rig->kv.setCalls);
}

static void test_holiday_expiry_save_failure_logged() {
    rig->configs[0] = expiredHolidayConfig();
    rig->begin();
    rig->kv.failSet = true;
    rig->clock.sync(middayEpoch());
    rig->schedule();
    TEST_ASSERT_FALSE(rig->ctl.config(0).holidayEnabled);
    TEST_ASSERT_TRUE(rig->log.has("[ERROR] Не удалось сохранить завершение режима отпуска."));
}

static size_t countOf(const std::string& text, const char* fragment) {
    size_t count = 0;
    for (size_t at = text.find(fragment); at != std::string::npos;
         at = text.find(fragment, at + 1)) {
        ++count;
    }
    return count;
}

// KI-013: при пересчете раз в минуту строки плана — только при изменении;
// при новой цене — всегда.
static void test_minute_recalc_logs_plan_only_on_change() {
    ChannelConfig config = testing::quietConfig();
    config.smartThresholdEnabled = true;
    rig->configs[0] = config;
    rig->begin();
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);
    const char* line = "[DECISION] Решение по сценарию «Умный порог включения».";

    rig->clock.sync(middayEpoch());
    rig->schedule();
    // Первая цена решается без прогноза, план по нему — в следующем loop() (D-030)
    TEST_ASSERT_EQUAL_UINT32(0, countOf(rig->log.text, line));
    rig->ctl.servicePriceSchedule();
    TEST_ASSERT_EQUAL_UINT32(1, countOf(rig->log.text, line));
    for (int minute = 0; minute < 5; ++minute) {
        rig->clock.advanceMs(60 * 1000UL);
        rig->schedule();
    }
    TEST_ASSERT_EQUAL_UINT32(1, countOf(rig->log.text, line));

    rig->clock.advanceTo(riga(2026, 10, 21, 12, 15));
    rig->schedule();
    TEST_ASSERT_EQUAL_UINT32(2, countOf(rig->log.text, line));
}

static void test_minute_recalc_logs_ceiling_once() {
    ChannelConfig config = testing::quietConfig();
    config.smartThresholdEnabled = true;
    config.safetyCeilingEurKwh = 0.04f;
    rig->configs[0] = config;
    rig->begin();
    const char* line = "[DECISION] Сработал аварийный предел цены; реле выключено.";

    rig->clock.sync(middayEpoch());
    rig->schedule();
    for (int minute = 0; minute < 3; ++minute) {
        rig->clock.advanceMs(60 * 1000UL);
        rig->schedule();
    }
    TEST_ASSERT_EQUAL_UINT32(1, countOf(rig->log.text, line));
}

static void test_chart_plan_empty_without_time() {
    rig->begin();
    const float hourPrices[48] = {};
    testing::fillHours(rig->prices.series, riga(2026, 10, 21, 0, 0), hourPrices, 48);
    rig->clock.sync(middayEpoch());
    rig->update();
    TEST_ASSERT_EQUAL_UINT32(0, rig->ctl.chartPlan(0, 0).slotCount);
    TEST_ASSERT_EQUAL_UINT32(192, rig->ctl.chartPlan(0, middayEpoch()).slotCount);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_turns_all_outputs_off);
    RUN_TEST(test_update_decides_all_channels);
    RUN_TEST(test_update_before_time_sync_accepts_price_without_forecast);
    RUN_TEST(test_ceiling_turns_channel_off);
    RUN_TEST(test_stale_timestamp_keeps_outputs_and_price);
    RUN_TEST(test_fetch_failure_keeps_outputs);
    RUN_TEST(test_price_applied_before_forecast);
    RUN_TEST(test_forecast_after_price_replans_immediately);
    RUN_TEST(test_update_while_fetching_does_not_start_second_request);
    RUN_TEST(test_boundary_during_fetch_keeps_waiting_for_new_price);
    RUN_TEST(test_price_freshness_checked_when_result_arrives);
    RUN_TEST(test_unsynced_result_skips_freshness_and_forecast);
    RUN_TEST(test_lost_result_counts_as_failure);
    RUN_TEST(test_first_schedule_requests_price_immediately);
    RUN_TEST(test_boundary_requests_price_and_keeps_manual);
    RUN_TEST(test_retry_every_30_seconds_until_new_timestamp);
    RUN_TEST(test_scheduled_control_switches_on_wakeup_minute);
    RUN_TEST(test_forecast_failure_keeps_previous_series);
    RUN_TEST(test_yesterday_prices_requested_once_per_day);
    RUN_TEST(test_forecast_requested_after_late_time_sync);
    RUN_TEST(test_missing_forecast_retried_every_minute);
    RUN_TEST(test_forecast_only_fetch_replans_immediately);
    RUN_TEST(test_failsafe_turns_off_only_automatic_channels);
    RUN_TEST(test_failsafe_without_price_does_nothing);
    RUN_TEST(test_manual_on_and_announce_order);
    RUN_TEST(test_force_on_rejected_in_manual);
    RUN_TEST(test_force_on_starts_without_manual);
    RUN_TEST(test_manual_survives_hours_and_failsafe);
    RUN_TEST(test_release_manual_decides_immediately);
    RUN_TEST(test_release_manual_without_price_turns_off);
    RUN_TEST(test_release_without_manual_only_logs);
    RUN_TEST(test_any_force_on_active);
    RUN_TEST(test_force_on_restart_counts_from_zero);
    RUN_TEST(test_force_on_ignores_price_update_and_boundary);
    RUN_TEST(test_force_on_expires_without_price_turns_off);
    RUN_TEST(test_force_on_expires_to_automation_decision);
    RUN_TEST(test_stop_force_on_returns_to_automation);
    RUN_TEST(test_stop_force_on_when_idle_only_logs);
    RUN_TEST(test_force_on_survives_millis_overflow);
    RUN_TEST(test_save_config_failure_keeps_ram);
    RUN_TEST(test_apply_config_uses_loaded_forecast);
    RUN_TEST(test_decision_does_not_save_expired_holiday);
    RUN_TEST(test_schedule_expires_holiday_once);
    RUN_TEST(test_holiday_expiry_save_failure_logged);
    RUN_TEST(test_minute_recalc_logs_plan_only_on_change);
    RUN_TEST(test_minute_recalc_logs_ceiling_once);
    RUN_TEST(test_chart_plan_empty_without_time);
    return UNITY_END();
}
