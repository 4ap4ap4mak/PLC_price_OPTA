#include "SwitchHistory.h"

#include <stdio.h>

#include <RigaTime.h>

namespace history {

void SwitchHistory::record(uint32_t epoch, uint32_t uptimeSeconds, uint8_t channel, bool on,
    Cause cause) {
    events_[next_] = {epoch, uptimeSeconds, channel, on, cause};
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) {
        ++count_;
    } else {
        ++dropped_;
    }
}

void SwitchHistory::fillMissingEpochs(uint32_t nowEpoch, uint32_t nowUptimeSeconds) {
    for (size_t i = 0; i < count_; ++i) {
        SwitchEvent& event = events_[i];
        if (event.epoch == 0 && event.uptimeSeconds <= nowUptimeSeconds) {
            event.epoch = nowEpoch - (nowUptimeSeconds - event.uptimeSeconds);
        }
    }
}

const SwitchEvent& SwitchHistory::newest(size_t index) const {
    return events_[(next_ + kCapacity - 1 - index % kCapacity) % kCapacity];
}

const char* causeName(Cause cause) {
    switch (cause) {
        case Cause::Startup: return "Запуск устройства";
        case Cause::Threshold: return "Порог включения";
        case Cause::Smart: return "Умный порог включения";
        case Cause::Wakeup: return "Включение перед пробуждением";
        case Cause::Night: return "Включение ночью";
        case Cause::Holiday: return "Отпуск: выбранный интервал";
        case Cause::HolidayOff: return "Отпуск: интервал не выбран";
        case Cause::NoScenario: return "Ни один сценарий не выбрал интервал";
        case Cause::Ceiling: return "Аварийный предел цены";
        case Cause::Failsafe: return "Нет актуальной цены больше часа";
        case Cause::Manual: return "Ручной режим";
        case Cause::ForceOn: return "Принудительное включение";
        case Cause::NoPrice: return "Нет актуальной цены после принудительного включения";
    }
    return "";
}

void formatEventTime(const SwitchEvent& event, char* out, size_t cap) {
    if (event.epoch == 0) {
        snprintf(out, cap, "+%lu с от запуска", static_cast<unsigned long>(event.uptimeSeconds));
        return;
    }
    const uint32_t local = rigatime::toLocal(event.epoch);
    int year;
    unsigned month;
    unsigned day;
    rigatime::civilFromDays(local / 86400UL, year, month, day);
    const uint32_t second = local % 86400UL;
    snprintf(out, cap, "%02u.%02u.%04d %02u:%02u:%02u", day, month, year,
        static_cast<unsigned>(second / 3600UL), static_cast<unsigned>(second % 3600UL / 60UL),
        static_cast<unsigned>(second % 60UL));
}

}  // namespace history
