#include "MqttSettingsStore.h"

#include <string.h>

namespace configstore {

bool validMqttHost(const char* host, size_t length) {
    if (length == 0 || length >= sizeof(MqttSettings::host)) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = host[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool validMqttUser(const char* user, size_t length) {
    if (length == 0 || length >= sizeof(MqttSettings::user)) return false;
    for (size_t i = 0; i < length; ++i) {
        if (user[i] <= ' ' || user[i] > '~') return false;
    }
    return true;
}

bool validMqttPassword(const char* password, size_t length) {
    if (length >= sizeof(MqttSettings::password)) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(password[i]);
        if (c < ' ' || c == 0x7F) return false;
    }
    return true;
}

bool MqttSettingsStore::load(MqttSettings& settings) {
    size_t storedSize = 0;
    const int result = kv_.get(kMqttSettingsKey, &settings, sizeof(settings), storedSize);
    if (result == 0 && storedSize == sizeof(settings) &&
        memchr(settings.host, '\0', sizeof(settings.host)) != nullptr &&
        memchr(settings.user, '\0', sizeof(settings.user)) != nullptr &&
        memchr(settings.password, '\0', sizeof(settings.password)) != nullptr) {
        return true;
    }
    memset(&settings, 0, sizeof(settings));
    settings.port = kMqttDefaultPort;
    return false;
}

bool MqttSettingsStore::save(const MqttSettings& settings) {
    return kv_.set(kMqttSettingsKey, &settings, sizeof(settings)) == 0;
}

}  // namespace configstore
