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
// Не потокобезопасен: все вызовы из loop(). Блокирует на время запросов PriceSource
// (до шага 2g).
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
    // Истечение отпуска, граница 15 минут, повтор раз в 30 с, пересчет плана раз
    // в минуту. Вызывать только при синхронизированных часах.
    void servicePriceSchedule();

    // --- Команды -------------------------------------------------------------------
    // Внеочередной запрос цены и решение по всем каналам.
    void updatePrice();
    // Ручной режим до ближайшей границы 15 минут; отменяет принудительное включение.
    // announce — строка лога источника команды: печатается после отмены
    // принудительного включения и перед переключением выхода.
    void setManual(uint8_t channel, bool on, const char* announce = nullptr);
    void clearManual(uint8_t channel);
    // Включить на forceOnHours или перезапустить отсчет. Отменяет ручной режим (D-007).
    void startForceOn(uint8_t channel);
    // «Остановить»: канал возвращается к автоматике, а не просто выключается.
    void stopForceOn(uint8_t channel);
    // Записать во flash настройки всех каналов с новой конфигурацией channel.
    // false — запись не удалась, настройки в RAM прежние.
    bool saveConfig(uint8_t channel, const ChannelConfig& config);
    // После сохранения настроек: решение для канала по последней цене и уже
    // загруженному прогнозу, без сетевых запросов (KI-011).
    void applyConfiguredControl(uint8_t channel);

    // --- Чтение ----------------------------------------------------------------------
    const ChannelConfig* configs() const { return configs_; }
    const ChannelConfig& config(uint8_t channel) const { return configs_[channel]; }
    bool outputOn(uint8_t channel) const { return outputs_.get(channel); }
    bool manual(uint8_t channel) const { return manual_[channel]; }
    bool forceOnActive(uint8_t channel) const { return forceOn_[channel].active(); }
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
    bool fetchForecast();
    // Наступила дата возвращения: снять отпуск и записать flash (KI-013). Решение
    // от этого не зависит: планировщик сам не учитывает прошедший отпуск.
    void expireHolidays();
    // onlyChanges — строки плана печатаются, только если они изменились с прошлого
    // раза (пересчет раз в минуту, KI-013).
    bool decide(uint8_t channel, float currentPrice, planner::Decision& decision,
        bool onlyChanges = false);
    void serviceScheduledControl();
    void returnToAutomation(uint8_t channel);
    void cancelForceOnByManual(uint8_t channel);
    void printForceOnPrefix(uint8_t channel);
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
    // Буфер разбора свежего ответа: в forecast_ попадает только удачный.
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

    uint32_t lastUpdateMs_ = 0;
    bool hasAttemptedPriceUpdate_ = false;
    // true с самого старта: первая цена запрашивается сразу после синхронизации времени
    bool awaitingNewPriceTimestamp_ = true;
    uint32_t lastScheduledPriceInterval_ = 0;
    bool hasScheduledPriceInterval_ = false;
    uint32_t lastEvaluatedMinute_ = 0;
};

}  // namespace controller
