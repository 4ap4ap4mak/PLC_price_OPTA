// Home Assistant через MQTT (D-042, D-043): идентификатор устройства, топики,
// описания сущностей (MQTT Discovery), разбор команд и тексты состояний.
// Чистый модуль L0: без Arduino и сети, не логирует; соединение — src/integrations/MqttLink.
//
// Топики: opta/<id>/status (online/offline, завещание), opta/<id>/price,
// opta/<id>/relay/<N>/state (ON/OFF), .../mode (Авто/Вкл/Выкл), .../reason (текст),
// команды .../mode/set (Авто/Вкл/Выкл), .../force/set и .../stop/set (PRESS).
// Описания — homeassistant/<платформа>/<id>/<объект>/config, retained.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ChannelConfig.h>
#include <Planner.h>
#include <PriceSeries.h>

namespace hamqtt {

using channelconfig::kChannelCount;

// "opta_" + 6 шестнадцатеричных цифр из уникального номера микроконтроллера (96 бит).
constexpr size_t kDeviceIdSize = 12;
void formatDeviceId(uint32_t uid0, uint32_t uid1, uint32_t uid2, char* out, size_t cap);

constexpr size_t kTopicSize = 96;
constexpr size_t kDiscoveryPayloadSize = 640;

void availabilityTopic(const char* deviceId, char* out, size_t cap);
void priceTopic(const char* deviceId, char* out, size_t cap);
// Подписка на все команды устройства: opta/<id>/relay/+/+/set
void commandFilter(const char* deviceId, char* out, size_t cap);
// Состояние канала: leaf — "state", "mode" или "reason"
void channelTopic(const char* deviceId, uint8_t channel, const char* leaf, char* out, size_t cap);

// Ручной режим в Home Assistant — выбор из трех вариантов (D-042 п. 4)
enum class Mode : uint8_t { Auto, On, Off };
const char* modeOption(Mode mode);
// Текст варианта, как его присылает Home Assistant. false — неизвестный.
bool parseModeOption(const char* text, size_t length, Mode& out);
// manual — ручной режим канала, output — выход включен
Mode channelMode(bool manual, bool output);

// Настройки сценариев канала (D-044): ключ — последний уровень топика команды
// (opta/<id>/relay/<N>/<ключ>/set) и поле JSON состояния opta/<id>/relay/<N>/settings.
enum class Setting : uint8_t {
    SafetyCeiling, Threshold,
    SmartEnabled, SmartHours,
    WakeupEnabled, WakeupHours, WakeupTime,
    NightEnabled, NightStart, NightEnd, NightHours,
    ForceHours,
    HolidayEnabled, HolidayReturn, HolidayHours
};
constexpr uint8_t kSettingCount = 15;
const char* settingKey(Setting setting);
// Значение из Home Assistant в поле config: число ("0.4", "3", "3.0"), ON/OFF,
// время ЧЧ:ММ или дата и время возвращения ГГГГ-ММ-ДДTЧЧ:ММ (или через пробел) по Риге.
// false — не разобрано или вне пределов поля (channelconfig); связи между полями
// (channelconfig::isValid, дата в будущем) проверяет вызывающий.
bool applySetting(channelconfig::ChannelConfig& config, Setting setting, const char* text,
    size_t length);
// JSON состояния настроек канала: {"safety":0.40000,"smart":"ON","wake_t":"06:30",...}.
// false — не хватило места.
bool formatSettingsJson(const channelconfig::ChannelConfig& config, char* out, size_t cap);
constexpr size_t kSettingsJsonSize = 384;

enum class CommandType : uint8_t {
    SetMode, ForceOn, ForceStop, SetSetting,
    FirmwareCheck, FirmwareInstall  // opta/<id>/firmware/<check|install>/set (D-046)
};
constexpr size_t kCommandValueSize = 40;
struct Command {
    CommandType type;
    uint8_t channel;  // индекс 0…kChannelCount-1
    Mode mode;        // для SetMode
    Setting setting;  // для SetSetting
    char value[kCommandValueSize];  // для SetSetting, с нулем в конце
    uint8_t valueLength;
};
// Команда из топика opta/<deviceId>/relay/<N>/<действие>/set: mode, force, stop или
// ключ настройки. false — не наш топик, неверный канал, неизвестный вариант режима или
// значение настройки длиннее kCommandValueSize - 1. Кнопки принимают любой текст.
bool parseCommand(const char* deviceId, const char* topic, const char* payload,
    size_t payloadLength, Command& out);

// Подписка на команды прошивки: opta/<id>/firmware/+/set
void firmwareCommandFilter(const char* deviceId, char* out, size_t cap);

// Число описаний: цена; на канал 5 управляющих (выход, режим, причина, принудительно,
// стоп) и kSettingCount настроек; в конце — прошивка (update) и кнопка проверки (D-046).
constexpr uint8_t kEntitiesPerChannel = 5 + kSettingCount;
constexpr uint8_t kDiscoveryCount = 1 + kEntitiesPerChannel * kChannelCount + 2;

// Состояние сущности update (D-046), топик opta/<id>/firmware:
// {"installed_version":..,"latest_version":..,"in_progress":..,"update_percentage":..,
//  "release_summary":..,"release_url":..,"title":"Opta"}. latestVersion пустой — как
// установленная; percent < 0 — null. false — не поместилось.
constexpr size_t kFirmwareJsonSize = 640;
void firmwareTopic(const char* deviceId, char* out, size_t cap);
bool formatFirmwareJson(const char* installedVersion, const char* latestVersion,
    bool inProgress, int percent, const char* summary, char* out, size_t cap);
// Описание номер index (0…kDiscoveryCount-1): топик и JSON. false — индекс вне
// диапазона или не хватило места.
bool discoveryMessage(uint8_t index, const char* deviceId, const char* firmwareVersion,
    char* topic, size_t topicCap, char* payload, size_t payloadCap);

// Цена EUR/kWh с 5 знаками ("-0.00123"); без printf с плавающей точкой.
void formatPrice(float eurKwh, char* out, size_t cap);

// График (D-045): атрибуты сущностей для карточки apexcharts-card.
// Прогноз — атрибут "d" сущности «Цена» (топик opta/<id>/forecast):
// {"d":[[epoch UTC начала интервала, EUR/kWh], ...]}.
// План канала — атрибут "on" сущности «Реле N: выход» (opta/<id>/relay/N/plan):
// {"on":[[начало, конец], ...]} — подряд идущие интервалы плана слиты в отрезки.
constexpr size_t kForecastJsonSize = 7 * 1024;
constexpr size_t kPlanJsonSize = 4 * 1024;
void forecastTopic(const char* deviceId, char* out, size_t cap);
// false — не поместилось; тогда out — пустая строка.
bool formatForecastJson(const prices::PriceSeries& forecast, char* out, size_t cap);
bool formatPlanJson(const prices::PriceSeries& forecast, const planner::ChannelPlan& plan,
    char* out, size_t cap);

}  // namespace hamqtt
