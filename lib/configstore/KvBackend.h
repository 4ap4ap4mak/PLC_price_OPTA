// Доступ к хранилищу ключ-значение во flash (KVStore mbed). На устройстве его
// реализует src/platform/KvStorage (kv_get_info/kv_get/kv_set), в тестах — фейк.
// Коды возврата как у kv_*: 0 — успех, иначе код ошибки KVStore.
#pragma once

#include <stddef.h>

namespace configstore {

class KvBackend {
public:
    virtual ~KvBackend() = default;
    // Размер значения по ключу.
    virtual int getInfo(const char* key, size_t& size) = 0;
    // Прочитать не больше bufferSize байт; actualSize — сколько прочитано.
    virtual int get(const char* key, void* buffer, size_t bufferSize, size_t& actualSize) = 0;
    virtual int set(const char* key, const void* data, size_t size) = 0;
};

}  // namespace configstore
