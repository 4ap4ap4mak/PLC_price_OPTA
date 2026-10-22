// Контроллер каналов (L2): единственный владелец состояния каналов — настройки,
// ручной режим, принудительное включение, последняя цена, прогноз, фиксации
// планировщика. Ведет расписание запроса цены на границах 15 минут, failsafe,
// решения автоматики и команды пользователя (веб, консоль, кнопка).
//
// Шаг 2e: перенос из main.cpp без изменения поведения, логи те же. Модуль лежит
// в lib/, а не в src/app/, чтобы тесты env:native проверяли failsafe, расписание и
// таймер принудительного включения с фейковыми портами (ControllerPorts.h).
// Поведение — docs/SCENARIOS.md §1, §2, §5–§7.
//
// Не потокобезопасен: все вызовы из основного потока (loop()). С шага 2g не блокирует:
// цены запрашиваются в фоне через PriceSource, этапы запроса ведет servicePriceFetch()
// (D-008, D-026).
#pragma once

#include <stdint.h>

#include <ChannelConfig.h>
#include <ConfigStore.h>
#include <ForceOnTimer.h>
#include <Planner.h>
#include <PriceSeries.h>
#include <SwitchHistory.h>

#include "ControllerPorts.h"

namespace controller {

using channelconfig::ChannelConfig;
using channelconfig::kChannelCount;

// Цена меняется каждые 15 минут: на границе интервала запрашивается новая.
constexpr uint32_t kPriceIntervalSeconds = 15UL * 60UL;
// Повтор запроса, пока API не отдаст новую цену.
constexpr uint32_t kPriceRetryIntervalMs = 30UL * 1000UL;
// Без новой цены дольше этого времени автоматические каналы выключаются (D-001).
constexpr uint32_t kPriceFailsafeTimeoutMs = 60UL * 60UL * 1000UL;
// Прогноз не покрывает текущее время: повтор запроса прогноза без новой цены (D-028).
constexpr uint32_t kForecastRetryIntervalMs = 60UL * 1000UL;

class Controller {
public:
    Controller(Clock& clock, Outputs& outputs, configstore::ConfigStore& store,
        PriceSource& prices, history::SwitchHistory& history, LogSink& log);

    // Принять загруженные настройки и выключить все выходы (причина — запуск).
    // Выходы уже инициализированы, событие запуска в истории записано вызывающим.
    void begin(const ChannelConfig loaded[kChannelCount]);

    // --- Сервисы loop() ---------------------------------------------------------
    // Вызывать в начале каждого loop(), до любых ранних return: работают и без сети,
    // и в режиме точки настройки.
    void serviceFailsafe();
    void serviceForceOn();
    // Забрать результат фонового запроса и перейти к следующему этапу: текущая цена и
    // решение по всем каналам → прогноз сегодня–завтра → вчерашние цены → пересчет
    // плана (D-030).
    void servicePriceFetch();
    // Часть расписания без сети (D-035): истечение отпуска, граница 15 минут (начало
    // ожидания новой цены), пересчет плана раз в минуту по последней цене и уже
    // загруженному прогнозу. Вызывать при синхронизированных часах, в том числе без
    // Wi-Fi и в режиме точки настройки.
    void serviceSchedule();
    // Запросы в сеть: повтор цены раз в 30 с, пока ждем новую, и прогноз, если он не
    // покрывает текущее время (раз в минуту, D-028). Вызывать после serviceSchedule()
    // и только при связи с интернетом.
    void servicePriceRequests();

