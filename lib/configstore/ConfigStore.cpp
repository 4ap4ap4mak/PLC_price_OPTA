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
        case LoadOutcome::Created:
        case LoadOutcome::UnknownSize:
        case LoadOutcome::Corrupt:
            return true;
        default:
            return false;
    }
}

bool ConfigStore::save(const ChannelConfig configs[kChannelCount]) {
    return kv_.set(kDeviceConfigKey, configs, sizeof(ChannelConfig) * kChannelCount) == 0;
}

LoadReport ConfigStore::load(ChannelConfig out[kChannelCount]) {
    LoadReport report = {LoadOutcome::Loaded, 0, 0};

    size_t storedSize = 0;
    int result = kv_.getInfo(kDeviceConfigKey, storedSize);
    if (result != 0) {
        report.kvResult = result;
        setDefaultConfigs(out);
        report.outcome = save(out) ? LoadOutcome::Created : LoadOutcome::CreateFailed;
        return report;
    }

    BlobKind kind = channelconfig::blobKind(storedSize);
    if (kind == BlobKind::Unknown) {
        setDefaultConfigs(out);
        report.outcome = save(out) ? LoadOutcome::UnknownSize : LoadOutcome::UnknownSizeSaveFailed;
        return report;
    }

    uint8_t blob[channelconfig::kBlobSize] = {};
    size_t actualSize = 0;
    result = kv_.get(kDeviceConfigKey, blob, storedSize, actualSize);
    // Нечитаемый блоб v2/v3 считается поврежденным, как и блоб с неизвестной версией
    DecodeResult decoded = DecodeResult::LegacyCorrupt;
    if (result == 0 && actualSize == storedSize) {
        decoded = channelconfig::decode(blob, storedSize, out, report.invalidMask);
    } else if (kind == BlobKind::LegacyMulti) {
        report.outcome = LoadOutcome::LegacyMultiReadFailed;
        return report;
    } else if (kind == BlobKind::Current) {
        report.outcome = LoadOutcome::CurrentReadFailed;
        return report;
    }
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

}  // namespace configstore
