#include "HaMqtt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <RigaTime.h>

namespace hamqtt {

using channelconfig::ChannelConfig;

namespace {

const char kModeAuto[] = "Авто";
const char kModeOn[] = "Вкл";
const char kModeOff[] = "Выкл";

// Управляющие объекты канала в порядке описаний; за ними — kSettingCount настроек
enum class ChannelEntity : uint8_t { Output, Mode, Reason, Force, Stop };
constexpr uint8_t kControlEntities = 5;

// Как настройка выглядит в Home Assistant
enum class SettingKind : uint8_t { Price, Hours, Switch, Time, DateTime };
struct SettingInfo {
    Setting setting;
    const char* key;
    const char* name;
    SettingKind kind;
    float minimum;
    float maximum;
};
// Пределы — как у формы страницы (channelconfig); часы отпуска — до 10, как поле страницы
const SettingInfo kSettings[kSettingCount] = {
    {Setting::SafetyCeiling, "safety", "аварийный предел", SettingKind::Price,
        channelconfig::kSafetyCeilingMin, channelconfig::kSafetyCeilingMax},
    {Setting::Threshold, "threshold", "порог включения", SettingKind::Price,
        channelconfig::kThresholdMin, channelconfig::kThresholdMax},
    {Setting::SmartEnabled, "smart", "умный порог", SettingKind::Switch, 0, 0},
    {Setting::SmartHours, "smart_h", "умный порог, часов", SettingKind::Hours,
        channelconfig::kHoursMin, channelconfig::kHoursMax},
    {Setting::WakeupEnabled, "wake", "пробуждение", SettingKind::Switch, 0, 0},
    {Setting::WakeupHours, "wake_h", "пробуждение, часов", SettingKind::Hours,
        channelconfig::kHoursMin, channelconfig::kHoursMax},
    {Setting::WakeupTime, "wake_t", "время пробуждения", SettingKind::Time, 0, 0},
    {Setting::NightEnabled, "night", "включение ночью", SettingKind::Switch, 0, 0},
    {Setting::NightStart, "night_s", "ночь, начало", SettingKind::Time, 0, 0},
    {Setting::NightEnd, "night_e", "ночь, конец", SettingKind::Time, 0, 0},
    {Setting::NightHours, "night_h", "ночь, часов", SettingKind::Hours,
        channelconfig::kHoursMin, channelconfig::kHoursMax},
    {Setting::ForceHours, "force_h", "принудительно, часов", SettingKind::Hours,
        channelconfig::kHoursMin, channelconfig::kHoursMax},
    {Setting::HolidayEnabled, "hol", "отпуск", SettingKind::Switch, 0, 0},
    {Setting::HolidayReturn, "hol_ret", "отпуск, возвращение", SettingKind::DateTime, 0, 0},
    {Setting::HolidayHours, "hol_h", "отпуск, часов", SettingKind::Hours,
        channelconfig::kHoursMin, 10},
};

const SettingInfo* findSetting(const char* key, size_t length) {
    for (const SettingInfo& info : kSettings) {
        if (strlen(info.key) == length && memcmp(info.key, key, length) == 0) return &info;
    }
    return nullptr;
}

// Число из текста целиком (strtof без хвоста); текст без нуля в конце — копия.
bool parseNumber(const char* text, size_t length, float& out) {
    char buffer[kCommandValueSize];
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length);
    buffer[length] = 0;
    char* end = nullptr;
    out = strtof(buffer, &end);
    return end == buffer + length && isfinite(out);
}

bool parseSwitch(const char* text, size_t length, bool& out) {
    if (length == 2 && memcmp(text, "ON", 2) == 0) {
        out = true;
        return true;
    }
    if (length == 3 && memcmp(text, "OFF", 3) == 0) {
        out = false;
        return true;
    }
    return false;
}

// Общая часть описания: доступность и устройство, к которому Home Assistant привяжет
// сущность. Возвращает длину или -1, если не поместилось.
int printDevice(char* out, size_t cap, const char* deviceId, const char* firmwareVersion) {
    return snprintf(out, cap,
        "\"avty_t\":\"opta/%s/status\","
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Opta\",\"mf\":\"Arduino\","
        "\"mdl\":\"Opta WiFi\",\"sw\":\"%s\"}}",
        deviceId, deviceId, firmwareVersion);
}

bool fits(int written, size_t cap) {
    return written >= 0 && static_cast<size_t>(written) < cap;
}

}  // namespace