    // --- Команды -------------------------------------------------------------------
    // Внеочередной запрос цены. Только ставит фоновый запрос: решение по всем каналам
    // принимается, когда придет цена (servicePriceFetch). Если запрос уже идет, второй
    // не ставится — решение будет по результату идущего.
    void updatePrice();
    // Ручной режим — рычаг (D-034): действует, пока пользователь сам не вернет канал
    // автоматике; главнее всех остальных включений и выключений, отменяет
    // принудительное включение. announce — строка лога источника команды: печатается
    // после отмены принудительного включения и перед переключением выхода.
    void setManual(uint8_t channel, bool on, const char* announce = nullptr);
    // Опустить рычаг: канал сразу возвращается к автоматике — решение по последней цене
    // и загруженному прогнозу, без цены — выключение (как после принудительного
    // включения). Без ручного режима — только строка лога.
    void releaseManual(uint8_t channel);
    // Включить на forceOnHours или перезапустить отсчет. В ручном режиме отклоняется
    // (D-034): false, выход не меняется.
    bool startForceOn(uint8_t channel);
    // «Остановить»: канал возвращается к автоматике, а не просто выключается.
    void stopForceOn(uint8_t channel);
    // Записать во flash настройки всех каналов с новой конфигурацией channel.
    // false — запись не удалась или заблокирована (configWriteLocked()), настройки
    // в RAM прежние.
    bool saveConfig(uint8_t channel, const ChannelConfig& config);
    // При запуске сохраненные настройки не прочитались: запись во flash заблокирована,
    // каналы работают на значениях по умолчанию (D-037).
    bool configWriteLocked() const { return store_.writeLocked(); }
    // Команда config reset: записать текущие настройки из RAM поверх нечитаемых и снять
    // блокировку. false — запись не удалась.
    bool overwriteStoredConfig();
    // После сохранения настроек: решение для канала по последней цене и уже
    // загруженному прогнозу, без сетевых запросов (KI-011).
    void applyConfiguredControl(uint8_t channel);

    // --- Чтение ----------------------------------------------------------------------
    const ChannelConfig* configs() const { return configs_; }
    const ChannelConfig& config(uint8_t channel) const { return configs_[channel]; }
    bool outputOn(uint8_t channel) const { return outputs_.get(channel); }
    bool manual(uint8_t channel) const { return manual_[channel]; }
    bool forceOnActive(uint8_t channel) const { return forceOn_[channel].active(); }
    // Принудительное включение идет хотя бы на одном канале: перезагрузку из-за
    // зависшего сетевого запроса main.cpp откладывает до его конца (D-026, п. 6).
    bool anyForceOnActive() const;
    uint32_t forceOnRemainingMs(uint8_t channel) const;
    // Название режима канала для страницы и консоли.
    const char* modeName(uint8_t channel) const;

    bool hasLatestPrice() const { return hasLatestPrice_; }
    // Цена хоть раз была получена (после failsafe hasLatestPrice() уже false).
    bool hadAnyPrice() const { return latestPriceTimestamp_ != 0; }
    float latestPriceEurKwh() const { return latestPriceEurKwh_; }
    const prices::PriceSeries& forecast() const { return forecast_; }
    // Идет ожидание новой цены с повтором раз в 30 с; remainingSeconds — до повтора.
    bool priceRetryPending(uint32_t& remainingSeconds) const;
    // Идет фоновый запрос цены или прогноза.
    bool priceFetchInProgress() const { return fetchStage_ != FetchStage::Idle; }
    // План канала для графика на момент now (0 — часы не синхронизированы, план пуст).
    // Обновляет фиксации канала, как и решение (KI-005). Ссылка действительна до
    // следующего решения или графика: рабочий план один на все каналы.
    const planner::ChannelPlan& chartPlan(uint8_t channel, uint32_t now);

private:
    uint32_t epochOrZero() const;
    // Выход пишется всегда; в историю попадает только смена состояния с причиной.
    void setOutput(uint8_t channel, bool on, history::Cause cause);
    // Команда пользователя поверх автоматики: ручной режим или принудительное
    // включение. Такие каналы не трогают ни решения по цене, ни failsafe.
    bool overridden(uint8_t channel) const;
    // Этапы фонового запроса цены.
    enum class FetchStage : uint8_t { Idle, Current, Forecast, Yesterday };
    void onCurrentResult(bool ok, float price, long timestamp);
    // Запросить прогноз сегодня–завтра; без часов или пути — сразу к решению.
    void startForecast();
    // Прогноз без новой цены (D-028): после запуска цена могла прийти раньше часов,
    // или прошлый запрос прогноза не удался.
    void serviceMissingForecast();
    // Конец запроса прогноза: немедленный пересчет плана, а не на следующей минуте.
    void finishForecast();
    void onForecastResult(bool ok);
    void onYesterdayResult(bool ok);
    void logForecastSize();
    // Принять новую цену и решить по всем каналам по уже загруженному прогнозу.
    void acceptPrice(float price, long timestamp);
    // Наступила дата возвращения: снять отпуск и записать flash (KI-013). Решение
    // от этого не зависит: планировщик сам не учитывает прошедший отпуск.
    void expireHolidays();
    // onlyChanges — строки плана печатаются, только если они изменились с прошлого
    // раза (пересчет раз в минуту, KI-013).
    bool decide(uint8_t channel, float currentPrice, planner::Decision& decision,
        bool onlyChanges = false);
    void serviceScheduledControl();
    // Решение автоматики сразу после команды пользователя; tag — метка строк лога.
    void returnToAutomation(uint8_t channel, const char* tag);
    void cancelForceOnByManual(uint8_t channel);
    void printChannelPrefix(const char* tag, uint8_t channel);
    void printForceOnPrefix(uint8_t channel) { printChannelPrefix("FORCE", channel); }
    void logScenarioStatus(planner::ScenarioStatus status, const char* waiting,
        const char* noPlan, const char* planned);
    void logClamped(const char* scenario, uint16_t requested, uint16_t available);
    void logPlanTrace(const planner::PlanTrace& trace);

