#include "ConfigStore.h"

namespace configstore {

using channelconfig::BlobKind;
using channelconfig::ChannelConfig;
using channelconfig::DecodeResult;
using channelconfig::kChannelCount;

namespace {

void setDefaultConfigs(ChannelConfig* configs) {
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        channelconfig::setDefaults(configs[i]);
    }
}

}  // namespace

bool loadSucceeded(LoadOutcome outcome) {
    switch (outcome) {
        case LoadOutcome::Loaded:
        case LoadOutcome::Migrated:
        case LoadOutcome::MigratedSaveFailed:
        case LoadOutcome::Created:
        case LoadOutcome::UnknownSize:
        case LoadOutcome::Corrupt:
            return true;
        default:
            return false;
    }
}

bool ConfigStore::save(const ChannelConfig configs[kChannelCount]) {
    if (writeLocked_) return false;
    return kv_.set(kDeviceConfigKey, configs, sizeof(ChannelConfig) * kChannelCount) == 0;
}

bool ConfigStore::overwrite(const ChannelConfig configs[kChannelCount]) {
    writeLocked_ = false;
    return save(configs);
}

LoadReport ConfigStore::load(ChannelConfig out[kChannelCount]) {
    LoadReport report = {LoadOutcome::Loaded, 0, 0};

    // Ошибка чтения flash может быть разовой: повтор, прежде чем решать (D-037)
    size_t storedSize = 0;
    int result = -1;
    for (uint8_t attempt = 0; attempt < kReadAttempts && result != 0; ++attempt) {
        result = kv_.getInfo(kDeviceConfigKey, storedSize);
        if (kv_.isNotFound(result)) break;
    }
    if (result != 0) {
        report.kvResult = result;
        setDefaultConfigs(out);
        if (kv_.isNotFound(result)) {
            report.outcome = save(out) ? LoadOutcome::Created : LoadOutcome::CreateFailed;
        } else {
            lockAfterReadFailure(report);
        }
        return report;
    }

    BlobKind kind = channelconfig::blobKind(storedSize);
    if (kind == BlobKind::Unknown) {
        setDefaultConfigs(out);
        report.outcome = save(out) ? LoadOutcome::UnknownSize : LoadOutcome::UnknownSizeSaveFailed;
        return report;
    }

    uint8_t blob[channelconfig::kBlobSize] = {};
    bool read = false;
    for (uint8_t attempt = 0; attempt < kReadAttempts && !read; ++attempt) {
        size_t actualSize = 0;
        result = kv_.get(kDeviceConfigKey, blob, storedSize, actualSize);
        read = result == 0 && actualSize == storedSize;
    }
    if (!read) {
        report.kvResult = result;
        setDefaultConfigs(out);
        lockAfterReadFailure(report);
        return report;
    }

    // Блоб v2/v3 с неизвестной версией прочитан, но не распознан: он поврежден
    const DecodeResult decoded = channelconfig::decode(blob, storedSize, out, report.invalidMask);
    if (decoded == DecodeResult::LegacyCorrupt) {
        setDefaultConfigs(out);
        report.outcome = save(out) ? LoadOutcome::Corrupt : LoadOutcome::CorruptSaveFailed;
        return report;
    }

    if (decoded == DecodeResult::Migrated) {
        report.outcome = save(out) ? LoadOutcome::Migrated : LoadOutcome::MigratedSaveFailed;
    }
    return report;
}

// Сохраненные настройки есть, но не читаются: не затирать их значениями по умолчанию
// до перезапуска или явной команды overwrite().
void ConfigStore::lockAfterReadFailure(LoadReport& report) {
    writeLocked_ = true;
    report.outcome = LoadOutcome::ReadFailed;
}

}  // namespace configstore