void formatDeviceId(uint32_t uid0, uint32_t uid1, uint32_t uid2, char* out, size_t cap) {
    // FNV-1a по 12 байтам номера: у соседних плат номера отличаются в немногих битах
    const uint32_t words[3] = {uid0, uid1, uid2};
    uint32_t hash = 2166136261UL;
    for (uint32_t word : words) {
        for (uint8_t shift = 0; shift < 32; shift += 8) {
            hash ^= (word >> shift) & 0xFFU;
            hash *= 16777619UL;
        }
    }
    snprintf(out, cap, "opta_%06lx", static_cast<unsigned long>(hash & 0xFFFFFFUL));
}

void availabilityTopic(const char* deviceId, char* out, size_t cap) {
    snprintf(out, cap, "opta/%s/status", deviceId);
}

void priceTopic(const char* deviceId, char* out, size_t cap) {
    snprintf(out, cap, "opta/%s/price", deviceId);
}

void commandFilter(const char* deviceId, char* out, size_t cap) {
    snprintf(out, cap, "opta/%s/relay/+/+/set", deviceId);
}

void channelTopic(const char* deviceId, uint8_t channel, const char* leaf, char* out, size_t cap) {
    snprintf(out, cap, "opta/%s/relay/%u/%s", deviceId, channel + 1U, leaf);
}

const char* modeOption(Mode mode) {
    switch (mode) {
        case Mode::Auto: return kModeAuto;
        case Mode::On: return kModeOn;
        case Mode::Off: return kModeOff;
    }
    return kModeAuto;
}

bool parseModeOption(const char* text, size_t length, Mode& out) {
    const Mode modes[] = {Mode::Auto, Mode::On, Mode::Off};
    for (Mode mode : modes) {
        const char* option = modeOption(mode);
        if (strlen(option) == length && memcmp(option, text, length) == 0) {
            out = mode;
            return true;
        }
    }
    return false;
}

Mode channelMode(bool manual, bool output) {
    if (!manual) return Mode::Auto;
    return output ? Mode::On : Mode::Off;
}

bool parseCommand(const char* deviceId, const char* topic, const char* payload,
    size_t payloadLength, Command& out) {
    char prefix[kTopicSize];
    snprintf(prefix, sizeof(prefix), "opta/%s/relay/", deviceId);
    const size_t prefixLength = strlen(prefix);
    if (strncmp(topic, prefix, prefixLength) != 0) return false;

    // <N>/<действие>/set; N — одна цифра 1…kChannelCount
    const char* rest = topic + prefixLength;
    if (rest[0] < '1' || rest[0] > static_cast<char>('0' + kChannelCount) || rest[1] != '/') {
        return false;
    }
    out.channel = static_cast<uint8_t>(rest[0] - '1');
    const char* action = rest + 2;
    if (strcmp(action, "mode/set") == 0) {
        out.type = CommandType::SetMode;
        return parseModeOption(payload, payloadLength, out.mode);
    }
    out.mode = Mode::Auto;
    if (strcmp(action, "force/set") == 0) {
        out.type = CommandType::ForceOn;
        return true;
    }
    if (strcmp(action, "stop/set") == 0) {
        out.type = CommandType::ForceStop;
        return true;
    }
    // <ключ настройки>/set
    const char* slash = strchr(action, '/');
    if (slash == nullptr || strcmp(slash, "/set") != 0) return false;
    const SettingInfo* info = findSetting(action, static_cast<size_t>(slash - action));
    if (info == nullptr || payloadLength >= kCommandValueSize) return false;
    out.type = CommandType::SetSetting;
    out.setting = info->setting;
    memcpy(out.value, payload, payloadLength);
    out.value[payloadLength] = 0;
    out.valueLength = static_cast<uint8_t>(payloadLength);
    return true;
}

const char* settingKey(Setting setting) {
    for (const SettingInfo& info : kSettings) {
        if (info.setting == setting) return info.key;
    }
    return "";
}

