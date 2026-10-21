// Фейковый KVStore в памяти для тестов lib/configstore и lib/controller.
// Одно значение на ключ, коды ошибок — как у kv_* (отрицательные), ошибки управляемые.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <KvBackend.h>

namespace testing {

class FakeKv : public configstore::KvBackend {
public:
    static constexpr int kNotFound = -276;
    static constexpr int kReadError = -301;
    static constexpr int kWriteError = -302;

    char key[32] = {};
    uint8_t data[512] = {};
    size_t size = 0;
    bool present = false;
    bool failGet = false;
    bool shortGet = false;  // прочитать на байт меньше, чем хранится
    bool failSet = false;
    int setCalls = 0;

    void put(const char* k, const void* value, size_t length) {
        strncpy(key, k, sizeof(key) - 1);
        memcpy(data, value, length);
        size = length;
        present = true;
    }

    int getInfo(const char* k, size_t& outSize) override {
        if (!present || strcmp(k, key) != 0) return kNotFound;
        outSize = size;
        return 0;
    }

    int get(const char* k, void* buffer, size_t bufferSize, size_t& actualSize) override {
        if (!present || strcmp(k, key) != 0) return kNotFound;
        if (failGet) return kReadError;
        size_t n = size < bufferSize ? size : bufferSize;
        if (shortGet && n > 0) --n;
        memcpy(buffer, data, n);
        actualSize = n;
        return 0;
    }

    int set(const char* k, const void* value, size_t length) override {
        ++setCalls;
        if (failSet) return kWriteError;
        put(k, value, length);
        return 0;
    }
};

}  // namespace testing
