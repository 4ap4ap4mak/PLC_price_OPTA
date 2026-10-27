// Тесты lib/hamqtt: Home Assistant через MQTT (D-042, D-043) — идентификатор
// устройства, топики, разбор команд, описания сущностей (корректный JSON), цена.
#include <unity.h>

#include <ArduinoJson.h>
#include <string.h>

#include <HaMqtt.h>

#include "../helpers/TestHelpers.h"

using namespace hamqtt;

static uint32_t testing_riga_2026_11_05_1800() {
    return testing::riga(2026, 11, 5, 18, 0);
}

void setUp() {}
void tearDown() {}

static const char kId[] = "opta_a1b2c3";

static void test_device_id() {
    char id[kDeviceIdSize];
    formatDeviceId(0x00230041, 0x3133510F, 0x31383930, id, sizeof(id));
    TEST_ASSERT_EQUAL_UINT32(11, strlen(id));
    TEST_ASSERT_EQUAL_MEMORY("opta_", id, 5);
    char same[kDeviceIdSize];
    formatDeviceId(0x00230041, 0x3133510F, 0x31383930, same, sizeof(same));
    TEST_ASSERT_EQUAL_STRING(id, same);
    // Соседняя плата — другой номер
    char other[kDeviceIdSize];
    formatDeviceId(0x00230042, 0x3133510F, 0x31383930, other, sizeof(other));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(id, other));
}

static void test_topics() {
    char topic[kTopicSize];
    availabilityTopic(kId, topic, sizeof(topic));
    TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/status", topic);
    priceTopic(kId, topic, sizeof(topic));
    TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/price", topic);
    commandFilter(kId, topic, sizeof(topic));
    TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/relay/+/+/set", topic);
    channelTopic(kId, 3, "mode", topic, sizeof(topic));
    TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/relay/4/mode", topic);
}

static void test_mode_options() {
    Mode mode = Mode::Auto;
    TEST_ASSERT_TRUE(parseModeOption("Вкл", strlen("Вкл"), mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mode::On), static_cast<int>(mode));
    TEST_ASSERT_TRUE(parseModeOption("Выкл", strlen("Выкл"), mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mode::Off), static_cast<int>(mode));
    TEST_ASSERT_TRUE(parseModeOption("Авто", strlen("Авто"), mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mode::Auto), static_cast<int>(mode));
    TEST_ASSERT_FALSE(parseModeOption("Вк", strlen("Вк"), mode));
    TEST_ASSERT_FALSE(parseModeOption("auto", 4, mode));
    TEST_ASSERT_FALSE(parseModeOption("", 0, mode));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mode::Auto), static_cast<int>(channelMode(false, true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mode::On), static_cast<int>(channelMode(true, true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mode::Off), static_cast<int>(channelMode(true, false)));
}

static void test_parse_commands() {
    Command command;
    const char on[] = "Вкл";
    TEST_ASSERT_TRUE(parseCommand(kId, "opta/opta_a1b2c3/relay/2/mode/set", on, strlen(on), command));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandType::SetMode), static_cast<int>(command.type));
    TEST_ASSERT_EQUAL_UINT8(1, command.channel);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mode::On), static_cast<int>(command.mode));

    TEST_ASSERT_TRUE(parseCommand(kId, "opta/opta_a1b2c3/relay/1/force/set", "PRESS", 5, command));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandType::ForceOn), static_cast<int>(command.type));
    TEST_ASSERT_EQUAL_UINT8(0, command.channel);

    TEST_ASSERT_TRUE(parseCommand(kId, "opta/opta_a1b2c3/relay/4/stop/set", "", 0, command));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandType::ForceStop), static_cast<int>(command.type));
    TEST_ASSERT_EQUAL_UINT8(3, command.channel);

    const char* bad[] = {
        "opta/opta_other/relay/1/force/set",   // другое устройство
        "opta/opta_a1b2c3/relay/0/force/set",  // нет канала 0
        "opta/opta_a1b2c3/relay/5/force/set",  // каналов 4
        "opta/opta_a1b2c3/relay/12/force/set",
        "opta/opta_a1b2c3/relay/1/force",
        "opta/opta_a1b2c3/relay/1/reboot/set",
        "opta/opta_a1b2c3/price",
    };
    for (const char* topic : bad) {
        TEST_ASSERT_FALSE_MESSAGE(parseCommand(kId, topic, "PRESS", 5, command), topic);
    }
    const char unknown[] = "Turbo";
    TEST_ASSERT_FALSE(parseCommand(kId, "opta/opta_a1b2c3/relay/1/mode/set", unknown,
        strlen(unknown), command));
}

