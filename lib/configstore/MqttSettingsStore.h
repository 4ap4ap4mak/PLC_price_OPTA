// Брокер MQTT для Home Assistant во flash (D-042): ключ /kv/mqtt, структура
// MqttSettings как есть. Отдельный ключ: формат настроек каналов (/kv/device_config)
// не меняется. Чистый модуль L0: KVStore приходит через KvBackend, модуль не логирует.
//
// ВНИМАНИЕ: раскладка MqttSettings — формат во flash. Изменение размера сделает
// сохраненные данные нечитаемыми (load() проверяет точный размер).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "KvBackend.h"

namespace configstore {

constexpr const char* kMqttSettingsKey = "/kv/mqtt";
constexpr uint16_t kMqttDefaultPort = 1883;

struct MqttSettings {
    char host[64];      // IP или имя брокера; пусто — MQTT выключен
    uint16_t port;
    char user[33];
    char password[65];
};
static_assert(sizeof(MqttSettings) == 164, "формат /kv/mqtt во flash изменился");

// Адрес брокера: 1…63 символа из букв, цифр, '.' и '-' (IPv4 или имя DNS).
bool validMqttHost(const char* host, size_t length);
// Пользователь: 1…32 печатных символа ASCII без пробела.
bool validMqttUser(const char* user, size_t length);
// Пароль: 0…64 байта без управляющих символов.
bool validMqttPassword(const char* password, size_t length);

class MqttSettingsStore {
public:
    explicit MqttSettingsStore(KvBackend& kv) : kv_(kv) {}

    // false — ключа нет, ошибка чтения, размер не тот или строки без завершающего
    // нуля; тогда settings — выключенный MQTT (пустой адрес, порт по умолчанию).
    bool load(MqttSettings& settings);
    bool save(const MqttSettings& settings);

private:
    KvBackend& kv_;
};

}  // namespace configstore
