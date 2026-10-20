// Настройки канала (реле) и формат их блоба во flash (/kv/device_config).
// Чистый модуль L0: без Arduino и KVStore, не логирует. Чтение/запись KVStore
// остаются у вызывающего слоя, здесь — значения по умолчанию, проверка и
// распознавание старых форматов блоба.
//
// ВНИМАНИЕ: раскладка ChannelConfig — это формат во flash (версия 5). Любое
// изменение полей требует новой версии и ветки миграции в decode(), см. CLAUDE.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace channelconfig {

constexpr uint8_t kChannelCount = 4;
constexpr uint16_t kConfigVersion = 5;

struct ChannelConfig {
    uint16_t version;
    float safetyCeilingEurKwh;
    float alwaysOnBelowPriceEurKwh;
    bool nightPrepEnabled;
    uint16_t nightStartMinuteOfDay;
    uint16_t nightEndMinuteOfDay;
    uint8_t nightCheapHours;
    bool unusedBlockEnabled;      // бывший флажок «непрерывного блока», не используется (D-006)
    uint8_t forceOnHours;          // длительность принудительного включения, ч (бывший consecutiveBlockHours)
    bool holidayEnabled;
    uint32_t holidayReturnEpoch;
    uint8_t holidayOnHours;
    bool smartThresholdEnabled;
    uint8_t smartThresholdHours;
    bool wakeupEnabled;
    uint8_t wakeupHours;
    uint16_t wakeupTimeMinuteOfDay;
};
static_assert(sizeof(ChannelConfig) == 36, "формат блоба во flash изменился — нужна миграция");

// Формат v2/v3 (одно реле) и v4 (четыре реле этого же формата).
struct LegacyChannelConfig {
    uint16_t version;
    float safetyCeilingEurKwh;
    float alwaysOnBelowPriceEurKwh;
    bool nightPrepEnabled;
    uint16_t nightStartMinuteOfDay;
    uint16_t nightEndMinuteOfDay;
    uint8_t nightCheapHours;
    bool consecutiveBlockEnabled;
    uint8_t consecutiveBlockHours;
    bool holidayEnabled;
    uint32_t holidayReturnEpoch;
    uint8_t holidayOnHours;
};
static_assert(sizeof(LegacyChannelConfig) == 32, "размер старого блоба v2/v3 изменился");

constexpr size_t kLegacySingleBlobSize = sizeof(LegacyChannelConfig);
constexpr size_t kLegacyMultiBlobSize = sizeof(LegacyChannelConfig) * kChannelCount;
constexpr size_t kBlobSize = sizeof(ChannelConfig) * kChannelCount;

void setDefaults(ChannelConfig& config);
// Перенос полей старого формата; поля, которых в нем нет, не трогаются.
void importLegacy(const LegacyChannelConfig& legacy, ChannelConfig& config);
bool isValid(const ChannelConfig& config);

// Какой формат лежит во flash, определяется только по размеру блоба.
enum class BlobKind : uint8_t { LegacySingle, LegacyMulti, Current, Unknown };
BlobKind blobKind(size_t size);

enum class DecodeResult : uint8_t {
    Ok,             // блоб текущего формата, все каналы корректны
    Migrated,       // старый формат перенесен или часть каналов сброшена — блоб нужно перезаписать
    LegacyCorrupt,  // старый блоб v2/v3 с неизвестной версией; out не изменен
    UnknownSize     // размер не распознан; out не изменен
};

// Блоб, прочитанный из KVStore, → конфигурации всех каналов.
// invalidMask: бит i — канал i не прошел проверку и сброшен на значения по умолчанию.
DecodeResult decode(const uint8_t* blob, size_t size,
    ChannelConfig out[kChannelCount], uint8_t& invalidMask);

}  // namespace channelconfig
