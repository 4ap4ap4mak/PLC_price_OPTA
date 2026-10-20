#include "Planner.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <RigaTime.h>

namespace planner {

using prices::kMaxSlots;
using rigatime::localHourAndDay;
using rigatime::toLocal;

int64_t nightWindowKey(uint32_t timestamp, const ChannelConfig& config) {
    uint8_t hour;
    int64_t day;
    localHourAndDay(timestamp, hour, day);
    uint16_t minuteOfDay = static_cast<uint16_t>(hour * 60);
    uint16_t start = config.nightStartMinuteOfDay;
    uint16_t end = config.nightEndMinuteOfDay;
    if (start < end) {
        return minuteOfDay >= start && minuteOfDay + 60 <= end ? day : -1;
    }
    if (minuteOfDay >= start && minuteOfDay + 60 <= 1440) return day;
    if (minuteOfDay + 60 <= end) return day - 1;
    return -1;
}

bool chooseCheapestNightHours(const ChannelConfig& config, const PriceHour* hours, size_t count,
    uint32_t now, bool& selectedNow) {
    uint32_t currentHour = now / 3600UL * 3600UL;
    if (config.nightStartMinuteOfDay == config.nightEndMinuteOfDay) return false;
    int64_t nearestWindow = INT64_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (hours[i].start + 3600UL <= currentHour || hours[i].quarters != 4) continue;
        int64_t key = nightWindowKey(hours[i].start, config);
        if (key >= 0 && key < nearestWindow) nearestWindow = key;
    }
    if (nearestWindow == INT64_MAX) return false;

    bool chosen[kMaxSlots] = {};
    for (uint8_t n = 0; n < config.nightCheapHours; ++n) {
        size_t cheapest = count;
        for (size_t i = 0; i < count; ++i) {
            if (hours[i].start + 3600UL <= currentHour ||
                hours[i].quarters != 4 ||
                nightWindowKey(hours[i].start, config) != nearestWindow || chosen[i]) continue;
            if (cheapest == count || hours[i].avgEurKwh < hours[cheapest].avgEurKwh) {
                cheapest = i;
            }
        }
        if (cheapest == count) return false;
        chosen[cheapest] = true;
        if (hours[cheapest].start == currentHour) selectedNow = true;
    }
    return true;
}

bool chooseCheapestDayHours(const ChannelConfig& config, const PriceHour* hours, size_t count,
    uint32_t now, bool& selectedNow, bool& clamped, size_t& requestedHours, size_t& availableHours) {
    int64_t currentDay;
    uint32_t currentHour = now / 3600UL * 3600UL;
    uint8_t currentHourOfDay;
    localHourAndDay(now, currentHourOfDay, currentDay);
    size_t available = 0;
    for (size_t i = 0; i < count; ++i) {
        uint8_t hourOfDay;
        int64_t day;
        localHourAndDay(hours[i].start, hourOfDay, day);
        if (day == currentDay && hours[i].quarters == 4) {
            ++available;
        }
    }
    if (available == 0) return false;

    size_t hoursToSelect = config.smartThresholdHours;
    if (hoursToSelect > available) {
        clamped = true;
        requestedHours = hoursToSelect;
        availableHours = available;
        hoursToSelect = available;
    }

    bool chosen[kMaxSlots] = {};
    for (size_t n = 0; n < hoursToSelect; ++n) {
        size_t cheapest = count;
        for (size_t i = 0; i < count; ++i) {
            uint8_t hourOfDay;
            int64_t day;
            localHourAndDay(hours[i].start, hourOfDay, day);
            if (day != currentDay || hours[i].quarters != 4 || chosen[i]) continue;
            if (cheapest == count || hours[i].avgEurKwh < hours[cheapest].avgEurKwh) {
                cheapest = i;
            }
        }
        if (cheapest == count) return false;
        chosen[cheapest] = true;
        if (hours[cheapest].start == currentHour) selectedNow = true;
    }
    return true;
}

