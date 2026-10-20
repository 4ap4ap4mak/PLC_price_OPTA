// Планировщик сценариев канала: решение «включить сейчас» и полосы работы на графике.
// Чистый модуль L0: без Arduino, не логирует, время — параметр `now` (epoch UTC).
//
// Шаг 2a: логика перенесена из монолита КАК ЕСТЬ, вместе с известными багами
// (KI-002, KI-003, KI-005 в docs/KNOWN_ISSUES.md). Решение и график пока считаются
// двумя разными функциями и могут расходиться. Исправления — шаг 2b, только с тестами.
//
// Ручной режим и побочные эффекты (выключение отпуска с записью во flash) остаются
// у вызывающего: см. holidayExpired() и порядок вызова в описании decideScenarios().
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ChannelConfig.h>
#include <PriceSeries.h>

namespace planner {

using channelconfig::ChannelConfig;
using prices::PriceHour;
using prices::PriceSeries;

// Зафиксированный «непрерывный блок» канала: выбранное окно не пересчитывается,
// пока не закончится. {0, 0} — блок не выбран.
struct BlockLock {
    uint32_t start;
    uint32_t end;
};

// Ключ ночного окна для часа: номер местных суток начала окна или -1, если час вне окна.
int64_t nightWindowKey(uint32_t timestamp, const ChannelConfig& config);

// Отдельные сценарии. true — план построен, selectedNow — текущий час выбран.
bool chooseCheapestNightHours(const ChannelConfig& config, const PriceHour* hours, size_t count,
    uint32_t now, bool& selectedNow);
// requestedHours/availableHours заполняются, когда запрошено больше часов, чем есть
// полных часов на сегодня (тогда выбираются все доступные).
bool chooseCheapestDayHours(const ChannelConfig& config, const PriceHour* hours, size_t count,
    uint32_t now, bool& selectedNow, bool& clamped, size_t& requestedHours, size_t& availableHours);
bool getNextWakeupEpoch(uint16_t wakeupMinute, uint32_t now, uint32_t& wakeupEpoch);
bool isWakeupOnTime(const ChannelConfig& config, uint32_t now, bool& selectedNow);
bool chooseConsecutiveHours(const ChannelConfig& config, const PriceHour* hours, size_t count,
    uint32_t now, BlockLock& lock, bool& selectedNow);
bool chooseHolidayHours(const ChannelConfig& config, const PriceHour* hours, size_t count,
    uint32_t now, bool& selectedNow);

// Шаг 1 решения: цена выше аварийного предела → выключить, остальное не проверять.
inline bool ceilingExceeded(const ChannelConfig& config, float currentPrice) {
    return currentPrice > config.safetyCeilingEurKwh;
}
// Шаг 2: дата возвращения наступила. Вызывающий выключает отпуск в конфиге,
// сохраняет его и только потом вызывает decideScenarios().
inline bool holidayExpired(const ChannelConfig& config, uint32_t now) {
    return config.holidayEnabled && config.holidayReturnEpoch <= now;
}

// Исход сценария для лога: так монолит сообщал о каждом сценарии.
enum class ScenarioStatus : uint8_t {
    Disabled,    // сценарий выключен в настройках
    NoForecast,  // прогноза нет
    NoPlan,      // прогноз есть, но план не построен
    Planned      // план построен (выбран ли текущий час — в решении)
};
enum class HolidayStatus : uint8_t {
    Inactive,    // отпуск выключен
    Selected,    // план построен, решение по выбранным часам
    NotCovered,  // прогноз не покрывает период до возвращения → выключено
    NoForecast   // прогноза нет → выключено
};

struct DecisionTrace {
    HolidayStatus holiday;
    ScenarioStatus smart;
    bool smartClamped;
    size_t smartRequestedHours;
    size_t smartAvailableHours;
    ScenarioStatus wakeup;   // NoPlan — не удалось вычислить время пробуждения
    ScenarioStatus night;
    ScenarioStatus block;
};

// Шаг 3: отпуск, затем порог ИЛИ умный порог ИЛИ пробуждение ИЛИ ночь ИЛИ блок.
// Вызывать, только если канал не в ручном режиме, ceilingExceeded() == false
// и истекший отпуск уже выключен. Возвращает, включить ли канал.
bool decideScenarios(const ChannelConfig& config, float currentPrice, bool forecastAvailable,
    const PriceSeries& forecast, uint32_t now, BlockLock& lock, DecisionTrace& trace);

// Полосы работы канала на графике для местных суток chartLocalDay: workSlots[i]
// соответствует forecast[i]. При пустом прогнозе массив не трогается.
// manual/relayOn — ручной режим и текущее состояние выхода.
void buildChartSlots(const ChannelConfig& config, const PriceSeries& forecast,
    bool timeSynced, uint32_t now, uint32_t chartLocalDay,
    bool manual, bool relayOn, const BlockLock& lock, bool* workSlots);

// Название режима канала для отчета в консоли.
struct ModeInputs {
    bool manual;
    bool hasLatestPrice;
    bool hadAnyPrice;        // цена хоть раз была получена (timestamp != 0)
    float latestPriceEurKwh;
    bool timeSynced;
    uint32_t now;
};
const char* modeName(const ChannelConfig& config, const ModeInputs& in);

}  // namespace planner
