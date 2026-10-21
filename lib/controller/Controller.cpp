#include "Controller.h"

#include <string.h>

#include <Elering.h>
#include <RigaTime.h>

namespace controller {

namespace {

history::Cause causeFor(planner::Reason reason) {
    switch (reason) {
        case planner::Reason::Threshold: return history::Cause::Threshold;
        case planner::Reason::Smart: return history::Cause::Smart;
        case planner::Reason::Wakeup: return history::Cause::Wakeup;
        case planner::Reason::Night: return history::Cause::Night;
        case planner::Reason::Holiday: return history::Cause::Holiday;
        case planner::Reason::HolidayOff: return history::Cause::HolidayOff;
        case planner::Reason::Ceiling: return history::Cause::Ceiling;
        case planner::Reason::None: break;
    }
    return history::Cause::NoScenario;
}

// Трассы дают одинаковые строки лога.
bool sameTrace(const planner::PlanTrace& a, const planner::PlanTrace& b) {
    return a.holiday == b.holiday && a.smart == b.smart && a.wakeup == b.wakeup &&
        a.night == b.night &&
        a.smartClamped == b.smartClamped && a.nightClamped == b.nightClamped &&
        a.holidayClamped == b.holidayClamped &&
        a.smartRequested == b.smartRequested && a.smartAvailable == b.smartAvailable &&
        a.nightRequested == b.nightRequested && a.nightAvailable == b.nightAvailable &&
        a.holidayRequested == b.holidayRequested && a.holidayAvailable == b.holidayAvailable;
}

}  // namespace

Controller::Controller(Clock& clock, Outputs& outputs, configstore::ConfigStore& store,
    PriceSource& prices, history::SwitchHistory& history, LogSink& log)
    : clock_(clock), outputs_(outputs), store_(store), prices_(prices), history_(history),
      log_(log) {}

void Controller::begin(const ChannelConfig loaded[kChannelCount]) {
    memcpy(configs_, loaded, sizeof(configs_));
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        setOutput(i, false, history::Cause::Startup);
    }
}

uint32_t Controller::epochOrZero() const {
    return clock_.synced() ? clock_.nowUtc() : 0;
}

void Controller::setOutput(uint8_t channel, bool on, history::Cause cause) {
    if (channel >= kChannelCount) return;
    if (outputs_.get(channel) != on) {
        history_.record(epochOrZero(), clock_.uptimeMs() / 1000UL, channel, on, cause);
    }
    outputs_.set(channel, on);
}

bool Controller::overridden(uint8_t channel) const {
    return manual_[channel] || forceOn_[channel].active();
}

bool Controller::anyForceOnActive() const {
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (forceOn_[i].active()) return true;
    }
    return false;
}

uint32_t Controller::forceOnRemainingMs(uint8_t channel) const {
    return forceOn_[channel].remainingMs(clock_.uptimeMs());
}

const char* Controller::modeName(uint8_t channel) const {
    planner::ModeInputs in = {
        manual_[channel], forceOn_[channel].active(),
        hasLatestPrice_, latestPriceTimestamp_ != 0,
        latestPriceEurKwh_, clock_.synced(),
        epochOrZero()
    };
    return planner::modeName(configs_[channel], in);
}

bool Controller::priceRetryPending(uint32_t& remainingSeconds) const {
    if (!awaitingNewPriceTimestamp_ || !hasAttemptedPriceUpdate_) return false;
    const uint32_t elapsed = clock_.uptimeMs() - lastUpdateMs_;
    remainingSeconds = elapsed >= kPriceRetryIntervalMs
        ? 0
        : (kPriceRetryIntervalMs - elapsed) / 1000;
    return true;
}

const planner::ChannelPlan& Controller::chartPlan(uint8_t channel, uint32_t now) {
    plan_.slotCount = 0;
    if (now != 0) {
        planner::buildPlan(configs_[channel], forecast_, now, plannerStates_[channel], plan_);
    }
    return plan_;
}

