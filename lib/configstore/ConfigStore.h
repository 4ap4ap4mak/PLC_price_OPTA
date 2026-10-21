// Хранение настроек каналов во flash: ключ /kv/device_config, формат — блоб
// channelconfig (сейчас v5). Чистый модуль L0: KVStore приходит через KvBackend,
// модуль не логирует — load() возвращает исход, а сообщения печатает вызывающий слой.
//
// ВНИМАНИЕ: формат блоба меняется только с повышением версии и миграцией в
// channelconfig::decode() (CLAUDE.md, правило 5).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ChannelConfig.h>
#include "KvBackend.h"

namespace configstore {

constexpr const char* kDeviceConfigKey = "/kv/device_config";

// Чем закончилась загрузка. После исходов *Failed конфигурации в out не гарантированы:
// вызывающий ставит значения по умолчанию в RAM.
enum class LoadOutcome : uint8_t {
    Loaded,              // блоб текущего формата, все каналы корректны; flash не менялся
    Migrated,            // старый формат или сброшенные каналы; блоб перезаписан
    MigratedSaveFailed,  // то же, но перезаписать не удалось
    Created,             // ключа не было; записаны значения по умолчанию
    CreateFailed,        // ключа не было, записать значения по умолчанию не удалось
    UnknownSize,         // размер не распознан; записаны значения по умолчанию
    UnknownSizeSaveFailed,
    LegacyMultiReadFailed,  // блоб v4 есть, но не читается
    CurrentReadFailed,      // блоб v5 есть, но не читается
    Corrupt,             // блоб v2/v3 с неизвестной версией или нечитаемый; записаны значения по умолчанию
    CorruptSaveFailed
};

struct LoadReport {
    LoadOutcome outcome;
    int kvResult;         // код kv_get_info при Created/CreateFailed
    uint8_t invalidMask;  // бит i — канал i был некорректен и сброшен (Migrated*)
};

// true — в out рабочие конфигурации (из flash или значения по умолчанию).
bool loadSucceeded(LoadOutcome outcome);

class ConfigStore {
public:
    explicit ConfigStore(KvBackend& kv) : kv_(kv) {}

    LoadReport load(channelconfig::ChannelConfig out[channelconfig::kChannelCount]);
    // Записывает блоб всех каналов целиком (kBlobSize байт).
    bool save(const channelconfig::ChannelConfig configs[channelconfig::kChannelCount]);

private:
    KvBackend& kv_;
};

}  // namespace configstore
