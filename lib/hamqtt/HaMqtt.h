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

enum class CommandType : uint8_t { SetMode, ForceOn, ForceStop };
struct Command {
    CommandType type;
    uint8_t channel;  // индекс 0…kChannelCount-1
    Mode mode;        // для SetMode
};
// Команда из топика opta/<deviceId>/relay/<N>/<mode|force|stop>/set. false — не наш
// топик, неверный канал или неизвестный вариант режима. Кнопки принимают любой текст.
bool parseCommand(const char* deviceId, const char* topic, const char* payload,
    size_t payloadLength, Command& out);

// Число описаний: цена + по 5 на канал (выход, режим, причина, принудительно, стоп).
constexpr uint8_t kDiscoveryCount = 1 + 5 * kChannelCount;
// Описание номер index (0…kDiscoveryCount-1): топик и JSON. false — индекс вне
// диапазона или не хватило места.
bool discoveryMessage(uint8_t index, const char* deviceId, const char* firmwareVersion,
    char* topic, size_t topicCap, char* payload, size_t payloadCap);

// Цена EUR/kWh с 5 знаками ("-0.00123"); без printf с плавающей точкой.
void formatPrice(float eurKwh, char* out, size_t cap);

}  // namespace hamqtt