bool Controller::saveConfig(uint8_t channel, const ChannelConfig& config) {
    ChannelConfig updated[kChannelCount];
    memcpy(updated, configs_, sizeof(updated));
    updated[channel] = config;
    if (!store_.save(updated)) return false;
    memcpy(configs_, updated, sizeof(configs_));
    return true;
}

// =============================================================================
// ПРОГНОЗ
// =============================================================================
// Обновить forecast_: цены сегодня–завтра + вчерашние (для плана ночи, начавшейся
// вчера, и графика «Вчера»). При неудаче остается прежний ряд (D-018).
// Запросы идут в фоне, результат — в onForecastResult/onYesterdayResult.
void Controller::startForecast() {
    if (!clock_.synced()) {
        log_.println("[FORECAST] Нет синхронизированного времени; прогноз не запрошен.");
        finishForecast();
        return;
    }

    const uint32_t now = clock_.nowUtc();
    hasAttemptedForecast_ = true;
    lastForecastAttemptMs_ = clock_.uptimeMs();
    fetchYesterday_ = rigatime::localDay(now) - 1;
    fetchYesterdayStart_ = rigatime::localDayStartUtc(fetchYesterday_);
    char path[96];
    if (!elering::forecastPath(now, path, sizeof(path))) {
        log_.println("[FORECAST] Не удалось сформировать период запроса прогноза.");
        finishForecast();
        return;
    }

    log_.println("[FORECAST] Запрос цен Elering на сегодня и завтра...");
    if (!prices_.startPrices(path, fetched_)) {
        onForecastResult(false);
        return;
    }
    fetchStage_ = FetchStage::Forecast;
}

void Controller::onForecastResult(bool ok) {
    if (!ok) {
        log_.print("[FORECAST] Используется прежний прогноз, квартальных значений: ");
        log_.println(static_cast<unsigned long>(forecast_.size()));
        finishForecast();
        return;
    }
    forecast_.replaceKeepingOlder(fetchYesterdayStart_, fetched_);

    // После запуска вчерашних цен нет: один удачный запрос за сутки, при неудаче —
    // повтор на следующем обновлении прогноза
    char path[96];
    if (forecast_.size() > 0 && forecast_[0].start > fetchYesterdayStart_ &&
        yesterdayFetchedFor_ != fetchYesterday_ &&
        elering::dayPricesPath(fetchYesterday_, path, sizeof(path))) {
        log_.println("[FORECAST] Запрос вчерашних цен Elering...");
        if (prices_.startPrices(path, fetched_)) {
            fetchStage_ = FetchStage::Yesterday;
            return;
        }
    }
    logForecastSize();
    finishForecast();
}

void Controller::onYesterdayResult(bool ok) {
    if (ok) {
        forecast_.prependOlder(fetched_);
        yesterdayFetchedFor_ = fetchYesterday_;
    }
    logForecastSize();
    finishForecast();
}

void Controller::finishForecast() {
    if (!forecastOnly_) {
        finishPriceUpdate();
        return;
    }
    forecastOnly_ = false;
    fetchStage_ = FetchStage::Idle;
    // План по новому прогнозу — сразу, а не на следующей минуте
    lastEvaluatedMinute_ = 0;
    log_.println("--------------------------------------------------");
}

void Controller::serviceMissingForecast() {
    if (fetchStage_ != FetchStage::Idle) return;
    const uint32_t now = clock_.nowUtc();
    if (forecast_.indexAt(now) >= 0) return;
    if (hasAttemptedForecast_ &&
        clock_.uptimeMs() - lastForecastAttemptMs_ < kForecastRetryIntervalMs) {
        return;
    }
    log_.println("\n--------------------------------------------------");
    log_.println("[FORECAST] Прогноз не покрывает текущее время; запрос без новой цены.");
    forecastOnly_ = true;
    startForecast();
}

void Controller::logForecastSize() {
    log_.print("[FORECAST] В прогнозе квартальных значений (вчера–завтра): ");
    log_.println(static_cast<unsigned long>(forecast_.size()));
}

