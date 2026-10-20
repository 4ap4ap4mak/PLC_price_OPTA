// Планировщик сценариев канала: один план на весь горизонт прогноза по 15-минутным
// интервалам. Из плана берутся и решение «включить сейчас», и полосы на графике.
// Чистый модуль L0: без Arduino, не логирует, время — параметр `now` (epoch UTC).
//
// Поведение — docs/SCENARIOS.md, решения пользователя — docs/DECISIONS.md (D-012…D-019).
// Коротко:
//   * умный порог, ночь и отпуск выбирают N·4 самых дешевых интервалов своего окна;
//   * выбор делается по всему окну, когда прогноз покрывает окно целиком, и дальше
//     не пересчитывается (PlannerState);
//   * смена настроек посреди окна засчитывает уже выбранные прошедшие интервалы и
//     добирает недостающие из оставшихся;
//   * ночь и отпуск переносят выбор к концу окна, если позже дороже не больше чем на
//     kShiftToleranceEurKwh.
//
// Ручной режим, принудительное включение и побочные эффекты (выключение отпуска
// с записью во flash) остаются у вызывающего: см. holidayExpired().
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ChannelConfig.h>
#include <PriceSeries.h>

namespace planner {

using channelconfig::ChannelConfig;
using prices::kMaxSlots;
using prices::kSlotSeconds;
using prices::PriceSeries;

// Ночь и отпуск: интервал переносится ближе к концу окна, если тот дороже исходного
// не больше чем на 1 цент/кВт·ч (D-016).
constexpr float kShiftToleranceEurKwh = 0.01f;
// Отпуск выбирает интервалы только из последних 10 часов перед возвращением (D-015).
constexpr uint32_t kHolidayWindowSeconds = 10UL * 3600UL;
// Самое длинное окно — сутки с переходом на зимнее время: 25 ч = 100 интервалов.
constexpr size_t kMaxWindowSlots = 128;

// Окно сценария [start, end), epoch UTC. В окно входят только интервалы целиком.
struct Window {
    uint32_t start;
    uint32_t end;
};

// Зафиксированный выбор интервалов для текущего окна одного сценария.
struct SlotSelection {
    bool tracked;          // запись заполнена; после загрузки false
    bool enabled;          // параметры сценария, при которых сделан выбор
    uint8_t hours;
    uint32_t param;        // ночь: начало<<16 | конец; отпуск: момент возвращения
    uint32_t windowStart;  // окно, к которому относится выбор
    uint32_t windowEnd;
    uint32_t earliest;     // кандидаты — интервалы не раньше этого момента
    bool built;            // выбор сделан: прогноз покрыл окно
    uint16_t requested;    // сколько интервалов искали при выборе (для лога)
    uint16_t available;    // сколько кандидатов было
    uint32_t bits[kMaxWindowSlots / 32];  // бит i → интервал firstSlot + i·15 мин выбран
};

// Состояние планировщика одного канала. Живет в RAM; нулевое значение — «после загрузки».
struct PlannerState {
    SlotSelection smart;
    SlotSelection night;
    SlotSelection holiday;
};

// Биты маски интервала в плане.
namespace slot {
constexpr uint8_t kThreshold = 1U << 0;      // цена интервала не выше порога
constexpr uint8_t kSmart = 1U << 1;
constexpr uint8_t kWakeup = 1U << 2;
constexpr uint8_t kNight = 1U << 3;
constexpr uint8_t kHoliday = 1U << 5;        // выбран отпуском
constexpr uint8_t kHolidayPeriod = 1U << 6;  // до возвращения: решает только отпуск
constexpr uint8_t kCeiling = 1U << 7;        // цена интервала выше аварийного предела
}  // namespace slot

enum class Reason : uint8_t {
    None,        // ни один сценарий не выбрал интервал
    Threshold,
    Smart,
    Wakeup,
    Night,
    Holiday,
    HolidayOff,  // отпуск: интервал не выбран, остальные сценарии не действуют
    Ceiling      // цена выше аварийного предела
};

// Состояние сценария для лога.
enum class ScenarioStatus : uint8_t {
    Disabled,  // выключен в настройках
    Waiting,   // прогноз еще не покрывает окно
    NoPlan,    // план построить не удалось (пробуждение)
    Planned    // выбор сделан
};
enum class HolidayStatus : uint8_t {
    Inactive,      // отпуск выключен или дата возвращения прошла
    BeforeWindow,  // до окна перед возвращением: реле выключено
    Waiting,       // окно идет, но прогноз его еще не покрывает: реле выключено
    Selected       // окно идет, решение по выбранным интервалам
};

struct PlanTrace {
    HolidayStatus holiday;
    ScenarioStatus smart;
    ScenarioStatus wakeup;
    ScenarioStatus night;
    // Выбрано меньше, чем нужно: в окне не хватило интервалов. Счет в интервалах.
    bool smartClamped, nightClamped, holidayClamped;
    uint16_t smartRequested, smartAvailable;
    uint16_t nightRequested, nightAvailable;
    uint16_t holidayRequested, holidayAvailable;
};

// План канала на весь прогноз: mask[i] относится к forecast[i].
struct ChannelPlan {
    size_t slotCount;
    uint8_t mask[kMaxSlots];
    PlanTrace trace;
};

struct Decision {
    bool on;
    Reason reason;
};

// Аварийный предел по текущей цене.
inline bool ceilingExceeded(const ChannelConfig& config, float currentPrice) {
    return currentPrice > config.safetyCeilingEurKwh;
}
// Дата возвращения наступила. Вызывающий выключает отпуск в конфиге и сохраняет его
// до buildPlan().
inline bool holidayExpired(const ChannelConfig& config, uint32_t now) {
    return config.holidayEnabled && config.holidayReturnEpoch <= now;
}

// Построить план канала по прогнозу. Обновляет фиксации state.
// Вызывать перед каждым решением и перед рисованием графика.
void buildPlan(const ChannelConfig& config, const PriceSeries& forecast, uint32_t now,
    PlannerState& state, ChannelPlan& plan);

// Решение для текущего момента. plan построен по этому же forecast.
// Аварийный предел и порог проверяются по текущей цене (/current), пробуждение — с
// точностью до минуты, остальное — по интервалу плана, в который попадает now.
Decision decideNow(const ChannelConfig& config, const ChannelPlan& plan,
    const PriceSeries& forecast, uint32_t now, float currentPrice);

// Итог интервала плана по маске: включен ли и почему.
bool slotOn(uint8_t mask);
Reason slotReason(uint8_t mask);
const char* reasonName(Reason reason);

// --- Окна и выбор (открыты для тестов) --------------------------------------

// Окно ночи, в котором лежит now, или ближайшее следующее. false — начало = конец.
bool activeNightWindow(const ChannelConfig& config, uint32_t now, Window& window);
// Окно ночи, которое начинается в местные сутки day.
Window nightWindowOfDay(const ChannelConfig& config, int64_t day);
// Местные сутки, в которых лежит now.
Window localDayWindow(uint32_t now);
// Последние kHolidayWindowSeconds перед возвращением.
Window holidayWindow(const ChannelConfig& config);
// Прогноз покрывает окно: есть цены до последнего интервала окна включительно.
bool covers(const PriceSeries& forecast, Window window);

// Выбрать wanted самых дешевых интервалов прогноза внутри окна, не раньше earliest.
// При равной цене — более ранний. shiftToEnd — перенос к концу окна (ночь, отпуск).
// chosen[i] (емкость kMaxSlots) — интервал forecast[i] выбран; остальное не трогается.
// Возвращает число выбранных, available — число кандидатов.
size_t selectCheapest(const PriceSeries& forecast, Window window, uint32_t earliest,
    size_t wanted, bool shiftToEnd, bool* chosen, size_t& available);

// Ближайшее время пробуждения строго после now. false — не нашлось за 3 суток.
bool getNextWakeupEpoch(uint16_t wakeupMinute, uint32_t now, uint32_t& wakeupEpoch);

// Название режима канала для отчета в консоли.
struct ModeInputs {
    bool manual;
    bool forceOn;            // идет принудительное включение
    bool hasLatestPrice;
    bool hadAnyPrice;        // цена хоть раз была получена (timestamp != 0)
    float latestPriceEurKwh;
    bool timeSynced;
    uint32_t now;
};
const char* modeName(const ChannelConfig& config, const ModeInputs& in);

}  // namespace planner
