// Общие помощники unit-тестов: время и ряды цен.
// Подключается из test/test_*/test_main.cpp как "../helpers/TestHelpers.h".
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ChannelConfig.h>
#include <PriceSeries.h>
#include <RigaTime.h>

// Сравнение enum class в Unity (TEST_ASSERT_EQUAL не умеет их приводить).
#define ASSERT_ENUM(expected, actual) \
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace testing {

// UTC-дата → epoch, без учета Риги.
inline uint32_t utc(int year, unsigned month, unsigned day, unsigned hour = 0, unsigned minute = 0) {
    return static_cast<uint32_t>(rigatime::daysFromCivil(year, month, day) * 86400 +
        hour * 3600 + minute * 60);
}

// Местное время Риги → epoch. Для несуществующего времени (переход на летнее) — 0.
inline uint32_t riga(int year, unsigned month, unsigned day, unsigned hour = 0, unsigned minute = 0) {
    char text[17];
    snprintf(text, sizeof(text), "%04d-%02u-%02uT%02u:%02u", year, month, day, hour, minute);
    uint32_t epoch = 0;
    return rigatime::parseLocalDateTime(text, strlen(text), epoch) ? epoch : 0;
}

// Ряд из hourCount полных часов (по 4 квартала) начиная с start; цена часа — prices[i].
inline void fillHours(prices::PriceSeries& series, uint32_t start, const float* hourPrices,
    size_t hourCount) {
    series.clear();
    for (size_t h = 0; h < hourCount; ++h) {
        for (uint32_t q = 0; q < 4; ++q) {
            series.append(start + static_cast<uint32_t>(h) * 3600UL + q * 900UL, hourPrices[h]);
        }
    }
}

// Канал со всеми сценариями выключенными и высоким аварийным пределом.
inline channelconfig::ChannelConfig quietConfig() {
    channelconfig::ChannelConfig config;
    channelconfig::setDefaults(config);
    config.safetyCeilingEurKwh = 10.0f;
    config.alwaysOnBelowPriceEurKwh = -10.0f;
    return config;
}

}  // namespace testing
