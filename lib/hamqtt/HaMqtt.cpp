#include "HaMqtt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace hamqtt {

namespace {

const char kModeAuto[] = "Авто";
const char kModeOn[] = "Вкл";
const char kModeOff[] = "Выкл";

// Объекты канала в порядке описаний
enum class ChannelEntity : uint8_t { Output, Mode, Reason, Force, Stop };
constexpr uint8_t kEntitiesPerChannel = 5;

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
    return false;
}

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