// Каждое описание — корректный JSON с уникальным id, доступностью и устройством
static void test_discovery_messages() {
    char topic[kTopicSize];
    char payload[kDiscoveryPayloadSize];
    char seenIds[kDiscoveryCount][48] = {};
    for (uint8_t i = 0; i < kDiscoveryCount; ++i) {
        TEST_ASSERT_TRUE(discoveryMessage(i, kId, "1.0.4", topic, sizeof(topic), payload, sizeof(payload)));
        TEST_ASSERT_EQUAL_MEMORY("homeassistant/", topic, 14);
        TEST_ASSERT_NOT_NULL(strstr(topic, "/opta_a1b2c3/"));
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, static_cast<int>(deserializeJson(doc, payload).code()), payload);
        TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/status", doc["avty_t"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("opta_a1b2c3", doc["dev"]["ids"][0].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("1.0.4", doc["dev"]["sw"].as<const char*>());
        const char* uniqueId = doc["uniq_id"].as<const char*>();
        TEST_ASSERT_NOT_NULL(uniqueId);
        for (uint8_t j = 0; j < i; ++j) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(seenIds[j], uniqueId), uniqueId);
        }
        strncpy(seenIds[i], uniqueId, sizeof(seenIds[i]) - 1);
        // Топик описания и тип сущности совпадают
        const char* platform = strchr(doc["default_entity_id"].as<const char*>(), '.');
        TEST_ASSERT_NOT_NULL(platform);
    }
    TEST_ASSERT_FALSE(discoveryMessage(kDiscoveryCount, kId, "1.0.4", topic, sizeof(topic),
        payload, sizeof(payload)));
}

