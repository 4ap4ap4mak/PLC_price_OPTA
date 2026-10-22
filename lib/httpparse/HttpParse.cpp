#include "HttpParse.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <RigaTime.h>

namespace httpparse {

static size_t decodeFormValue(const char* value, size_t length, char* out) {
    size_t decoded = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = value[i];
        if (c == '+') {
            out[decoded++] = ' ';
        } else if (c == '%' && i + 2 < length) {
            char hex[3] = {value[i + 1], value[i + 2], '\0'};
            out[decoded++] = static_cast<char>(strtoul(hex, nullptr, 16));
            i += 2;
        } else {
            out[decoded++] = c;
        }
    }
    out[decoded] = '\0';
    return decoded;
}

void parseRequestLine(const char* line, RequestLine& out) {
    out = RequestLine{0, false, 0, 0};
    const char* firstSpace = strchr(line, ' ');
    if (firstSpace == nullptr) {
        return;
    }
    out.methodLength = static_cast<size_t>(firstSpace - line);
    const char* secondSpace = strchr(firstSpace + 1, ' ');
    if (secondSpace == nullptr) {
        return;
    }
    out.hasPath = true;
    out.pathStart = out.methodLength + 1;
    out.pathLength = static_cast<size_t>(secondSpace - firstSpace) - 1;
}

bool parseContentLength(const char* headerLine, long& value) {
    static const char kName[] = "content-length:";
    for (size_t i = 0; i < sizeof(kName) - 1; ++i) {
        // Нулевой байт строки не совпадет ни с одним символом имени
        if (tolower(static_cast<unsigned char>(headerLine[i])) != kName[i]) {
            return false;
        }
    }
    value = strtol(headerLine + sizeof(kName) - 1, nullptr, 10);
    return true;
}

size_t formField(const char* body, size_t bodyLen, const char* name, char* out) {
    // Как String::indexOf в монолите: поиск ключа и '&' останавливается на нулевом
    // байте (strstr/strchr), а значение без '&' тянется до конца тела.
    size_t nameLen = strlen(name);
    char key[32];
    if (nameLen + 2 > sizeof(key)) {
        out[0] = '\0';
        return 0;
    }
    memcpy(key, name, nameLen);
    key[nameLen] = '=';
    key[nameLen + 1] = '\0';

    const char* found = strstr(body, key);
    if (found == nullptr) {
        out[0] = '\0';
        return 0;
    }
    size_t start = static_cast<size_t>(found - body) + nameLen + 1;
    const char* ampersand = strchr(body + start, '&');
    size_t end = ampersand != nullptr ? static_cast<size_t>(ampersand - body) : bodyLen;
    return decodeFormValue(body + start, end - start, out);
}

bool parseFormFloat(const char* text, size_t len, float minimum, float maximum, float& value) {
    if (len == 0) {
        return false;
    }
    char* end = nullptr;
    value = strtof(text, &end);
    return end != text && *end == '\0' && isfinite(value) &&
        value >= minimum && value <= maximum;
}