bool applySetting(ChannelConfig& config, Setting setting, const char* text, size_t length) {
    const SettingInfo* info = nullptr;
    for (const SettingInfo& candidate : kSettings) {
        if (candidate.setting == setting) info = &candidate;
    }
    if (info == nullptr) return false;

    float number = 0.0f;
    uint8_t hours = 0;
    bool enabled = false;
    uint16_t minuteOfDay = 0;
    switch (info->kind) {
        case SettingKind::Price:
            if (!parseNumber(text, length, number) || number < info->minimum ||
                number > info->maximum) {
                return false;
            }
            break;
        case SettingKind::Hours:
            // Home Assistant присылает "3" или "3.0"
            if (!parseNumber(text, length, number) || number != floorf(number) ||
                number < info->minimum || number > info->maximum) {
                return false;
            }
            hours = static_cast<uint8_t>(number);
            break;
        case SettingKind::Switch:
            if (!parseSwitch(text, length, enabled)) return false;
            break;
        case SettingKind::Time:
            if (!rigatime::parseTimeOfDay(text, length, minuteOfDay)) return false;
            break;
        case SettingKind::DateTime: {
            // Как поле страницы (ГГГГ-ММ-ДДTЧЧ:ММ); вместо T можно пробел
            char buffer[17];
            if (length != 16) return false;
            memcpy(buffer, text, length);
            if (buffer[10] == ' ') buffer[10] = 'T';
            uint32_t epoch = 0;
            if (!rigatime::parseLocalDateTime(buffer, length, epoch)) return false;
            config.holidayReturnEpoch = epoch;
            return true;
        }
    }

    switch (setting) {
        case Setting::SafetyCeiling: config.safetyCeilingEurKwh = number; break;
        case Setting::Threshold: config.alwaysOnBelowPriceEurKwh = number; break;
        case Setting::SmartEnabled: config.smartThresholdEnabled = enabled; break;
        case Setting::SmartHours: config.smartThresholdHours = hours; break;
        case Setting::WakeupEnabled: config.wakeupEnabled = enabled; break;
        case Setting::WakeupHours: config.wakeupHours = hours; break;
        case Setting::WakeupTime: config.wakeupTimeMinuteOfDay = minuteOfDay; break;
        case Setting::NightEnabled: config.nightPrepEnabled = enabled; break;
        case Setting::NightStart: config.nightStartMinuteOfDay = minuteOfDay; break;
        case Setting::NightEnd: config.nightEndMinuteOfDay = minuteOfDay; break;
        case Setting::NightHours: config.nightCheapHours = hours; break;
        case Setting::ForceHours: config.forceOnHours = hours; break;
        case Setting::HolidayEnabled: config.holidayEnabled = enabled; break;
        case Setting::HolidayHours: config.holidayOnHours = hours; break;
        case Setting::HolidayReturn: break;  // выше
    }
    return true;
}

bool formatSettingsJson(const ChannelConfig& config, char* out, size_t cap) {
    char safety[16];
    char threshold[16];
    formatPrice(config.safetyCeilingEurKwh, safety, sizeof(safety));
    formatPrice(config.alwaysOnBelowPriceEurKwh, threshold, sizeof(threshold));
    char holidayReturn[17] = "";
    if (config.holidayReturnEpoch != 0) {
        rigatime::formatLocalDateTime(config.holidayReturnEpoch, holidayReturn,
            sizeof(holidayReturn));
    }
    const auto onOff = [](bool value) { return value ? "ON" : "OFF"; };
    const int written = snprintf(out, cap,
        "{\"safety\":%s,\"threshold\":%s,\"smart\":\"%s\",\"smart_h\":%u,"
        "\"wake\":\"%s\",\"wake_h\":%u,\"wake_t\":\"%02u:%02u\","
        "\"night\":\"%s\",\"night_s\":\"%02u:%02u\",\"night_e\":\"%02u:%02u\",\"night_h\":%u,"
        "\"force_h\":%u,\"hol\":\"%s\",\"hol_ret\":\"%s\",\"hol_h\":%u}",
        safety, threshold, onOff(config.smartThresholdEnabled), config.smartThresholdHours,
        onOff(config.wakeupEnabled), config.wakeupHours,
        config.wakeupTimeMinuteOfDay / 60U, config.wakeupTimeMinuteOfDay % 60U,
        onOff(config.nightPrepEnabled),
        config.nightStartMinuteOfDay / 60U, config.nightStartMinuteOfDay % 60U,
        config.nightEndMinuteOfDay / 60U, config.nightEndMinuteOfDay % 60U,
        config.nightCheapHours, config.forceOnHours, onOff(config.holidayEnabled),
        holidayReturn, config.holidayOnHours);
    return fits(written, cap);
}

