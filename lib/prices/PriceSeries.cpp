#include "PriceSeries.h"

namespace prices {

bool PriceSeries::append(uint32_t start, float eurKwh) {
    if (count_ >= kMaxSlots) return false;
    slots_[count_++] = {start, eurKwh};
    return true;
}

size_t PriceSeries::buildHours(PriceHour* hours, size_t capacity) const {
    size_t count = 0;
    for (size_t i = 0; i < count_; ++i) {
        uint32_t hourTimestamp = slots_[i].start / 3600UL * 3600UL;
        if (count == 0 || hours[count - 1].start != hourTimestamp) {
            if (count >= capacity) break;
            hours[count++] = {hourTimestamp, slots_[i].eurKwh, 1};
        } else {
            PriceHour& hour = hours[count - 1];
            hour.avgEurKwh += slots_[i].eurKwh;
            ++hour.quarters;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        hours[i].avgEurKwh /= hours[i].quarters;
    }
    return count;
}

}  // namespace prices