bool parseFormInteger(const char* text, size_t len, int minimum, int maximum, int& value) {
    if (len == 0) {
        return false;
    }
    char* end = nullptr;
    long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool queryChannel(const char* path, uint8_t& channelIndex) {
    const char* found = strstr(path, "?relay=");
    if (found == nullptr) return false;
    int channelNumber = static_cast<int>(atol(found + 7));
    if (channelNumber < 1 || channelNumber > kChannelCount) return false;
    channelIndex = static_cast<uint8_t>(channelNumber - 1);
    return true;
}

bool queryChartDay(const char* path, int8_t& day) {
    const char* found = strstr(path, "?day=");
    if (found == nullptr) {
        found = strstr(path, "&day=");
    }
    if (found == nullptr) return false;
    const char* value = found + 5;
    const char* ampersand = strchr(value, '&');
    size_t length = ampersand != nullptr ? static_cast<size_t>(ampersand - value) : strlen(value);
    // parseFormInteger по значению до '&' без копии: strtol все равно остановится на '&'
    if (length == 0) return false;
    char* end = nullptr;
    long parsed = strtol(value, &end, 10);
    if (end == value || end != value + length || parsed < -1 || parsed > 1) return false;
    day = static_cast<int8_t>(parsed);
    return true;
}

PageSelection pageSelection(const char* path) {
    PageSelection selection{0, 0};
    uint8_t channelIndex;
    if (queryChannel(path, channelIndex)) {
        selection.channelIndex = channelIndex;
    }
    int8_t chartDay;
    if (queryChartDay(path, chartDay)) {
        selection.chartDay = chartDay;
    }
    return selection;
}

void formatHomeLocation(uint8_t channelIndex, int8_t chartDay, char* out, size_t cap) {
    if (chartDay == 0) {
        snprintf(out, cap, "/?relay=%u", static_cast<unsigned>(channelIndex) + 1U);
        return;
    }
    snprintf(out, cap, "/?relay=%u&day=%d", static_cast<unsigned>(channelIndex) + 1U,
        static_cast<int>(chartDay));
}

const char* formErrorText(FormError error) {
    switch (error) {
        case FormError::Ok: return "";
        case FormError::InvalidChannel: return "Invalid relay selection";
        case FormError::InvalidScenario: return "Invalid scenario settings";
        case FormError::NightHoursExceedWindow: return "Night hours exceed the window";
        case FormError::InvalidReturnDate: return "Invalid Europe/Riga return date/time";
        case FormError::ReturnDateNotFuture: return "Return date must be in the future";
        case FormError::InvalidWifi: return "Invalid Wi-Fi credentials";
    }
    return "";
}

namespace {

// Буфер под одно декодированное поле: значение не длиннее тела.
struct FieldReader {
    const char* body;
    size_t bodyLen;
    char value[kMaxFormBody + 1];
    size_t length;

    const char* read(const char* name) {
        length = formField(body, bodyLen, name, value);
        return value;
    }
    bool isOne(const char* name) {
        // String == "1" в монолите — это strcmp
        return strcmp(read(name), "1") == 0;
    }
    bool readFloat(const char* name, float minimum, float maximum, float& out) {
        read(name);
        return parseFormFloat(value, length, minimum, maximum, out);
    }
    bool readInteger(const char* name, int minimum, int maximum, int& out) {
        read(name);
        return parseFormInteger(value, length, minimum, maximum, out);
    }
    bool readTimeOfDay(const char* name, uint16_t& out) {
        read(name);
        return rigatime::parseTimeOfDay(value, length, out);
    }
    // Вкладка графика для адреса возврата; нет поля или неверное — сегодня.
    int8_t readChartDay() {
        int day;
        return readInteger("chart_day", -1, 1, day) ? static_cast<int8_t>(day) : 0;
    }
};

// String::toCharArray: копия до нулевого байта, остаток буфера — нули.
void copyCredential(const char* value, size_t length, char* out, size_t cap) {
    memset(out, 0, cap);
    size_t copyLength = strnlen(value, length < cap - 1 ? length : cap - 1);
    memcpy(out, value, copyLength);
}

}  // namespace

FormError parseSettingsForm(const char* body, size_t bodyLen,
    const ChannelConfig current[kChannelCount], uint32_t now, bool wifiFieldsVisible,
    SettingsForm& out) {
    memset(&out, 0, sizeof(out));
    if (bodyLen > kMaxFormBody) {
        return FormError::InvalidChannel;
    }
    FieldReader field{body, bodyLen, {}, 0};

    int channelNumber;
    if (!field.readInteger("relay", 1, kChannelCount, channelNumber)) {
        return FormError::InvalidChannel;
    }
    out.channelIndex = static_cast<uint8_t>(channelNumber - 1);
    out.chartDay = field.readChartDay();
    ChannelConfig updated = current[out.channelIndex];

    float safetyCeiling;
    float alwaysOnThreshold;
    int smartHours, wakeupHours, nightHours, forceHours, holidayHours;
    uint16_t wakeupTime, nightStart, nightEnd;
    if (!field.readFloat("safety", 0.0f, 100.0f, safetyCeiling) ||
        !field.readFloat("threshold", -100.0f, 100.0f, alwaysOnThreshold) ||
        !field.readInteger("smart_hours", 1, 24, smartHours) ||
        !field.readInteger("wakeup_hours", 1, 24, wakeupHours) ||
        !field.readTimeOfDay("wakeup_time", wakeupTime) ||
        !field.readTimeOfDay("night_start", nightStart) ||
        !field.readTimeOfDay("night_end", nightEnd) ||
        nightStart == nightEnd ||
        !field.readInteger("night_hours", 1, 24, nightHours) ||
        !field.readInteger("force_hours", 1, 24, forceHours) ||
        !field.readInteger("holiday_hours", 1, 24, holidayHours) ||
        safetyCeiling <= alwaysOnThreshold) {
        return FormError::InvalidScenario;
    }
    uint16_t nightWindowMinutes = nightStart < nightEnd
        ? nightEnd - nightStart
        : 1440 - nightStart + nightEnd;
    if (field.isOne("night_enabled") && nightHours > nightWindowMinutes / 60) {
        return FormError::NightHoursExceedWindow;
    }

    updated.safetyCeilingEurKwh = safetyCeiling;
    updated.alwaysOnBelowPriceEurKwh = alwaysOnThreshold;
    updated.smartThresholdEnabled = field.isOne("smart_enabled");
    updated.smartThresholdHours = static_cast<uint8_t>(smartHours);
    updated.wakeupEnabled = field.isOne("wakeup_enabled");
    updated.wakeupHours = static_cast<uint8_t>(wakeupHours);
    updated.wakeupTimeMinuteOfDay = wakeupTime;
    updated.nightPrepEnabled = field.isOne("night_enabled");
    updated.nightStartMinuteOfDay = nightStart;
    updated.nightEndMinuteOfDay = nightEnd;
    updated.nightCheapHours = static_cast<uint8_t>(nightHours);
    updated.unusedBlockEnabled = false;  // флажок старого «непрерывного блока» (D-006)
    updated.forceOnHours = static_cast<uint8_t>(forceHours);
    updated.holidayEnabled = field.isOne("holiday_enabled");
    updated.holidayOnHours = static_cast<uint8_t>(holidayHours);

    // Пустое поле даты оставляет прежнюю дату возвращения
    field.read("return_local");
    if (field.length > 0 &&
        !rigatime::parseLocalDateTime(field.value, field.length, updated.holidayReturnEpoch)) {
        return FormError::InvalidReturnDate;
    }
    if (updated.holidayEnabled &&
        (updated.holidayReturnEpoch == 0 ||
         (now != 0 && updated.holidayReturnEpoch <= now))) {
        return FormError::ReturnDateNotFuture;
    }

    if (wifiFieldsVisible) {
        field.read("manual_ssid");
        if (field.length == 0) {
            field.read("ssid");
        }
        size_t ssidLength = field.length;
        copyCredential(field.value, field.length, out.ssid, sizeof(out.ssid));
        field.read("password");
        size_t passwordLength = field.length;
        copyCredential(field.value, field.length, out.password, sizeof(out.password));
        if (ssidLength > 32 || passwordLength > 63) {
            return FormError::InvalidWifi;
        }
        out.hasWifi = ssidLength > 0;
    }

    out.restartRequested = strcmp(field.read("action"), "save_restart") == 0;
    out.config = updated;
    return FormError::Ok;
}

bool parseForceForm(const char* body, size_t bodyLen, ForceForm& out) {
    memset(&out, 0, sizeof(out));
    if (bodyLen > kMaxFormBody) {
        return false;
    }
    FieldReader field{body, bodyLen, {}, 0};
    int channelNumber;
    if (!field.readInteger("relay", 1, kChannelCount, channelNumber)) {
        return false;
    }
    out.channelIndex = static_cast<uint8_t>(channelNumber - 1);
    out.chartDay = field.readChartDay();
    const char* action = field.read("action");
    if (strcmp(action, "start") == 0) {
        out.start = true;
        return true;
    }
    return strcmp(action, "stop") == 0;
}

bool parseManualForm(const char* body, size_t bodyLen, ManualForm& out) {
    memset(&out, 0, sizeof(out));
    if (bodyLen > kMaxFormBody) {
        return false;
    }
    FieldReader field{body, bodyLen, {}, 0};
    int channelNumber;
    if (!field.readInteger("relay", 1, kChannelCount, channelNumber)) {
        return false;
    }
    out.channelIndex = static_cast<uint8_t>(channelNumber - 1);
    out.chartDay = field.readChartDay();
    const char* action = field.read("action");
    if (strcmp(action, "on") == 0) {
        out.action = ManualAction::On;
    } else if (strcmp(action, "off") == 0) {
        out.action = ManualAction::Off;
    } else if (strcmp(action, "auto") == 0) {
        out.action = ManualAction::Release;
    } else {
        return false;
    }
    return true;
}

}  // namespace httpparse
