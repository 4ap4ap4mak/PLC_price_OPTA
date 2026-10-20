#include "RigaTime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rigatime {

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
    const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
}

void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    year = static_cast<int>(yearOfEra) + static_cast<int>(era * 400);
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    month = monthPrime < 10 ? monthPrime + 3 : monthPrime - 9;
    year += month <= 2;
}

unsigned lastSundayOfMonth(int year, unsigned month) {
    static const uint8_t daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned lastDay = daysPerMonth[month - 1];
    bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leapYear) {
        lastDay = 29;
    }
    int64_t lastDayEpoch = daysFromCivil(year, month, lastDay);
    unsigned weekday = static_cast<unsigned>((lastDayEpoch + 4) % 7);
    return lastDay - weekday;
}

bool isDaylightSaving(uint32_t epoch) {
    int year;
    unsigned month;
    unsigned day;
    civilFromDays(epoch / 86400UL, year, month, day);
    uint32_t dstStart = static_cast<uint32_t>(
        daysFromCivil(year, 3, lastSundayOfMonth(year, 3)) * 86400 + 3600);
    uint32_t dstEnd = static_cast<uint32_t>(
        daysFromCivil(year, 10, lastSundayOfMonth(year, 10)) * 86400 + 3600);
    return epoch >= dstStart && epoch < dstEnd;
}

uint32_t toLocal(uint32_t epoch) {
    return epoch + (isDaylightSaving(epoch) ? 3UL : 2UL) * 3600UL;
}

void localHourAndDay(uint32_t epoch, uint8_t& hour, int64_t& day) {
    uint32_t localEpoch = toLocal(epoch);
    day = localEpoch / 86400UL;
    hour = static_cast<uint8_t>((localEpoch % 86400UL) / 3600UL);
}

static bool isAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

bool parseTimeOfDay(const char* text, size_t len, uint16_t& minuteOfDay) {
    if (len != 5 || text[2] != ':') return false;
    if (!isAsciiDigit(text[0]) || !isAsciiDigit(text[1]) ||
        !isAsciiDigit(text[3]) || !isAsciiDigit(text[4])) {
        return false;
    }
    int hour = (text[0] - '0') * 10 + text[1] - '0';
    int minute = (text[3] - '0') * 10 + text[4] - '0';
    if (hour > 23 || minute > 59) return false;
    minuteOfDay = static_cast<uint16_t>(hour * 60 + minute);
    return true;
}

void formatTimeOfDay(uint16_t minuteOfDay, char* out, size_t cap) {
    snprintf(out, cap, "%02u:%02u",
        static_cast<unsigned>(minuteOfDay / 60),
        static_cast<unsigned>(minuteOfDay % 60));
}

// Повторяет String::substring(from, to).toInt(): atol по копии фрагмента.
static long fieldToLong(const char* text, size_t from, size_t to) {
    char field[8];
    size_t length = to - from;
    memcpy(field, text + from, length);
    field[length] = '\0';
    return atol(field);
}

bool parseLocalDateTime(const char* text, size_t len, uint32_t& epoch) {
    if (len != 16 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':') {
        return false;
    }
    int year = static_cast<int>(fieldToLong(text, 0, 4));
    unsigned month = static_cast<unsigned>(fieldToLong(text, 5, 7));
    unsigned day = static_cast<unsigned>(fieldToLong(text, 8, 10));
    unsigned hour = static_cast<unsigned>(fieldToLong(text, 11, 13));
    unsigned minute = static_cast<unsigned>(fieldToLong(text, 14, 16));
    if (year < 2024 || year > 2038 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59) {
        return false;
    }
    int64_t localAsUtc = daysFromCivil(year, month, day) * 86400 +
        hour * 3600 + minute * 60;
    int normalizedYear;
    unsigned normalizedMonth;
    unsigned normalizedDay;
    civilFromDays(localAsUtc / 86400, normalizedYear, normalizedMonth, normalizedDay);
    if (year != normalizedYear || month != normalizedMonth || day != normalizedDay) {
        return false;
    }
    uint32_t standardTimeCandidate = static_cast<uint32_t>(localAsUtc - 2 * 3600);
    epoch = static_cast<uint32_t>(localAsUtc -
        (isDaylightSaving(standardTimeCandidate) ? 3 * 3600 : 2 * 3600));
    return isDaylightSaving(epoch) ? epoch + 3 * 3600 == localAsUtc
                                   : epoch + 2 * 3600 == localAsUtc;
}

void formatLocalDateTime(uint32_t epoch, char* out, size_t cap) {
    uint32_t localEpoch = toLocal(epoch);
    int year;
    unsigned month;
    unsigned day;
    civilFromDays(localEpoch / 86400UL, year, month, day);
    uint32_t secondsInDay = localEpoch % 86400UL;
    snprintf(out, cap, "%04d-%02u-%02uT%02u:%02u",
        year, month, day, static_cast<unsigned>((secondsInDay / 3600UL)),
        static_cast<unsigned>((secondsInDay % 3600UL) / 60UL));
}

}  // namespace rigatime
