#include "WifiCredentialStore.h"

#include <string.h>

namespace configstore {

bool WifiCredentialStore::load(WifiCredentials& credentials) {
    size_t storedSize = 0;
    int result = kv_.get(kWifiCredentialsKey, &credentials, sizeof(credentials), storedSize);
    if (result != 0 || storedSize != sizeof(credentials) ||
        credentials.ssid[0] == '\0' ||
        memchr(credentials.ssid, '\0', sizeof(credentials.ssid)) == nullptr ||
        memchr(credentials.password, '\0', sizeof(credentials.password)) == nullptr) {
        return false;
    }
    return true;
}

bool WifiCredentialStore::save(const WifiCredentials& credentials) {
    return kv_.set(kWifiCredentialsKey, &credentials, sizeof(credentials)) == 0;
}

}  // namespace configstore