// =============================================================================
// РЕШЕНИЕ АВТОМАТИКИ
// =============================================================================
// Строки лога решения по трассе плана.
void Controller::logScenarioStatus(planner::ScenarioStatus status, const char* waiting,
    const char* noPlan, const char* planned) {
    switch (status) {
        case planner::ScenarioStatus::Disabled: break;
        case planner::ScenarioStatus::Waiting: log_.println(waiting); break;
        case planner::ScenarioStatus::NoPlan: log_.println(noPlan); break;
        case planner::ScenarioStatus::Planned: log_.println(planned); break;
    }
}

void Controller::logClamped(const char* scenario, uint16_t requested, uint16_t available) {
    log_.print("[WARN] ");
    log_.print(scenario);
    log_.print(": нужно 15-минутных интервалов ");
    log_.print(requested);
    log_.print(", в окне доступно ");
    log_.print(available);
    log_.println("; выбраны все доступные.");
}

void Controller::logPlanTrace(const planner::PlanTrace& trace) {
    switch (trace.holiday) {
        case planner::HolidayStatus::Selected:
            if (trace.holidayClamped) {
                logClamped("Отпуск", trace.holidayRequested, trace.holidayAvailable);
            }
            log_.println("[DECISION] Сценарий отпуска: включение только в выбранные дешевые интервалы.");
            return;
        case planner::HolidayStatus::BeforeWindow:
            log_.println("[DECISION] Отпуск: до окна 10 ч перед возвращением реле выключено.");
            return;
        case planner::HolidayStatus::Waiting:
            log_.println("[WARN] Прогноз еще не покрывает окно перед возвращением; реле выключено.");
            return;
        case planner::HolidayStatus::Inactive:
            break;
    }

    if (trace.smartClamped) {
        logClamped("Умный порог", trace.smartRequested, trace.smartAvailable);
    }
    logScenarioStatus(trace.smart,
        "[WARN] Прогноз не покрывает текущие сутки; умный порог ждет цен, остальные сценарии продолжают работать.",
        "",
        "[DECISION] Решение по сценарию «Умный порог включения».");
    logScenarioStatus(trace.wakeup,
        "",
        "[WARN] Не удалось вычислить время перед пробуждением.",
        "[DECISION] Решение по сценарию «Включение перед пробуждением».");
    if (trace.nightClamped) {
        logClamped("Включение ночью", trace.nightRequested, trace.nightAvailable);
    }
    logScenarioStatus(trace.night,
        "[WARN] Прогноз еще не покрывает ночное окно; включение ночью ждет цен.",
        "",
        "[DECISION] Решение по сценарию «Включение ночью».");
}

void Controller::expireHolidays() {
    if (!clock_.synced()) return;
    const uint32_t now = clock_.nowUtc();
    for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
        if (!planner::holidayExpired(configs_[channel], now)) continue;
        configs_[channel].holidayEnabled = false;
        if (!store_.save(configs_)) {
            log_.println("[ERROR] Не удалось сохранить завершение режима отпуска.");
        }
        log_.print("[HOLIDAY] Реле ");
        log_.print(channel + 1);
        log_.println(": дата возвращения наступила; режим отпуска завершен.");
    }
}

// Решение автоматики для канала. false — ручной режим или принудительное включение,
// решения нет. Без побочных эффектов, кроме фиксаций планировщика и лога.
// Порядок по docs/SCENARIOS.md: аварийный предел, отпуск, остальные сценарии.
// План строится и при аварийном пределе: фиксации выбора следят за окнами и настройками.
bool Controller::decide(uint8_t channel, float currentPrice, planner::Decision& decision,
    bool onlyChanges) {
    const ChannelConfig& config = configs_[channel];
    if (overridden(channel)) return false;
    LoggedPlan& logged = logged_[channel];
    uint32_t now = clock_.nowUtc();
    if (planner::ceilingExceeded(config, currentPrice)) {
        if (!onlyChanges || !logged.valid || !logged.ceiling) {
            log_.println("[DECISION] Сработал аварийный предел цены; реле выключено.");
        }
        logged.valid = true;
        logged.ceiling = true;
        planner::buildPlan(config, forecast_, now, plannerStates_[channel], plan_);
        decision = {false, planner::Reason::Ceiling};
        return true;
    }
    planner::buildPlan(config, forecast_, now, plannerStates_[channel], plan_);
    if (!onlyChanges || !logged.valid || logged.ceiling ||
        !sameTrace(logged.trace, plan_.trace)) {
        logPlanTrace(plan_.trace);
    }
    logged.valid = true;
    logged.ceiling = false;
    logged.trace = plan_.trace;
    decision = planner::decideNow(config, plan_, forecast_, now, currentPrice);
    return true;
}

