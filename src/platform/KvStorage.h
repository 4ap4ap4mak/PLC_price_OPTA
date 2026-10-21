// KVStore mbed (kv_get_info/kv_get/kv_set) для lib/configstore.
#pragma once

#include <KvBackend.h>

class KvStorage : public configstore::KvBackend {
public:
    int getInfo(const char* key, size_t& size) override;
    int get(const char* key, void* buffer, size_t bufferSize, size_t& actualSize) override;
    int set(const char* key, const void* data, size_t size) override;
};
