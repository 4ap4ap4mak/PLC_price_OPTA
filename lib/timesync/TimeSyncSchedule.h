// Когда запрашивать время NTP (KI-034). Чистый модуль L0, только заголовок.
//
// До первой синхронизации и после неудачного запроса — раз в retryMs (минута), после
// удачного — раз в resyncMs (6 часов). Раньше неудачный запрос после синхронизации
// повторялся только через 6 часов: на железе NTP не получил сокет, пока были открыты
// MQTT, страница и проверка прошивки (лимит 4 сокета, D-031).
#pragma once

#include <stdint.h>

namespace timesync {

struct Schedule {
    bool requested = false;     // запрос уже ставился
    bool lastFailed = false;    // последний завершенный запрос не удался
    uint32_t lastRequestMs = 0; // millis() постановки последнего запроса
};

// Сравнение интервалов только разностью: millis() переполняется (CLAUDE.md).
inline bool due(const Schedule& schedule, bool synced, uint32_t nowMs, uint32_t retryMs,
    uint32_t resyncMs) {
    if (!schedule.requested) return true;
    const uint32_t interval = (!synced || schedule.lastFailed) ? retryMs : resyncMs;
    return nowMs - schedule.lastRequestMs >= interval;
}

}  // namespace timesync