namespace {

// Описание настройки: категория «Настройки» в Home Assistant, значение — поле JSON
// состояния opta/<id>/relay/N/settings.
bool settingDiscovery(const SettingInfo& info, unsigned number, const char* deviceId,
    char* topic, size_t topicCap, char* payload, size_t payloadCap, int& written) {
    const char* platform = "number";
    switch (info.kind) {
        case SettingKind::Price:
        case SettingKind::Hours: platform = "number"; break;
        case SettingKind::Switch: platform = "switch"; break;
        case SettingKind::Time:
        case SettingKind::DateTime: platform = "text"; break;
    }
    if (!fits(snprintf(topic, topicCap, "homeassistant/%s/%s/relay%u_%s/config",
            platform, deviceId, number, info.key), topicCap)) {
        return false;
    }
    written = snprintf(payload, payloadCap,
        "{\"name\":\"Реле %u: %s\",\"uniq_id\":\"%s_relay%u_%s\","
        "\"default_entity_id\":\"%s.%s_relay%u_%s\",\"entity_category\":\"config\","
        "\"stat_t\":\"opta/%s/relay/%u/settings\",\"val_tpl\":\"{{ value_json.%s }}\","
        "\"cmd_t\":\"opta/%s/relay/%u/%s/set\",",
        number, info.name, deviceId, number, info.key, platform, deviceId, number, info.key,
        deviceId, number, info.key, deviceId, number, info.key);
    if (!fits(written, payloadCap)) return false;
    int extra = 0;
    switch (info.kind) {
        case SettingKind::Price:
            extra = snprintf(payload + written, payloadCap - written,
                "\"min\":%d,\"max\":%d,\"step\":0.001,\"mode\":\"box\","
                "\"unit_of_meas\":\"EUR/kWh\",",
                static_cast<int>(info.minimum), static_cast<int>(info.maximum));
            break;
        case SettingKind::Hours:
            extra = snprintf(payload + written, payloadCap - written,
                "\"min\":%d,\"max\":%d,\"step\":1,\"mode\":\"box\",\"unit_of_meas\":\"ч\",",
                static_cast<int>(info.minimum), static_cast<int>(info.maximum));
            break;
        case SettingKind::Switch:
            extra = snprintf(payload + written, payloadCap - written, "%s", "");
            break;
        case SettingKind::Time:
            extra = snprintf(payload + written, payloadCap - written,
                "\"min\":5,\"max\":5,\"pattern\":\"^[0-2][0-9]:[0-5][0-9]$\",\"ic\":\"mdi:clock-outline\",");
            break;
        case SettingKind::DateTime:
            extra = snprintf(payload + written, payloadCap - written,
                "\"min\":0,\"max\":16,\"pattern\":\"^$|^[0-9]{4}-[0-9]{2}-[0-9]{2}[T ][0-2][0-9]:[0-5][0-9]$\","
                "\"ic\":\"mdi:calendar-clock\",");
            break;
    }
    if (!fits(extra, payloadCap - written)) return false;
    written += extra;
    return true;
}

}  // namespace

