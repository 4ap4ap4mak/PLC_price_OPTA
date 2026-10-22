#include "Elering.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>
#include <RigaTime.h>

namespace elering {

bool jsonObjectBounds(const char* body, size_t len, size_t& start, size_t& end) {
    // Поиск '{' останавливается на нулевом байте, как String::indexOf (strchr) в монолите.
    size_t first = len;
    for (size_t i = 0; i < len && body[i] != '\0'; ++i) {
        if (body[i] == '{') {
            first = i;
            break;
        }
    }
    if (first == len) return false;
    for (size_t i = len; i > first; --i) {
        if (body[i - 1] == '}') {
            start = first;
            end = i;
            return true;
        }
    }
    return false;
}

int parseHttpStatus(const char* statusLine) {
    if (strncmp(statusLine, "HTTP/", 5) != 0) return 0;
    const char* space = strchr(statusLine, ' ');
    if (space == nullptr) return 0;
    int code = 0;
    for (int i = 1; i <= 3; ++i) {
        const char c = space[i];
        if (c < '0' || c > '9') return 0;
        code = code * 10 + (c - '0');
    }
    const char after = space[4];
    return after == '\0' || after == ' ' ? code : 0;
}

bool priceIsCurrent(long timestamp, uint32_t now) {
    // Разность всегда от большего к меньшему: в uint32_t now - priceEpoch при цене
    // из будущего переполнялась, и допуск не действовал (KI-015)
    uint32_t priceEpoch = static_cast<uint32_t>(timestamp);
    if (priceEpoch > now) {
        return priceEpoch - now <= kMaxPriceLeadSeconds;
    }
    return now - priceEpoch < kMaxPriceAgeSeconds;
}

Error parseCurrent(const char* json, size_t len, uint32_t now,
    CurrentPrice& out, const char*& jsonError) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json, len);
    if (error) {
        jsonError = error.c_str();
        return Error::Json;
    }

    bool success = doc["success"] | false;
    if (!success || !doc["data"].is<JsonArray>() || doc["data"].size() == 0) {
        return Error::Format;
    }

    // Без явной проверки отсутствующее поле превращается в 0 и может включить реле
    JsonVariant priceValue = doc["data"][0]["price"];
    JsonVariant timestampValue = doc["data"][0]["timestamp"];
    if (!priceValue.is<float>() || !timestampValue.is<long>()) {
        return Error::NoPrice;
    }
    out.eurMwh = priceValue.as<float>();
    out.timestamp = timestampValue.as<long>();
    // Гармонизированные пределы рынка: от -500 до 4000 EUR/MWh, берем с запасом
    if (!isfinite(out.eurMwh) || out.eurMwh < -1000.0f || out.eurMwh > 10000.0f) {
        return Error::OutOfRange;
    }
    if (out.timestamp < static_cast<long>(kMinValidEpoch)) {
        return Error::BadTimestamp;
    }
    if (now != 0 && !priceIsCurrent(out.timestamp, now)) {
        return Error::Stale;
    }
    out.eurKwh = out.eurMwh / 1000.0f;
    return Error::Ok;
}

Error parseForecast(const char* json, size_t len, prices::PriceSeries& out,
    const char*& jsonError) {
    out.clear();
    JsonDocument filter;
    filter["success"] = true;
    filter["data"]["lv"][0]["timestamp"] = true;
    filter["data"]["lv"][0]["price"] = true;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json, len,
        DeserializationOption::Filter(filter));
    if (error) {
        jsonError = error.c_str();
        return Error::Json;
    }
    if (!(doc["success"] | false) || !doc["data"]["lv"].is<JsonArray>()) {
        return Error::Format;
    }

    JsonArray items = doc["data"]["lv"].as<JsonArray>();
    for (JsonObject item : items) {
        if (out.size() >= prices::kMaxSlots) break;
        uint32_t timestamp = item["timestamp"].as<uint32_t>();
        float price = item["price"].as<float>();
        if (timestamp == 0 || !isfinite(price)) continue;
        out.append(timestamp, price / 1000.0f);
    }
    return out.size() == 0 ? Error::Empty : Error::Ok;
}

static bool formatUtcIso(uint32_t epoch, char* out, size_t cap) {
    int year;
    unsigned month;
    unsigned day;
    rigatime::civilFromDays(epoch / 86400UL, year, month, day);
    uint32_t secondsInDay = epoch % 86400UL;
    int length = snprintf(out, cap, "%04d-%02u-%02uT%02u:%02u:%02u.000Z",
        year, month, day,
        static_cast<unsigned>(secondsInDay / 3600UL),
        static_cast<unsigned>((secondsInDay % 3600UL) / 60UL),
        static_cast<unsigned>(secondsInDay % 60UL));
    return length > 0 && static_cast<size_t>(length) < cap;
}

// Период из местных суток [firstDay, lastDay]: от 00:00 первых до 23:45 последних.
static bool rangePath(int64_t firstDay, int64_t lastDay, char* out, size_t cap) {
    uint32_t startEpoch = rigatime::localDayStartUtc(firstDay);
    uint32_t endEpoch = rigatime::localDayStartUtc(lastDay + 1) - prices::kSlotSeconds;

    char startDate[32];
    char endDate[32];
    if (!formatUtcIso(startEpoch, startDate, sizeof(startDate)) ||
        !formatUtcIso(endEpoch, endDate, sizeof(endDate))) {
        return false;
    }
    int length = snprintf(out, cap, "/api/nps/price?start=%s&end=%s", startDate, endDate);
    return length > 0 && static_cast<size_t>(length) < cap;
}

bool forecastPath(uint32_t now, char* out, size_t cap) {
    int64_t today = rigatime::localDay(now);
    return rangePath(today, today + 1, out, cap);
}

bool dayPricesPath(int64_t localDay, char* out, size_t cap) {
    return rangePath(localDay, localDay, out, cap);
}

}  // namespace elering
