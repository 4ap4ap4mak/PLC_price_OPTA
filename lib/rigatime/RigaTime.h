// Календарь и местное время Europe/Riga (UTC+2, летом UTC+3).
// Чистый модуль L0: без Arduino, без глобального состояния, не блокирует, не логирует.
// Время везде — uint32_t epoch UTC. Переход на летнее время — последнее воскресенье
// марта в 01:00 UTC, обратно — последнее воскресенье октября в 01:00 UTC.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rigatime {

constexpr uint32_t kSecondsPerDay = 86400UL;

// Дни от 1970-01-01 для григорианской даты и обратно (алгоритм Howard Hinnant).
int64_t daysFromCivil(int year, unsigned month, unsigned day);
void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day);
unsigned lastSundayOfMonth(int year, unsigned month);

bool isDaylightSaving(uint32_t epoch);
// epoch + смещение Риги. Целая часть деления на сутки — номер местных суток.
uint32_t toLocal(uint32_t epoch);
void localHourAndDay(uint32_t epoch, uint8_t& hour, int64_t& day);
// Номер местных суток (дни от 1970-01-01 по календарю Риги).
int64_t localDay(uint32_t epoch);

// Местные сутки day и минута суток → epoch. Час 03:00–03:59 в день перехода на
// зимнее время повторяется — берется второй, зимний. В день перехода на летнее время
// 03:00–03:59 не существует: тогда false, а epoch — момент перехода (01:00 UTC).
bool localToUtc(int64_t day, uint16_t minuteOfDay, uint32_t& epoch);
// Начало местных суток day (00:00 по Риге) в UTC.
uint32_t localDayStartUtc(int64_t day);

// "HH:MM" → минута суток. text не обязан заканчиваться нулем: важна длина len.
bool parseTimeOfDay(const char* text, size_t len, uint16_t& minuteOfDay);
// out — не меньше 8 байт.
void formatTimeOfDay(uint16_t minuteOfDay, char* out, size_t cap);

// "YYYY-MM-DDTHH:MM" (местное время Риги) → epoch. false для неверной даты,
// года вне 2024–2038 и несуществующего времени при переходе на летнее.
// Числовые поля разбираются как atol (так работал String::toInt в монолите).
bool parseLocalDateTime(const char* text, size_t len, uint32_t& epoch);
// out — не меньше 17 байт.
void formatLocalDateTime(uint32_t epoch, char* out, size_t cap);

}  // namespace rigatime
