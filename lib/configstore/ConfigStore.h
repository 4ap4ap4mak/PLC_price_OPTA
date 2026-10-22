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

// Чем закончилась загрузка. В out всегда рабочие конфигурации: из flash или значения
// по умолчанию.
enum class LoadOutcome : uint8_t {
    Loaded,              // блоб текущего формата, все каналы корректны; flash не менялся
    Migrated,            // старый формат или сброшенные каналы; блоб перезаписан
    MigratedSaveFailed,  // то же, но перезаписать не удалось; в out перенесенные настройки
    Created,             // ключа нет; записаны значения по умолчанию
    CreateFailed,        // ключа нет, записать значения по умолчанию не удалось
    UnknownSize,         // размер не распознан; записаны значения по умолчанию
    UnknownSizeSaveFailed,
    // Настройки есть, но не читаются и после kReadAttempts попыток (D-037): в out
    // значения по умолчанию, запись во flash заблокирована до перезапуска или
    // overwrite(), чтобы не затереть сохраненное.
    ReadFailed,
    Corrupt,             // блоб v2/v3 с неизвестной версией; записаны значения по умолчанию
    CorruptSaveFailed
};

struct LoadReport {
    LoadOutcome outcome;
    int kvResult;         // код KVStore при Created/CreateFailed/ReadFailed
    uint8_t invalidMask;  // бит i — канал i был некорректен и сброшен (Migrated*)
};

// true — в out настройки из flash или записанные значения по умолчанию; false — flash
// недоступен, в out значения по умолчанию только в RAM.
bool loadSucceeded(LoadOutcome outcome);

class ConfigStore {
public:
    // Попыток чтения при ошибке KVStore, прежде чем считать настройки нечитаемыми.
    static constexpr uint8_t kReadAttempts = 3;

    explicit ConfigStore(KvBackend& kv) : kv_(kv) {}

    LoadReport load(channelconfig::ChannelConfig out[channelconfig::kChannelCount]);
    // Записывает блоб всех каналов целиком (kBlobSize байт). false — запись не удалась
    // или заблокирована (writeLocked()).
    bool save(const channelconfig::ChannelConfig configs[channelconfig::kChannelCount]);
    // После ReadFailed: сохраненные настройки не прочитались, запись заблокирована.
    bool writeLocked() const { return writeLocked_; }
    // Явная команда пользователя: снять блокировку и записать configs поверх
    // нечитаемых настроек.
    bool overwrite(const channelconfig::ChannelConfig configs[channelconfig::kChannelCount]);

private:
    void lockAfterReadFailure(LoadReport& report);

    KvBackend& kv_;
    bool writeLocked_ = false;
};

}  // namespace configstore