bool getNextWakeupEpoch(uint16_t wakeupMinute, uint32_t now, uint32_t& wakeupEpoch) {
    uint8_t currentHour;
    int64_t currentDay;
    localHourAndDay(now, currentHour, currentDay);
    for (int dayOffset = 0; dayOffset < 3; ++dayOffset) {
        int year;
        unsigned month;
        unsigned day;
        rigatime::civilFromDays(currentDay + dayOffset, year, month, day);
        char localDateTime[48];
        int length = snprintf(localDateTime, sizeof(localDateTime), "%04d-%02u-%02uT%02u:%02u",
            year, month, day, wakeupMinute / 60, wakeupMinute % 60);
        uint32_t candidate = 0;
        if (length > 0 &&
            rigatime::parseLocalDateTime(localDateTime, static_cast<size_t>(length), candidate) &&
            candidate > now) {
            wakeupEpoch = candidate;
            return true;
        }
    }
    return false;
}

bool isWakeupOnTime(const ChannelConfig& config, uint32_t now, bool& selectedNow) {
    uint32_t wakeupEpoch;
    if (!getNextWakeupEpoch(config.wakeupTimeMinuteOfDay, now, wakeupEpoch)) return false;
    uint32_t endHour = wakeupEpoch / 3600UL * 3600UL;
    uint32_t startHour = endHour - static_cast<uint32_t>(config.wakeupHours) * 3600UL;
    uint32_t currentHour = now / 3600UL * 3600UL;
    selectedNow = currentHour >= startHour && currentHour < endHour;
    return true;
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

bool chooseHolidayHours(const ChannelConfig& config, const PriceHour* hours, size_t count,
    uint32_t now, bool& selectedNow) {
    uint32_t currentHour = now / 3600UL * 3600UL;
    uint32_t returnTime = config.holidayReturnEpoch;
    if (returnTime <= now || count == 0 ||
        hours[count - 1].start + 3600UL < returnTime) {
        return false;
    }
    bool chosen[kMaxSlots] = {};
    for (uint8_t n = 0; n < config.holidayOnHours; ++n) {
        size_t cheapest = count;
        for (size_t i = 0; i < count; ++i) {
            if (hours[i].start < currentHour ||
                hours[i].start + 3600UL > returnTime ||
                hours[i].quarters != 4 || chosen[i]) continue;
            if (cheapest == count || hours[i].avgEurKwh < hours[cheapest].avgEurKwh) {
                cheapest = i;
            }
        }
        if (cheapest == count) return false;
        chosen[cheapest] = true;
        if (hours[cheapest].start == currentHour) selectedNow = true;
    }
    return true;
}

static ScenarioStatus scenarioStatus(bool forecastAvailable, bool planAvailable) {
    if (!forecastAvailable) return ScenarioStatus::NoForecast;
    return planAvailable ? ScenarioStatus::Planned : ScenarioStatus::NoPlan;
}

bool decideScenarios(const ChannelConfig& config, float currentPrice, bool forecastAvailable,
    const PriceSeries& forecast, uint32_t now, BlockLock& lock, DecisionTrace& trace) {
    trace = {};
    trace.holiday = HolidayStatus::Inactive;
    trace.smart = ScenarioStatus::Disabled;
    trace.wakeup = ScenarioStatus::Disabled;
    trace.night = ScenarioStatus::Disabled;
    trace.block = ScenarioStatus::Disabled;

    PriceHour hours[kMaxSlots];
    if (config.holidayEnabled) {
        if (!forecastAvailable) {
            trace.holiday = HolidayStatus::NoForecast;
            return false;
        }
        size_t count = forecast.buildHours(hours, kMaxSlots);
        bool selectedNow = false;
        if (chooseHolidayHours(config, hours, count, now, selectedNow)) {
            trace.holiday = HolidayStatus::Selected;
            return selectedNow;
        }
        trace.holiday = HolidayStatus::NotCovered;
        return false;
    }

    bool selectedByScenario = currentPrice <= config.alwaysOnBelowPriceEurKwh;

    size_t hourCount = 0;
    if (forecastAvailable &&
        (config.smartThresholdEnabled || config.nightPrepEnabled ||
         config.consecutiveBlockEnabled)) {
        hourCount = forecast.buildHours(hours, kMaxSlots);
    }

    if (config.smartThresholdEnabled) {
        bool selectedNow = false;
        bool planAvailable = forecastAvailable &&
            chooseCheapestDayHours(config, hours, hourCount, now, selectedNow,
                trace.smartClamped, trace.smartRequestedHours, trace.smartAvailableHours);
        selectedByScenario = selectedByScenario || (planAvailable && selectedNow);
        trace.smart = scenarioStatus(forecastAvailable, planAvailable);
    }
    if (config.wakeupEnabled) {
        bool selectedNow = false;
        if (!isWakeupOnTime(config, now, selectedNow)) {
            trace.wakeup = ScenarioStatus::NoPlan;
        } else {
            selectedByScenario = selectedByScenario || selectedNow;
            trace.wakeup = ScenarioStatus::Planned;
        }
    }
    if (config.nightPrepEnabled) {
        bool selectedNow = false;
        bool planAvailable = forecastAvailable &&
            chooseCheapestNightHours(config, hours, hourCount, now, selectedNow);
        selectedByScenario = selectedByScenario || (planAvailable && selectedNow);
        trace.night = scenarioStatus(forecastAvailable, planAvailable);
    }
    if (config.consecutiveBlockEnabled) {
        bool selectedNow = false;
        bool planAvailable = forecastAvailable &&
            chooseConsecutiveHours(config, hours, hourCount, now, lock, selectedNow);
        selectedByScenario = selectedByScenario || (planAvailable && selectedNow);
        trace.block = scenarioStatus(forecastAvailable, planAvailable);
    }
    return selectedByScenario;
}

void buildChartSlots(const ChannelConfig& config, const PriceSeries& forecast,
    bool timeSynced, uint32_t now, uint32_t chartLocalDay,
    bool manual, bool relayOn, const BlockLock& lock, bool* workSlots) {
    const size_t slotCount = forecast.size();
    if (slotCount == 0) return;

    // static, как в монолите: ~2.6 КБ не должны ложиться на стек веб-обработчика.
    // Функция не реентерабельна; вызывается только из основного потока.
    static PriceHour hours[kMaxSlots];
    static bool chosen[kMaxSlots];
    memset(workSlots, 0, kMaxSlots * sizeof(workSlots[0]));
    uint32_t currentHour = now / 3600UL * 3600UL;
    size_t hourCount = forecast.buildHours(hours, kMaxSlots);

    if (manual) {
        if (relayOn) {
            for (size_t i = 0; i < slotCount; ++i) {
                workSlots[i] = toLocal(forecast[i].start) / 86400UL == chartLocalDay;
            }
        }
        return;
    }

    auto markHour = [&](uint32_t hourTimestamp) {
        for (size_t slot = 0; slot < slotCount; ++slot) {
            uint32_t timestamp = forecast[slot].start;
            if (timestamp >= hourTimestamp && timestamp < hourTimestamp + 3600UL) {
                if (toLocal(timestamp) / 86400UL == chartLocalDay) {
                    workSlots[slot] = true;
                }
            }
        }
    };

    bool holidayActive = config.holidayEnabled &&
        (!timeSynced || config.holidayReturnEpoch > now);
    if (holidayActive) {
        if (!timeSynced) return;
        memset(chosen, 0, sizeof(chosen));
        for (uint8_t n = 0; n < config.holidayOnHours; ++n) {
            size_t cheapest = hourCount;
            for (size_t i = 0; i < hourCount; ++i) {
                if (hours[i].start < currentHour ||
                    hours[i].start + 3600UL > config.holidayReturnEpoch ||
                    hours[i].quarters != 4 || chosen[i]) {
                    continue;
                }
                if (cheapest == hourCount ||
                    hours[i].avgEurKwh < hours[cheapest].avgEurKwh) {
                    cheapest = i;
                }
            }
            if (cheapest == hourCount) break;
            chosen[cheapest] = true;
            markHour(hours[cheapest].start);
        }
    } else {
        for (size_t i = 0; i < slotCount; ++i) {
            if (toLocal(forecast[i].start) / 86400UL == chartLocalDay &&
                forecast[i].eurKwh <= config.alwaysOnBelowPriceEurKwh) {
                workSlots[i] = true;
            }
        }

        if (config.smartThresholdEnabled) {
            memset(chosen, 0, sizeof(chosen));
            for (uint8_t n = 0; n < config.smartThresholdHours; ++n) {
                size_t cheapest = hourCount;
                for (size_t i = 0; i < hourCount; ++i) {
                    uint8_t hourOfDay;
                    int64_t day;
                    localHourAndDay(hours[i].start, hourOfDay, day);
                    if (day != chartLocalDay || hours[i].quarters != 4 ||
                        chosen[i]) {
                        continue;
                    }
                    if (cheapest == hourCount ||
                        hours[i].avgEurKwh < hours[cheapest].avgEurKwh) {
                        cheapest = i;
                    }
                }
                if (cheapest == hourCount) break;
                chosen[cheapest] = true;
                markHour(hours[cheapest].start);
            }
        }

        if (config.wakeupEnabled && timeSynced) {
            uint32_t wakeupEpoch;
            if (getNextWakeupEpoch(config.wakeupTimeMinuteOfDay, now, wakeupEpoch)) {
                uint32_t endHour = wakeupEpoch / 3600UL * 3600UL;
                uint32_t startHour = endHour -
                    static_cast<uint32_t>(config.wakeupHours) * 3600UL;
                for (size_t i = 0; i < slotCount; ++i) {
                    uint32_t timestamp = forecast[i].start;
                    if (timestamp >= startHour && timestamp < endHour &&
                        toLocal(timestamp) / 86400UL == chartLocalDay) {
                        workSlots[i] = true;
                    }
                }
            }
        }

        if (config.nightPrepEnabled) {
            int64_t nearestWindow = INT64_MAX;
            for (size_t i = 0; i < hourCount; ++i) {
                if (hours[i].start < currentHour || hours[i].quarters != 4) {
                    continue;
                }
                uint8_t hourOfDay;
                int64_t day;
                localHourAndDay(hours[i].start, hourOfDay, day);
                if (day != chartLocalDay) continue;
                int64_t key = nightWindowKey(hours[i].start, config);
                if (key >= 0 && key < nearestWindow) nearestWindow = key;
            }
            if (nearestWindow != INT64_MAX) {
                memset(chosen, 0, sizeof(chosen));
                for (uint8_t n = 0; n < config.nightCheapHours; ++n) {
                    size_t cheapest = hourCount;
                    for (size_t i = 0; i < hourCount; ++i) {
                        if (hours[i].start < currentHour ||
                            hours[i].quarters != 4 || chosen[i] ||
                            nightWindowKey(hours[i].start, config) != nearestWindow) {
                            continue;
                        }
                        uint8_t hourOfDay;
                        int64_t day;
                        localHourAndDay(hours[i].start, hourOfDay, day);
                        if (day != chartLocalDay) continue;
                        if (cheapest == hourCount ||
                            hours[i].avgEurKwh < hours[cheapest].avgEurKwh) {
                            cheapest = i;
                        }
                    }
                    if (cheapest == hourCount) break;
                    chosen[cheapest] = true;
                    markHour(hours[cheapest].start);
                }
            }
        }

        if (config.consecutiveBlockEnabled) {
            uint32_t blockStart = lock.start;
            uint32_t blockEnd = lock.end;
            if (blockEnd <= currentHour) {
                blockStart = 0;
                blockEnd = 0;
                size_t duration = config.consecutiveBlockHours;
                float lowestCost = INFINITY;
                size_t bestStart = hourCount;
                for (size_t start = 0; start + duration <= hourCount; ++start) {
                    float cost = 0.0f;
                    bool valid = true;
                    for (size_t offset = 0; offset < duration; ++offset) {
                        size_t index = start + offset;
                        if (hours[index].quarters != 4 ||
                            hours[index].start < currentHour ||
                            (offset > 0 && hours[index].start !=
                                hours[index - 1].start + 3600UL)) {
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
                if (bestStart < hourCount) {
                    blockStart = hours[bestStart].start;
                    blockEnd = hours[bestStart + duration - 1].start + 3600UL;
                }
            }
            if (blockEnd > blockStart) {
                for (size_t i = 0; i < slotCount; ++i) {
                    uint32_t timestamp = forecast[i].start;
                    if (timestamp >= blockStart && timestamp < blockEnd &&
                        toLocal(timestamp) / 86400UL == chartLocalDay) {
                        workSlots[i] = true;
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < slotCount; ++i) {
        if (forecast[i].eurKwh > config.safetyCeilingEurKwh) {
            workSlots[i] = false;
        }
    }
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
