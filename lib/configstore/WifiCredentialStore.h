// Учетные данные Wi-Fi во flash: ключ /kv/wifi, структура WifiCredentials как есть.
// Чистый модуль L0: KVStore приходит через KvBackend, модуль не логирует.
//
// ВНИМАНИЕ: раскладка WifiCredentials — формат во flash. Изменение размера сделает
// сохраненные данные нечитаемыми (load() проверяет точный размер).
#pragma once

#include <stdint.h>

#include "KvBackend.h"

namespace configstore {

constexpr const char* kWifiCredentialsKey = "/kv/wifi";

struct WifiCredentials {
    char ssid[33];
    char password[64];
};
static_assert(sizeof(WifiCredentials) == 97, "формат /kv/wifi во flash изменился");

class WifiCredentialStore {
public:
    explicit WifiCredentialStore(KvBackend& kv) : kv_(kv) {}

    // false — ключа нет, размер не тот, SSID пустой или строки без завершающего нуля.
    // При false содержимое credentials не гарантировано.
    bool load(WifiCredentials& credentials);
    bool save(const WifiCredentials& credentials);

private:
    KvBackend& kv_;
};

}  // namespace configstore