    Clock& clock_;
    Outputs& outputs_;
    configstore::ConfigStore& store_;
    PriceSource& prices_;
    history::SwitchHistory& history_;
    LogSink& log_;

    ChannelConfig configs_[kChannelCount] = {};
    bool manual_[kChannelCount] = {};
    // Принудительное включение (SCENARIOS §6). После перезагрузки пусто (D-007).
    forceon::ForceOnTimer forceOn_[kChannelCount];

    // Цены вчера–завтра. При неудачном запросе сохраняется прежний ряд (D-018).
    prices::PriceSeries forecast_;
    // Буфер разбора свежего ответа: в forecast_ попадает только удачный. Пока идет
    // запрос цен, буфер заполняет PriceSource (на устройстве — другой поток).
    prices::PriceSeries fetched_;
    // Сутки, за которые вчерашние цены уже догружены.
    int64_t yesterdayFetchedFor_ = INT64_MIN;
    // Фиксации выбора интервалов (D-012) и рабочий план: строится перед каждым
    // решением и перед графиком, поэтому один на все каналы.
    planner::PlannerState plannerStates_[kChannelCount] = {};
    planner::ChannelPlan plan_ = {};
    // Что последний раз напечатано о плане канала: для лога только при изменении.
    struct LoggedPlan {
        bool valid;
        bool ceiling;
        planner::PlanTrace trace;
    };
    LoggedPlan logged_[kChannelCount] = {};

    // Последняя цена. Цена может быть отрицательной, поэтому наличие — отдельный флаг.
    bool hasLatestPrice_ = false;
    float latestPriceEurKwh_ = 0.0f;
    long latestPriceTimestamp_ = 0;
    uint32_t latestPriceReceivedMs_ = 0;

    // Фоновый запрос: этап, вчерашние сутки на момент запроса прогноза.
    FetchStage fetchStage_ = FetchStage::Idle;
    int64_t fetchYesterday_ = 0;
    uint32_t fetchYesterdayStart_ = 0;
    bool hasAttemptedForecast_ = false;
    uint32_t lastForecastAttemptMs_ = 0;
    // Номер границы 15 минут: если граница наступила, пока шел запрос, полученная
    // цена относится к прошлому интервалу и ожидание новой продолжается.
    uint32_t scheduleGeneration_ = 0;
    uint32_t requestGeneration_ = 0;

    uint32_t lastUpdateMs_ = 0;
    bool hasAttemptedPriceUpdate_ = false;
    // true с самого старта: первая цена запрашивается сразу после синхронизации времени
    bool awaitingNewPriceTimestamp_ = true;
    uint32_t lastScheduledPriceInterval_ = 0;
    bool hasScheduledPriceInterval_ = false;
    uint32_t lastEvaluatedMinute_ = 0;
};

}  // namespace controller