static void test_discovery_select_and_buttons() {
    char topic[kTopicSize];
    char payload[kDiscoveryPayloadSize];
    // Реле 2: режим — индекс 1 + kEntitiesPerChannel * 1 + 1
    const uint8_t relay2Mode = 1 + kEntitiesPerChannel + 1;
    TEST_ASSERT_TRUE(discoveryMessage(relay2Mode, kId, "1.0.4", topic, sizeof(topic), payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/select/opta_a1b2c3/relay2_mode/config", topic);
    JsonDocument doc;
    deserializeJson(doc, payload);
    TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/relay/2/mode/set", doc["cmd_t"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Авто", doc["ops"][0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Выкл", doc["ops"][2].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("select.opta_a1b2c3_relay2_mode", doc["default_entity_id"].as<const char*>());

    // Реле 4: стоп — пятый объект последнего канала
    const uint8_t relay4Stop = 1 + kEntitiesPerChannel * 3 + 4;
    TEST_ASSERT_TRUE(discoveryMessage(relay4Stop, kId, "1.0.4", topic, sizeof(topic),
        payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/button/opta_a1b2c3/relay4_stop/config", topic);
    deserializeJson(doc, payload);
    TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/relay/4/stop/set", doc["cmd_t"].as<const char*>());

    // Мало места — false, а не обрезанный JSON
    char small[64];
    TEST_ASSERT_FALSE(discoveryMessage(relay2Mode, kId, "1.0.4", topic, sizeof(topic), small, sizeof(small)));
}

// D-044: настройки — категория config, значение из JSON настроек канала
static void test_discovery_settings() {
    char topic[kTopicSize];
    char payload[kDiscoveryPayloadSize];
    JsonDocument doc;
    // Реле 1: аварийный предел — первая настройка
    TEST_ASSERT_TRUE(discoveryMessage(1 + 5, kId, "1.0.4", topic, sizeof(topic), payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/number/opta_a1b2c3/relay1_safety/config", topic);
    deserializeJson(doc, payload);
    TEST_ASSERT_EQUAL_STRING("config", doc["entity_category"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/relay/1/settings", doc["stat_t"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("{{ value_json.safety }}", doc["val_tpl"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("opta/opta_a1b2c3/relay/1/safety/set", doc["cmd_t"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(100, doc["max"].as<int>());

    // Последнее описание — Реле 4: отпуск, часов (до 10, как поле страницы)
    TEST_ASSERT_TRUE(discoveryMessage(kDiscoveryCount - 1, kId, "1.0.4", topic, sizeof(topic),
        payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/number/opta_a1b2c3/relay4_hol_h/config", topic);
    deserializeJson(doc, payload);
    TEST_ASSERT_EQUAL_INT(10, doc["max"].as<int>());

    // Реле 3: время пробуждения — text с шаблоном
    TEST_ASSERT_TRUE(discoveryMessage(1 + kEntitiesPerChannel * 2 + 5 + 6, kId, "1.0.4", topic,
        sizeof(topic), payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/text/opta_a1b2c3/relay3_wake_t/config", topic);
    deserializeJson(doc, payload);
    TEST_ASSERT_NOT_NULL(doc["pattern"].as<const char*>());
}

static channelconfig::ChannelConfig baseConfig() {
    channelconfig::ChannelConfig config;
    channelconfig::setDefaults(config);
    return config;
}

static bool apply(channelconfig::ChannelConfig& config, Setting setting, const char* text) {
    return applySetting(config, setting, text, strlen(text));
}

static void test_apply_settings() {
    channelconfig::ChannelConfig config = baseConfig();
    TEST_ASSERT_TRUE(apply(config, Setting::SafetyCeiling, "0.35"));
    TEST_ASSERT_EQUAL_FLOAT(0.35f, config.safetyCeilingEurKwh);
    TEST_ASSERT_TRUE(apply(config, Setting::Threshold, "-0.01"));
    TEST_ASSERT_EQUAL_FLOAT(-0.01f, config.alwaysOnBelowPriceEurKwh);
    TEST_ASSERT_TRUE(apply(config, Setting::SmartHours, "6"));
    TEST_ASSERT_EQUAL_UINT8(6, config.smartThresholdHours);
    TEST_ASSERT_TRUE(apply(config, Setting::NightHours, "5.0"));
    TEST_ASSERT_EQUAL_UINT8(5, config.nightCheapHours);
    TEST_ASSERT_TRUE(apply(config, Setting::SmartEnabled, "ON"));
    TEST_ASSERT_TRUE(config.smartThresholdEnabled);
    TEST_ASSERT_TRUE(apply(config, Setting::SmartEnabled, "OFF"));
    TEST_ASSERT_FALSE(config.smartThresholdEnabled);
    TEST_ASSERT_TRUE(apply(config, Setting::WakeupTime, "06:45"));
    TEST_ASSERT_EQUAL_UINT16(6 * 60 + 45, config.wakeupTimeMinuteOfDay);
    TEST_ASSERT_TRUE(apply(config, Setting::NightStart, "22:30"));
    TEST_ASSERT_EQUAL_UINT16(22 * 60 + 30, config.nightStartMinuteOfDay);
    TEST_ASSERT_TRUE(apply(config, Setting::HolidayReturn, "2026-11-05T18:00"));
    TEST_ASSERT_EQUAL_UINT32(testing_riga_2026_11_05_1800(), config.holidayReturnEpoch);
    TEST_ASSERT_TRUE(apply(config, Setting::HolidayReturn, "2026-11-06 18:00"));
    TEST_ASSERT_EQUAL_UINT32(testing_riga_2026_11_05_1800() + 86400UL, config.holidayReturnEpoch);
    TEST_ASSERT_TRUE(apply(config, Setting::HolidayHours, "10"));
    TEST_ASSERT_EQUAL_UINT8(10, config.holidayOnHours);

    const channelconfig::ChannelConfig before = config;
    const struct { Setting setting; const char* text; } bad[] = {
        {Setting::SafetyCeiling, "101"}, {Setting::SafetyCeiling, "-1"},
        {Setting::SafetyCeiling, "abc"}, {Setting::SafetyCeiling, ""},
        {Setting::SafetyCeiling, "0.3x"}, {Setting::Threshold, "nan"},
        {Setting::SmartHours, "0"}, {Setting::SmartHours, "25"}, {Setting::SmartHours, "2.5"},
        {Setting::HolidayHours, "11"}, {Setting::SmartEnabled, "on"},
        {Setting::SmartEnabled, "1"}, {Setting::WakeupTime, "24:00"},
        {Setting::WakeupTime, "6:45"}, {Setting::HolidayReturn, "2026-11-05"},
        {Setting::HolidayReturn, "2026-13-05T18:00"},
    };
    for (const auto& c : bad) {
        TEST_ASSERT_FALSE_MESSAGE(apply(config, c.setting, c.text), c.text);
    }
    TEST_ASSERT_EQUAL_MEMORY(&before, &config, sizeof(config));
}

static void test_settings_json() {
    channelconfig::ChannelConfig config = baseConfig();
    config.safetyCeilingEurKwh = 0.4f;
    config.smartThresholdEnabled = true;
    config.wakeupTimeMinuteOfDay = 6 * 60 + 5;
    config.holidayReturnEpoch = testing_riga_2026_11_05_1800();
    char json[kSettingsJsonSize];
    TEST_ASSERT_TRUE(formatSettingsJson(config, json, sizeof(json)));
    JsonDocument doc;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, static_cast<int>(deserializeJson(doc, json).code()), json);
    TEST_ASSERT_EQUAL_FLOAT(0.4f, doc["safety"].as<float>());
    TEST_ASSERT_EQUAL_STRING("ON", doc["smart"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("06:05", doc["wake_t"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("2026-11-05T18:00", doc["hol_ret"].as<const char*>());
    // Каждый ключ настройки есть в JSON и разбирается обратно
    for (uint8_t i = 0; i < kSettingCount; ++i) {
        const Setting setting = static_cast<Setting>(i);
        TEST_ASSERT_TRUE_MESSAGE(doc[settingKey(setting)].is<JsonVariant>() &&
            !doc[settingKey(setting)].isNull(), settingKey(setting));
    }
    config.holidayReturnEpoch = 0;
    TEST_ASSERT_TRUE(formatSettingsJson(config, json, sizeof(json)));
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("", doc["hol_ret"].as<const char*>());
    char small[64];
    TEST_ASSERT_FALSE(formatSettingsJson(config, small, sizeof(small)));
}

static void test_parse_setting_command() {
    Command command;
    TEST_ASSERT_TRUE(parseCommand(kId, "opta/opta_a1b2c3/relay/3/night_s/set", "23:15", 5, command));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandType::SetSetting), static_cast<int>(command.type));
    TEST_ASSERT_EQUAL_UINT8(2, command.channel);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Setting::NightStart), static_cast<int>(command.setting));
    TEST_ASSERT_EQUAL_STRING("23:15", command.value);
    TEST_ASSERT_EQUAL_UINT8(5, command.valueLength);

    TEST_ASSERT_FALSE(parseCommand(kId, "opta/opta_a1b2c3/relay/1/unknown/set", "1", 1, command));
    TEST_ASSERT_FALSE(parseCommand(kId, "opta/opta_a1b2c3/relay/1/safety", "1", 1, command));
    char tooLong[kCommandValueSize + 1];
    memset(tooLong, '1', sizeof(tooLong));
    TEST_ASSERT_FALSE(parseCommand(kId, "opta/opta_a1b2c3/relay/1/safety/set", tooLong,
        kCommandValueSize, command));
}

static void test_format_price() {
    char text[16];
    formatPrice(0.01161f, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("0.01161", text);
    formatPrice(-0.00123f, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("-0.00123", text);
    formatPrice(1.5f, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("1.50000", text);
    formatPrice(0.0f, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("0.00000", text);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_device_id);
    RUN_TEST(test_topics);
    RUN_TEST(test_mode_options);
    RUN_TEST(test_parse_commands);
    RUN_TEST(test_discovery_messages);
    RUN_TEST(test_discovery_select_and_buttons);
    RUN_TEST(test_format_price);
    RUN_TEST(test_discovery_settings);
    RUN_TEST(test_apply_settings);
    RUN_TEST(test_settings_json);
    RUN_TEST(test_parse_setting_command);
    return UNITY_END();
}
