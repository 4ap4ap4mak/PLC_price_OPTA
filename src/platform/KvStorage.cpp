#include "KvStorage.h"

#include <kvstore_global_api.h>
#include <mbed_error.h>

int KvStorage::getInfo(const char* key, size_t& size) {
    kv_info_t info = {};
    int result = kv_get_info(key, &info);
    size = info.size;
    return result;
}

int KvStorage::get(const char* key, void* buffer, size_t bufferSize, size_t& actualSize) {
    return kv_get(key, buffer, bufferSize, &actualSize);
}

int KvStorage::set(const char* key, const void* data, size_t size) {
    return kv_set(key, data, size, 0);
}

bool KvStorage::isNotFound(int result) const {
    return result != 0 && MBED_GET_ERROR_CODE(result) == MBED_ERROR_CODE_ITEM_NOT_FOUND;
}