bool discoveryMessage(uint8_t index, const char* deviceId, const char* firmwareVersion,
    char* topic, size_t topicCap, char* payload, size_t payloadCap) {
    if (index >= kDiscoveryCount) return false;
    int written = 0;
    if (index == 0) {
        if (!fits(snprintf(topic, topicCap, "homeassistant/sensor/%s/price/config", deviceId), topicCap)) {
            return false;
        }
        written = snprintf(payload, payloadCap,
            "{\"name\":\"Цена\",\"uniq_id\":\"%s_price\","
            "\"default_entity_id\":\"sensor.%s_price\",\"stat_t\":\"opta/%s/price\","
            "\"unit_of_meas\":\"EUR/kWh\",\"sug_dsp_prc\":4,\"ic\":\"mdi:cash\",",
            deviceId, deviceId, deviceId);
    } else if ((index - 1) % kEntitiesPerChannel >= kControlEntities) {
        const unsigned number = (index - 1) / kEntitiesPerChannel + 1U;
        const SettingInfo& info = kSettings[(index - 1) % kEntitiesPerChannel - kControlEntities];
        if (!settingDiscovery(info, number, deviceId, topic, topicCap, payload, payloadCap,
                written)) {
            return false;
        }
    } else {
        const uint8_t channel = static_cast<uint8_t>((index - 1) / kEntitiesPerChannel);
        const ChannelEntity entity = static_cast<ChannelEntity>((index - 1) % kEntitiesPerChannel);
        const unsigned number = channel + 1U;
        const char* platform = "sensor";
        const char* object = "";
        switch (entity) {
            case ChannelEntity::Output: platform = "binary_sensor"; object = "output"; break;
            case ChannelEntity::Mode: platform = "select"; object = "mode"; break;
            case ChannelEntity::Reason: platform = "sensor"; object = "reason"; break;
            case ChannelEntity::Force: platform = "button"; object = "force"; break;
            case ChannelEntity::Stop: platform = "button"; object = "stop"; break;
        }
        if (!fits(snprintf(topic, topicCap, "homeassistant/%s/%s/relay%u_%s/config",
                platform, deviceId, number, object), topicCap)) {
            return false;
        }
        // Общее начало: имя, уникальный id и id сущности в Home Assistant
        const char* name = "";
        switch (entity) {
            case ChannelEntity::Output: name = "выход"; break;
            case ChannelEntity::Mode: name = "режим"; break;
            case ChannelEntity::Reason: name = "причина"; break;
            case ChannelEntity::Force: name = "принудительно включить"; break;
            case ChannelEntity::Stop: name = "остановить принудительное"; break;
        }
        written = snprintf(payload, payloadCap,
            "{\"name\":\"Реле %u: %s\",\"uniq_id\":\"%s_relay%u_%s\","
            "\"default_entity_id\":\"%s.%s_relay%u_%s\",",
            number, name, deviceId, number, object, platform, deviceId, number, object);
        if (!fits(written, payloadCap)) return false;
        const char* specific = "";
        char buffer[256];
        switch (entity) {
            case ChannelEntity::Output:
                snprintf(buffer, sizeof(buffer),
                    "\"stat_t\":\"opta/%s/relay/%u/state\",\"dev_cla\":\"power\",", deviceId, number);
                specific = buffer;
                break;
            case ChannelEntity::Mode:
                snprintf(buffer, sizeof(buffer),
                    "\"stat_t\":\"opta/%s/relay/%u/mode\",\"cmd_t\":\"opta/%s/relay/%u/mode/set\","
                    "\"ops\":[\"%s\",\"%s\",\"%s\"],\"ic\":\"mdi:hand-back-right\",",
                    deviceId, number, deviceId, number, kModeAuto, kModeOn, kModeOff);
                specific = buffer;
                break;
            case ChannelEntity::Reason:
                snprintf(buffer, sizeof(buffer),
                    "\"stat_t\":\"opta/%s/relay/%u/reason\",\"ic\":\"mdi:information-outline\",",
                    deviceId, number);
                specific = buffer;
                break;
            case ChannelEntity::Force:
                snprintf(buffer, sizeof(buffer),
                    "\"cmd_t\":\"opta/%s/relay/%u/force/set\",\"ic\":\"mdi:timer-play\",",
                    deviceId, number);
                specific = buffer;
                break;
            case ChannelEntity::Stop:
                snprintf(buffer, sizeof(buffer),
                    "\"cmd_t\":\"opta/%s/relay/%u/stop/set\",\"ic\":\"mdi:timer-stop\",",
                    deviceId, number);
                specific = buffer;
                break;
        }
        const int extra = snprintf(payload + written, payloadCap - written, "%s", specific);
        if (!fits(extra, payloadCap - written)) return false;
        written += extra;
    }
    if (!fits(written, payloadCap)) return false;
    const int device = printDevice(payload + written, payloadCap - written, deviceId, firmwareVersion);
    return fits(device, payloadCap - written);
}

void formatPrice(float eurKwh, char* out, size_t cap) {
    const long scaled = lroundf(eurKwh * 100000.0f);
    const unsigned long magnitude = static_cast<unsigned long>(scaled < 0 ? -scaled : scaled);
    snprintf(out, cap, "%s%lu.%05lu", scaled < 0 ? "-" : "",
        magnitude / 100000UL, magnitude % 100000UL);
}

}  // namespace hamqtt