void Controller::applyConfiguredControl(uint8_t channel) {
    if (!hasLatestPrice_) return;
    if (overridden(channel)) return;
    planner::Decision decision;
    if (decide(channel, latestPriceEurKwh_, decision)) {
        setOutput(channel, decision.on, causeFor(decision.reason));
    }
}

void Controller::serviceScheduledControl() {
    if (!clock_.synced() || !hasLatestPrice_) return;
    uint32_t currentMinute = clock_.nowUtc() / 60UL;
    if (currentMinute == lastEvaluatedMinute_) return;
    lastEvaluatedMinute_ = currentMinute;

    for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
        const ChannelConfig& config = configs_[channel];
        if (!config.holidayEnabled && !config.smartThresholdEnabled &&
            !config.wakeupEnabled && !config.nightPrepEnabled) {
            continue;
        }
        planner::Decision decision;
        if (decide(channel, latestPriceEurKwh_, decision, true) &&
            decision.on != outputs_.get(channel)) {
            log_.print("[SCHEDULE] Переключение реле ");
            log_.print(channel + 1);
            log_.print(" по сохраненному плану цен: ");
            log_.println(planner::reasonName(decision.reason));
            setOutput(channel, decision.on, causeFor(decision.reason));
        }
    }
}

// =============================================================================
// РАСПИСАНИЕ ЗАПРОСА ЦЕНЫ
// =============================================================================
void Controller::servicePriceSchedule() {
    expireHolidays();
    uint32_t nowEpoch = clock_.nowUtc();
    uint32_t intervalNumber = nowEpoch / kPriceIntervalSeconds;
    uint32_t secondsIntoInterval = nowEpoch % kPriceIntervalSeconds;

    if (secondsIntoInterval < 60 &&
        (!hasScheduledPriceInterval_ || intervalNumber != lastScheduledPriceInterval_)) {
        lastScheduledPriceInterval_ = intervalNumber;
        hasScheduledPriceInterval_ = true;
        awaitingNewPriceTimestamp_ = true;
        ++scheduleGeneration_;
        for (uint8_t i = 0; i < kChannelCount; ++i) {
            manual_[i] = false;
        }
        lastUpdateMs_ = clock_.uptimeMs() - kPriceRetryIntervalMs;
        log_.println("[SCHEDULE] Начинается проверка цены на границе 15-минутного интервала.");
    }

    if (awaitingNewPriceTimestamp_ && fetchStage_ == FetchStage::Idle &&
        (!hasAttemptedPriceUpdate_ ||
         clock_.uptimeMs() - lastUpdateMs_ >= kPriceRetryIntervalMs)) {
        updatePrice();
    }
    serviceMissingForecast();
    serviceScheduledControl();
}

// =============================================================================
// ОБНОВЛЕНИЕ ЦЕНЫ И ПРИНЯТИЕ РЕШЕНИЯ ДЛЯ КАНАЛОВ
// =============================================================================
void Controller::updatePrice() {
    if (fetchStage_ != FetchStage::Idle) {
        log_.println("[TASK] Запрос цены уже идет; решение будет принято по его результату.");
        return;
    }
    lastUpdateMs_ = clock_.uptimeMs();
    hasAttemptedPriceUpdate_ = true;
    requestGeneration_ = scheduleGeneration_;
    log_.println("\n--------------------------------------------------");
    log_.println("[TASK] Запрос котировок NordPool (Латвия)...");
    if (!prices_.startCurrent()) {
        onCurrentResult(false, 0.0f, 0);
        return;
    }
    fetchStage_ = FetchStage::Current;
}

