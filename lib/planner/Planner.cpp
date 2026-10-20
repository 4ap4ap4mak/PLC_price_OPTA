#include "Planner.h"

#include <math.h>
#include <string.h>

#include <RigaTime.h>

namespace planner {

namespace {

// Цены приходят как EUR/MWh / 1000 во float: допуск сравнения против ошибок округления.
constexpr float kPriceEpsilon = 1e-6f;
constexpr uint8_t kScenarioBits =
    slot::kThreshold | slot::kSmart | slot::kWakeup | slot::kNight | slot::kBlock;

uint32_t firstSlotStart(Window window) {
    return (window.start + kSlotSeconds - 1) / kSlotSeconds * kSlotSeconds;
}

uint32_t lastSlotEnd(Window window) {
    return window.end / kSlotSeconds * kSlotSeconds;
}

Window selectionWindow(const SlotSelection& selection) {
    return {selection.windowStart, selection.windowEnd};
}

void setBit(SlotSelection& selection, size_t index) {
    if (index < kMaxWindowSlots) {
        selection.bits[index / 32] |= static_cast<uint32_t>(1) << (index % 32);
    }
}

size_t countBits(const SlotSelection& selection) {
    size_t count = 0;
    for (uint32_t word : selection.bits) {
        for (; word != 0; word &= word - 1) ++count;
    }
    return count;
}

// Выбран ли интервал, начинающийся в slotStart.
bool isSelected(const SlotSelection& selection, uint32_t slotStart) {
    const uint32_t first = firstSlotStart(selectionWindow(selection));
    if (slotStart < first || slotStart + kSlotSeconds > selection.windowEnd) return false;
    const size_t index = (slotStart - first) / kSlotSeconds;
    return index < kMaxWindowSlots && ((selection.bits[index / 32] >> (index % 32)) & 1U) != 0;
}

void resetSelection(SlotSelection& selection, bool enabled, uint8_t hours, uint32_t param,
    Window window) {
    memset(&selection, 0, sizeof(selection));
    selection.tracked = true;
    selection.enabled = enabled;
    selection.hours = hours;
    selection.param = param;
    selection.windowStart = window.start;
    selection.windowEnd = window.end;
    selection.earliest = window.start;
}

// Выбор для окна: N·4 интервалов минус уже засчитанные (биты, перенесенные при смене
// настроек), из кандидатов не раньше earliest.
void buildSelection(SlotSelection& selection, const PriceSeries& forecast, bool shiftToEnd) {
    const Window window = selectionWindow(selection);
    const size_t wanted = static_cast<size_t>(selection.hours) * 4;
    const size_t worked = countBits(selection);
    const size_t remaining = wanted > worked ? wanted - worked : 0;
    bool chosen[kMaxSlots] = {};
    size_t available = 0;
    selectCheapest(forecast, window, selection.earliest, remaining, shiftToEnd, chosen,
        available);
    const uint32_t first = firstSlotStart(window);
    for (size_t i = forecast.lowerBound(first);
         i < forecast.size() && forecast[i].start + kSlotSeconds <= window.end; ++i) {
        if (chosen[i]) setBit(selection, (forecast[i].start - first) / kSlotSeconds);
    }
    selection.requested = static_cast<uint16_t>(remaining);
    selection.available = static_cast<uint16_t>(available);
    selection.built = true;
}

// Привести фиксацию к текущему окну и параметрам и, если можно, сделать выбор.
ScenarioStatus updateSelection(SlotSelection& selection, bool enabled, uint8_t hours,
    uint32_t param, Window window, uint32_t now, const PriceSeries& forecast,
    bool shiftToEnd) {
    const bool sameParams = selection.tracked && selection.enabled == enabled &&
        selection.hours == hours && selection.param == param;
    const bool sameWindow = selection.tracked && selection.windowStart == window.start &&
        selection.windowEnd == window.end;
    if (!sameParams || !sameWindow) {
        const SlotSelection previous = selection;
        resetSelection(selection, enabled, hours, param, window);
        const bool windowRunning = window.start <= now && now < window.end;
        // После загрузки и при смене окна выбор идет по всему окну (D-012). Смена
        // настроек посреди окна: прошедшие выбранные интервалы засчитываются,
        // остальное — из оставшихся, начиная с текущего интервала (D-013).
        if (previous.tracked && !sameParams && windowRunning) {
            const uint32_t nowSlot = now / kSlotSeconds * kSlotSeconds;
            if (nowSlot > selection.earliest) selection.earliest = nowSlot;
            if (previous.enabled && previous.built) {
                const uint32_t first = firstSlotStart(window);
                for (uint32_t t = first; t < nowSlot && t + kSlotSeconds <= window.end;
                     t += kSlotSeconds) {
                    if (isSelected(previous, t)) setBit(selection, (t - first) / kSlotSeconds);
                }
            }
        }
    }
    if (!enabled) return ScenarioStatus::Disabled;
    if (!selection.built) {
        if (!covers(forecast, window)) return ScenarioStatus::Waiting;
        buildSelection(selection, forecast, shiftToEnd);
    }
    return ScenarioStatus::Planned;
}

void markSelection(const SlotSelection& selection, const PriceSeries& forecast, uint8_t bit,
    ChannelPlan& plan) {
    if (!selection.enabled || !selection.built) return;
    for (size_t i = forecast.lowerBound(selection.windowStart);
         i < forecast.size() && forecast[i].start + kSlotSeconds <= selection.windowEnd; ++i) {
        if (isSelected(selection, forecast[i].start)) plan.mask[i] |= bit;
    }
}

// Окно, которое уже не текущее (вчера, завтра): выбор по всему окну без фиксации.
void markWholeWindow(const PriceSeries& forecast, Window window, uint8_t hours,
    bool shiftToEnd, uint8_t bit, ChannelPlan& plan) {
    if (!covers(forecast, window)) return;
    bool chosen[kMaxSlots] = {};
    size_t available = 0;
    selectCheapest(forecast, window, window.start, static_cast<size_t>(hours) * 4,
        shiftToEnd, chosen, available);
    for (size_t i = 0; i < forecast.size(); ++i) {
        if (chosen[i]) plan.mask[i] |= bit;
    }
}

bool clamped(const SlotSelection& selection) {
    return selection.built && selection.available < selection.requested;
}

}  // namespace

Window localDayWindow(uint32_t now) {
    const int64_t day = rigatime::localDay(now);
    return {rigatime::localDayStartUtc(day), rigatime::localDayStartUtc(day + 1)};
}

Window nightWindowOfDay(const ChannelConfig& config, int64_t day) {
    Window window = {0, 0};
    rigatime::localToUtc(day, config.nightStartMinuteOfDay, window.start);
    const int64_t endDay =
        config.nightStartMinuteOfDay < config.nightEndMinuteOfDay ? day : day + 1;
    rigatime::localToUtc(endDay, config.nightEndMinuteOfDay, window.end);
    return window;
}

bool activeNightWindow(const ChannelConfig& config, uint32_t now, Window& window) {
    if (config.nightStartMinuteOfDay == config.nightEndMinuteOfDay) return false;
    const int64_t today = rigatime::localDay(now);
    bool found = false;
    for (int64_t day = today - 1; day <= today + 1; ++day) {
        const Window candidate = nightWindowOfDay(config, day);
        if (candidate.start <= now && now < candidate.end) {
            window = candidate;
            return true;
        }
        if (candidate.start > now && (!found || candidate.start < window.start)) {
            window = candidate;
            found = true;
        }
    }
    return found;
}

Window holidayWindow(const ChannelConfig& config) {
    const uint32_t returnAt = config.holidayReturnEpoch;
    return {returnAt > kHolidayWindowSeconds ? returnAt - kHolidayWindowSeconds : 0, returnAt};
}

bool covers(const PriceSeries& forecast, Window window) {
    const uint32_t end = lastSlotEnd(window);
    return forecast.size() > 0 && end > firstSlotStart(window) && forecast.coverageEnd() >= end;
}

size_t selectCheapest(const PriceSeries& forecast, Window window, uint32_t earliest,
    size_t wanted, bool shiftToEnd, bool* chosen, size_t& available) {
    uint16_t candidates[kMaxWindowSlots];
    bool picked[kMaxWindowSlots] = {};
    size_t count = 0;
    const uint32_t from = earliest > window.start ? earliest : window.start;
    for (size_t i = forecast.lowerBound(from);
         i < forecast.size() && count < kMaxWindowSlots; ++i) {
        if (forecast[i].start + kSlotSeconds > window.end) break;
        candidates[count++] = static_cast<uint16_t>(i);
    }
    available = count;

    size_t selected = 0;
    for (; selected < wanted && selected < count; ++selected) {
        size_t best = count;
        for (size_t j = 0; j < count; ++j) {
            if (picked[j]) continue;
            // Строго меньше: при равной цене остается более ранний
            if (best == count ||
                forecast[candidates[j]].eurKwh < forecast[candidates[best]].eurKwh) {
                best = j;
            }
        }
        picked[best] = true;
    }

    if (shiftToEnd) {
        // От поздних к ранним: каждый выбранный интервал переносится на самый поздний
        // свободный, который дороже его собственной цены не больше чем на допуск.
        // Сравнение всегда с исходной ценой, поэтому разница не накапливается.
        uint16_t order[kMaxWindowSlots];
        size_t orderCount = 0;
        for (size_t j = count; j-- > 0;) {
            if (picked[j]) order[orderCount++] = static_cast<uint16_t>(j);
        }
        for (size_t k = 0; k < orderCount; ++k) {
            const size_t source = order[k];
            const float limit = forecast[candidates[source]].eurKwh +
                kShiftToleranceEurKwh + kPriceEpsilon;
            for (size_t target = count; target-- > source + 1;) {
                if (!picked[target] && forecast[candidates[target]].eurKwh <= limit) {
                    picked[source] = false;
                    picked[target] = true;
                    break;
                }
            }
        }
    }

    for (size_t j = 0; j < count; ++j) {
        if (picked[j]) chosen[candidates[j]] = true;
    }
    return selected;
}

bool getNextWakeupEpoch(uint16_t wakeupMinute, uint32_t now, uint32_t& wakeupEpoch) {
    const int64_t today = rigatime::localDay(now);
    for (int64_t dayOffset = 0; dayOffset < 3; ++dayOffset) {
        uint32_t candidate = 0;
        // Несуществующее время в день перехода на летнее пропускается
        if (rigatime::localToUtc(today + dayOffset, wakeupMinute, candidate) &&
            candidate > now) {
            wakeupEpoch = candidate;
            return true;
        }
    }
    return false;
}

bool chooseConsecutiveHours(const ChannelConfig& config, const PriceHour* hours, size_t count,
    uint32_t now, BlockLock& lock, bool& selectedNow) {
    size_t duration = config.consecutiveBlockHours;
    uint32_t currentHour = now / 3600UL * 3600UL;
    if (lock.end > currentHour) {
        selectedNow = lock.start <= currentHour;
        return true;
    }
    lock.start = 0;
    lock.end = 0;
    if (duration > count) return false;
    float lowestCost = INFINITY;
    size_t bestStart = count;
    for (size_t start = 0; start + duration <= count; ++start) {
        float cost = 0.0f;
        bool valid = true;
        for (size_t offset = 0; offset < duration; ++offset) {
            size_t index = start + offset;
            if (hours[index].quarters != 4 ||
                hours[index].start < currentHour ||
                (offset > 0 && hours[index].start != hours[index - 1].start + 3600UL)) {
                valid = false;
                break;
            }
            cost += hours[index].avgEurKwh;
        }
        if (valid && cost < lowestCost) {
            lowestCost = cost;
            bestStart = start;
        }
    }
    if (bestStart == count) return false;
    lock.start = hours[bestStart].start;
    lock.end = hours[bestStart + duration - 1].start + 3600UL;
    selectedNow = lock.start <= currentHour;
    return true;
}

void buildPlan(const ChannelConfig& config, const PriceSeries& forecast, uint32_t now,
    PlannerState& state, BlockLock& lock, ChannelPlan& plan) {
    const size_t count = forecast.size();
    plan.slotCount = count;
    memset(plan.mask, 0, sizeof(plan.mask));
    plan.trace = {};
    PlanTrace& trace = plan.trace;
    const int64_t firstDay = count > 0 ? rigatime::localDay(forecast[0].start) : 0;
    const int64_t lastDay = count > 0 ? rigatime::localDay(forecast[count - 1].start) : -1;

    // Порог и аварийный предел — по цене каждого интервала
    for (size_t i = 0; i < count; ++i) {
        if (forecast[i].eurKwh <= config.alwaysOnBelowPriceEurKwh) plan.mask[i] |= slot::kThreshold;
        if (forecast[i].eurKwh > config.safetyCeilingEurKwh) plan.mask[i] |= slot::kCeiling;
    }

    // Умный порог: текущие сутки — с фиксацией, остальные сутки прогноза — по всему окну
    const Window today = localDayWindow(now);
    trace.smart = updateSelection(state.smart, config.smartThresholdEnabled,
        config.smartThresholdHours, 0, today, now, forecast, false);
    if (config.smartThresholdEnabled) {
        markSelection(state.smart, forecast, slot::kSmart, plan);
        trace.smartClamped = clamped(state.smart);
        trace.smartRequested = state.smart.requested;
        trace.smartAvailable = state.smart.available;
        for (int64_t day = firstDay; day <= lastDay; ++day) {
            const Window window = {rigatime::localDayStartUtc(day),
                rigatime::localDayStartUtc(day + 1)};
            if (window.start == today.start) continue;
            markWholeWindow(forecast, window, config.smartThresholdHours, false,
                slot::kSmart, plan);
        }
    }

    // Ночь: текущее или ближайшее окно — с фиксацией, остальные — по всему окну
    Window night = {0, 0};
    const bool hasNight = activeNightWindow(config, now, night);
    const uint32_t nightParam =
        (static_cast<uint32_t>(config.nightStartMinuteOfDay) << 16) | config.nightEndMinuteOfDay;
    trace.night = updateSelection(state.night, config.nightPrepEnabled && hasNight,
        config.nightCheapHours, nightParam, night, now, forecast, true);
    if (config.nightPrepEnabled && hasNight) {
        markSelection(state.night, forecast, slot::kNight, plan);
        trace.nightClamped = clamped(state.night);
        trace.nightRequested = state.night.requested;
        trace.nightAvailable = state.night.available;
        for (int64_t day = firstDay - 1; day <= lastDay; ++day) {
            const Window window = nightWindowOfDay(config, day);
            if (window.start == night.start) continue;
            markWholeWindow(forecast, window, config.nightCheapHours, true, slot::kNight, plan);
        }
    }

    // Пробуждение: цена не нужна; интервал закрашивается, если окно его задевает
    if (config.wakeupEnabled) {
        uint32_t nextWakeup = 0;
        trace.wakeup = getNextWakeupEpoch(config.wakeupTimeMinuteOfDay, now, nextWakeup)
            ? ScenarioStatus::Planned
            : ScenarioStatus::NoPlan;
        const uint32_t duration = static_cast<uint32_t>(config.wakeupHours) * 3600UL;
        for (int64_t day = firstDay - 1; day <= lastDay + 1; ++day) {
            uint32_t wakeup = 0;
            if (!rigatime::localToUtc(day, config.wakeupTimeMinuteOfDay, wakeup)) continue;
            const uint32_t start = wakeup - duration;
            for (size_t i = 0; i < count; ++i) {
                if (forecast[i].start < wakeup && forecast[i].start + kSlotSeconds > start) {
                    plan.mask[i] |= slot::kWakeup;
                }
            }
        }
    }

    // Непрерывный блок: без изменений до шага 2c, считается по средним ценам часов
    if (config.consecutiveBlockEnabled) {
        if (count == 0) {
            trace.block = ScenarioStatus::Waiting;
        } else {
            PriceHour hours[prices::kMaxHours];
            const size_t hourCount = forecast.buildHours(hours, prices::kMaxHours);
            bool selectedNow = false;
            if (chooseConsecutiveHours(config, hours, hourCount, now, lock, selectedNow)) {
                trace.block = ScenarioStatus::Planned;
                for (size_t i = 0; i < count; ++i) {
                    if (forecast[i].start >= lock.start && forecast[i].start < lock.end) {
                        plan.mask[i] |= slot::kBlock;
                    }
                }
            } else {
                trace.block = ScenarioStatus::NoPlan;
            }
        }
    }

    // Отпуск: до возвращения решает только он
    const bool holidayActive = config.holidayEnabled && config.holidayReturnEpoch > now;
    const Window holiday = holidayActive ? holidayWindow(config) : Window{0, 0};
    const ScenarioStatus holidayStatus = updateSelection(state.holiday, holidayActive,
        config.holidayOnHours, config.holidayReturnEpoch, holiday, now, forecast, true);
    if (holidayActive) {
        for (size_t i = 0; i < count; ++i) {
            if (forecast[i].start < config.holidayReturnEpoch) plan.mask[i] |= slot::kHolidayPeriod;
        }
        markSelection(state.holiday, forecast, slot::kHoliday, plan);
        if (now < holiday.start) {
            trace.holiday = HolidayStatus::BeforeWindow;
        } else {
            trace.holiday = holidayStatus == ScenarioStatus::Planned
                ? HolidayStatus::Selected
                : HolidayStatus::Waiting;
        }
        trace.holidayClamped = clamped(state.holiday);
        trace.holidayRequested = state.holiday.requested;
        trace.holidayAvailable = state.holiday.available;
    }
}

Decision decideNow(const ChannelConfig& config, const ChannelPlan& plan,
    const PriceSeries& forecast, uint32_t now, float currentPrice) {
    if (ceilingExceeded(config, currentPrice)) return {false, Reason::Ceiling};
    const int index = forecast.indexAt(now);
    const uint8_t mask = index >= 0 && static_cast<size_t>(index) < plan.slotCount
        ? plan.mask[index]
        : 0;
    if (config.holidayEnabled && config.holidayReturnEpoch > now) {
        return (mask & slot::kHoliday) != 0 ? Decision{true, Reason::Holiday}
                                            : Decision{false, Reason::HolidayOff};
    }
    if (currentPrice <= config.alwaysOnBelowPriceEurKwh) return {true, Reason::Threshold};
    if ((mask & slot::kSmart) != 0) return {true, Reason::Smart};
    if (config.wakeupEnabled) {
        uint32_t wakeup = 0;
        if (getNextWakeupEpoch(config.wakeupTimeMinuteOfDay, now, wakeup) &&
            now >= wakeup - static_cast<uint32_t>(config.wakeupHours) * 3600UL) {
            return {true, Reason::Wakeup};
        }
    }
    if ((mask & slot::kNight) != 0) return {true, Reason::Night};
    if ((mask & slot::kBlock) != 0) return {true, Reason::Block};
    return {false, Reason::None};
}

bool slotOn(uint8_t mask) {
    if ((mask & slot::kCeiling) != 0) return false;
    if ((mask & slot::kHolidayPeriod) != 0) return (mask & slot::kHoliday) != 0;
    return (mask & kScenarioBits) != 0;
}

Reason slotReason(uint8_t mask) {
    Reason reason = Reason::None;
    if ((mask & slot::kHolidayPeriod) != 0) {
        reason = (mask & slot::kHoliday) != 0 ? Reason::Holiday : Reason::HolidayOff;
        if (reason == Reason::HolidayOff) return reason;
    } else if ((mask & slot::kThreshold) != 0) {
        reason = Reason::Threshold;
    } else if ((mask & slot::kSmart) != 0) {
        reason = Reason::Smart;
    } else if ((mask & slot::kWakeup) != 0) {
        reason = Reason::Wakeup;
    } else if ((mask & slot::kNight) != 0) {
        reason = Reason::Night;
    } else if ((mask & slot::kBlock) != 0) {
        reason = Reason::Block;
    }
    if (reason != Reason::None && (mask & slot::kCeiling) != 0) return Reason::Ceiling;
    return reason;
}

const char* reasonName(Reason reason) {
    switch (reason) {
        case Reason::None: return "ни один сценарий не выбрал интервал";
        case Reason::Threshold: return "Порог включения";
        case Reason::Smart: return "Умный порог включения";
        case Reason::Wakeup: return "Включение перед пробуждением";
        case Reason::Night: return "Включение ночью";
        case Reason::Block: return "Непрерывный блок";
        case Reason::Holiday: return "Отпуск: выбранный интервал";
        case Reason::HolidayOff: return "Отпуск: интервал не выбран";
        case Reason::Ceiling: return "Аварийный предел цены";
    }
    return "";
}

const char* modeName(const ChannelConfig& config, const ModeInputs& in) {
    if (in.manual) return "Ручной";
    if (!in.hasLatestPrice) {
        return in.hadAnyPrice
            ? "Нет актуальной цены: реле выключено"
            : "Ожидание первой цены";
    }
    if (in.latestPriceEurKwh > config.safetyCeilingEurKwh) {
        return "Аварийное отключение по верхнему пределу";
    }
    if (config.holidayEnabled &&
        (!in.timeSynced || config.holidayReturnEpoch > in.now)) {
        return "Отпуск";
    }
    if (in.latestPriceEurKwh <= config.alwaysOnBelowPriceEurKwh) {
        return "Порог включения";
    }
    if (config.smartThresholdEnabled) return "Умный порог включения";
    if (config.wakeupEnabled) return "Включение перед пробуждением";
    if (config.nightPrepEnabled) return "Включение ночью";
    if (config.consecutiveBlockEnabled) return "Непрерывный нагрев";
    return "По цене";
}

}  // namespace planner
