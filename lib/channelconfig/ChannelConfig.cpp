#include "ChannelConfig.h"

#include <math.h>
#include <string.h>

namespace channelconfig {

void setDefaults(ChannelConfig& config) {
    memset(&config, 0, sizeof(config));
    config.version = kConfigVersion;
    config.safetyCeilingEurKwh = 0.3f;
    config.alwaysOnBelowPriceEurKwh = 0.0f;
    config.nightStartMinuteOfDay = 23 * 60;
    config.nightEndMinuteOfDay = 7 * 60;
    config.nightCheapHours = 4;
    config.consecutiveBlockHours = 3;
    config.holidayOnHours = 3;
    config.smartThresholdHours = 4;
    config.wakeupHours = 3;
    config.wakeupTimeMinuteOfDay = 7 * 60;
}

void importLegacy(const LegacyChannelConfig& legacy, ChannelConfig& config) {
    config.version = legacy.version;
    config.safetyCeilingEurKwh = legacy.safetyCeilingEurKwh;
    config.alwaysOnBelowPriceEurKwh = legacy.alwaysOnBelowPriceEurKwh;
    config.nightPrepEnabled = legacy.nightPrepEnabled;
    config.nightStartMinuteOfDay = legacy.nightStartMinuteOfDay;
    config.nightEndMinuteOfDay = legacy.nightEndMinuteOfDay;
    config.nightCheapHours = legacy.nightCheapHours;
    config.consecutiveBlockEnabled = legacy.consecutiveBlockEnabled;
    config.consecutiveBlockHours = legacy.consecutiveBlockHours;
    config.holidayEnabled = legacy.holidayEnabled;
    config.holidayReturnEpoch = legacy.holidayReturnEpoch;
    config.holidayOnHours = legacy.holidayOnHours;
}

bool isValid(const ChannelConfig& config) {
    uint16_t nightWindowMinutes = config.nightStartMinuteOfDay < config.nightEndMinuteOfDay
        ? config.nightEndMinuteOfDay - config.nightStartMinuteOfDay
        : 1440 - config.nightStartMinuteOfDay + config.nightEndMinuteOfDay;
    return config.version == kConfigVersion &&
        isfinite(config.safetyCeilingEurKwh) &&
        isfinite(config.alwaysOnBelowPriceEurKwh) &&
        config.safetyCeilingEurKwh > config.alwaysOnBelowPriceEurKwh &&
        config.nightStartMinuteOfDay < 1440 && config.nightEndMinuteOfDay < 1440 &&
        config.nightStartMinuteOfDay != config.nightEndMinuteOfDay &&
        config.nightCheapHours > 0 && config.nightCheapHours <= 24 &&
        (!config.nightPrepEnabled || config.nightCheapHours <= nightWindowMinutes / 60) &&
        config.consecutiveBlockHours > 0 && config.consecutiveBlockHours <= 24 &&
        config.holidayOnHours > 0 && config.holidayOnHours <= 24 &&
        (!config.holidayEnabled || config.holidayReturnEpoch != 0) &&
        config.smartThresholdHours > 0 && config.smartThresholdHours <= 24 &&
        config.wakeupHours > 0 && config.wakeupHours <= 24 &&
        config.wakeupTimeMinuteOfDay < 1440;
}

BlobKind blobKind(size_t size) {
    if (size == kLegacySingleBlobSize) return BlobKind::LegacySingle;
    if (size == kLegacyMultiBlobSize) return BlobKind::LegacyMulti;
    if (size == kBlobSize) return BlobKind::Current;
    return BlobKind::Unknown;
}

DecodeResult decode(const uint8_t* blob, size_t size,
    ChannelConfig out[kChannelCount], uint8_t& invalidMask) {
    invalidMask = 0;
    bool migrated = false;
    switch (blobKind(size)) {
        case BlobKind::LegacySingle: {
            LegacyChannelConfig legacy;
            memcpy(&legacy, blob, sizeof(legacy));
            if (legacy.version != 2 && legacy.version != 3) {
                return DecodeResult::LegacyCorrupt;
            }
            for (uint8_t i = 0; i < kChannelCount; ++i) {
                setDefaults(out[i]);
            }
            importLegacy(legacy, out[0]);
            // v2 хранил цены в EUR/MWh
            if (legacy.version == 2) {
                out[0].safetyCeilingEurKwh /= 1000.0f;
                out[0].alwaysOnBelowPriceEurKwh /= 1000.0f;
            }
            out[0].version = kConfigVersion;
            migrated = true;
            break;
        }
        case BlobKind::LegacyMulti: {
            LegacyChannelConfig legacy[kChannelCount];
            memcpy(legacy, blob, sizeof(legacy));
            for (uint8_t i = 0; i < kChannelCount; ++i) {
                setDefaults(out[i]);
                importLegacy(legacy[i], out[i]);
                out[i].version = kConfigVersion;
            }
            migrated = true;
            break;
        }
        case BlobKind::Current:
            memcpy(out, blob, kBlobSize);
            break;
        case BlobKind::Unknown:
            return DecodeResult::UnknownSize;
    }

    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (!isValid(out[i])) {
            setDefaults(out[i]);
            invalidMask |= static_cast<uint8_t>(1U << i);
            migrated = true;
        }
    }
    return migrated ? DecodeResult::Migrated : DecodeResult::Ok;
}

}  // namespace channelconfig