void Controller::servicePriceFetch() {
    if (fetchStage_ == FetchStage::Idle) return;
    float price = 0.0f;
    long timestamp = 0;
    const FetchStatus status = prices_.poll(price, timestamp);
    if (status == FetchStatus::Busy) return;
    // Idle на месте ожидаемого результата — результат потерян: считаем неудачей,
    // иначе запрос цены не повторится никогда
    const bool ok = status == FetchStatus::Done;
    switch (fetchStage_) {
        case FetchStage::Current: onCurrentResult(ok, price, timestamp); break;
        case FetchStage::Forecast: onForecastResult(ok); break;
        case FetchStage::Yesterday: onYesterdayResult(ok); break;
        case FetchStage::Idle: break;
    }
}

// Актуальность проверяется по часам в момент результата, как до шага 2g проверял
// источник; без синхронизации — не проверяется.
void Controller::onCurrentResult(bool ok, float price, long timestamp) {
    fetchStage_ = FetchStage::Idle;
    if (ok && clock_.synced()) {
        const uint32_t now = clock_.nowUtc();
        if (!elering::priceIsCurrent(timestamp, now)) {
            log_.print("[ERROR] Цена не относится к текущему времени (timestamp ");
            log_.print(timestamp);
            log_.print(", сейчас ");
            log_.print(static_cast<unsigned long>(now));
            log_.println(").");
            ok = false;
        }
    }
    if (!ok) {
        awaitingNewPriceTimestamp_ = true;
        log_.println("[WARN] Не удалось получить цену. Реле сохраняет предыдущее состояние.");
        log_.println("[TASK] Повторный запрос через 30 секунд.");
        log_.println("--------------------------------------------------");
        return;
    }

    log_.print("[NORDPOOL-LV] Цена из API: ");
    log_.print(price, 5);
    log_.print(" EUR/kWh, timestamp: ");
    log_.println(timestamp);

    if (timestamp <= latestPriceTimestamp_) {
        awaitingNewPriceTimestamp_ = true;
        log_.print("[WARN] API пока возвращает устаревшую цену (timestamp ");
        log_.print(timestamp);
        log_.print(", ожидается значение больше ");
        log_.print(latestPriceTimestamp_);
        log_.println("). Повторный запрос через 30 секунд; реле не изменяется.");
        log_.println("--------------------------------------------------");
        return;
    }

    pendingPriceEurKwh_ = price;
    pendingPriceTimestamp_ = timestamp;
    // При неудаче прогноза решения строятся по прежнему (D-018)
    startForecast();
}

void Controller::finishPriceUpdate() {
    fetchStage_ = FetchStage::Idle;
    hasLatestPrice_ = true;
    latestPriceEurKwh_ = pendingPriceEurKwh_;
    latestPriceTimestamp_ = pendingPriceTimestamp_;
    latestPriceReceivedMs_ = clock_.uptimeMs();
    // Граница 15 минут во время запроса: цена — прошлого интервала, ждем новую
    awaitingNewPriceTimestamp_ = scheduleGeneration_ != requestGeneration_;

    const float currentPrice = latestPriceEurKwh_;
    for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
        if (manual_[channel]) {
            log_.print("[DECISION] Реле ");
            log_.print(channel + 1);
            log_.println(": активен ручной режим, остальные условия не применяются.");
            continue;
        }
        if (forceOn_[channel].active()) {
            log_.print("[DECISION] Реле ");
            log_.print(channel + 1);
            log_.println(": идет принудительное включение, цена не учитывается.");
            continue;
        }
        planner::Decision decision = {outputs_.get(channel), planner::Reason::None};
        decide(channel, currentPrice, decision);
        log_.print("[DECISION] Цена ");
        log_.print(currentPrice, 5);
        log_.print(" EUR/kWh, реле ");
        log_.print(channel + 1);
        log_.print(decision.on ? " ВКЛЮЧАЕТСЯ: " : " ВЫКЛЮЧАЕТСЯ: ");
        log_.println(planner::reasonName(decision.reason));
        setOutput(channel, decision.on, causeFor(decision.reason));
    }
    log_.println("--------------------------------------------------");
}

// =============================================================================
// АВАРИЙНОЕ ОТКЛЮЧЕНИЕ ПРИ ОТСУТСТВИИ ЦЕНЫ
// =============================================================================
// Каналы в ручном режиме и при принудительном включении не трогаем: это явное
// решение пользователя, а принудительное включение ограничено своим таймером (D-007).
void Controller::serviceFailsafe() {
    if (!hasLatestPrice_ ||
        clock_.uptimeMs() - latestPriceReceivedMs_ < kPriceFailsafeTimeoutMs) {
        return;
    }
    hasLatestPrice_ = false;
    awaitingNewPriceTimestamp_ = true;
    log_.println("[FAILSAFE] Новой цены нет больше часа; автоматические реле выключаются.");
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (!overridden(i) && outputs_.get(i)) {
            setOutput(i, false, history::Cause::Failsafe);
        }
    }
}

// =============================================================================
// РУЧНОЙ РЕЖИМ И ПРИНУДИТЕЛЬНОЕ ВКЛЮЧЕНИЕ (docs/SCENARIOS.md §6, §7, D-006, D-007)
// =============================================================================
void Controller::setManual(uint8_t channel, bool on, const char* announce) {
    cancelForceOnByManual(channel);
    manual_[channel] = true;
    if (announce != nullptr) {
        log_.println(announce);
    }
    setOutput(channel, on, history::Cause::Manual);
}

void Controller::clearManual(uint8_t channel) {
    manual_[channel] = false;
}

void Controller::printForceOnPrefix(uint8_t channel) {
    log_.print("[FORCE] Реле ");
    log_.print(channel + 1);
    log_.print(": ");
}

// После принудительного включения решение принимается сразу, по последней цене и уже
// загруженному прогнозу, без сетевых запросов: работает и без Wi-Fi. Без актуальной
// цены канал выключается, как при failsafe.
void Controller::returnToAutomation(uint8_t channel) {
    if (!hasLatestPrice_ || !clock_.synced()) {
        printForceOnPrefix(channel);
        log_.println("актуальной цены нет; реле выключается до решения автоматики.");
        setOutput(channel, false, history::Cause::NoPrice);
        return;
    }
    planner::Decision decision;
    if (decide(channel, latestPriceEurKwh_, decision)) {
        printForceOnPrefix(channel);
        log_.print(decision.on ? "автоматика оставляет реле включенным: "
                               : "автоматика выключает реле: ");
        log_.println(planner::reasonName(decision.reason));
        setOutput(channel, decision.on, causeFor(decision.reason));
    }
}

void Controller::startForceOn(uint8_t channel) {
    const bool restarted = forceOn_[channel].active();
    const uint8_t hours = configs_[channel].forceOnHours;
    manual_[channel] = false;
    forceOn_[channel].start(clock_.uptimeMs(), hours);
    printForceOnPrefix(channel);
    log_.print(restarted ? "принудительное включение перезапущено на "
                         : "принудительное включение на ");
    log_.print(hours);
    log_.println(" ч, цена не учитывается.");
    setOutput(channel, true, history::Cause::ForceOn);
}

void Controller::stopForceOn(uint8_t channel) {
    if (!forceOn_[channel].active()) {
        printForceOnPrefix(channel);
        log_.println("принудительное включение не идет.");
        return;
    }
    forceOn_[channel].stop();
    printForceOnPrefix(channel);
    log_.println("принудительное включение остановлено пользователем.");
    returnToAutomation(channel);
}

// Ручное вкл/выкл отменяет принудительное включение: последняя команда побеждает.
void Controller::cancelForceOnByManual(uint8_t channel) {
    if (!forceOn_[channel].active()) return;
    forceOn_[channel].stop();
    printForceOnPrefix(channel);
    log_.println("принудительное включение отменено ручным режимом.");
}

// Таймер истекает и без сети, и в режиме точки настройки.
void Controller::serviceForceOn() {
    const uint32_t nowMs = clock_.uptimeMs();
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (forceOn_[i].expire(nowMs)) {
            printForceOnPrefix(i);
            log_.println("время принудительного включения вышло.");
            returnToAutomation(i);
        }
    }
}

}  // namespace controller
