#include "PriceSeries.h"

#include <string.h>

namespace prices {

bool PriceSeries::append(uint32_t start, float eurKwh) {
    if (count_ >= kMaxSlots) return false;
    if (count_ > 0 && start <= slots_[count_ - 1].start) return false;
    slots_[count_++] = {start, eurKwh};
    return true;
}

size_t PriceSeries::lowerBound(uint32_t epoch) const {
    size_t low = 0;
    size_t high = count_;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (slots_[middle].start < epoch) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

int PriceSeries::indexAt(uint32_t epoch) const {
    size_t next = lowerBound(epoch);
    if (next < count_ && slots_[next].start == epoch) return static_cast<int>(next);
    if (next == 0) return -1;
    const PriceSlot& previous = slots_[next - 1];
    return epoch - previous.start < kSlotSeconds ? static_cast<int>(next - 1) : -1;
}

uint32_t PriceSeries::coverageEnd() const {
    return count_ == 0 ? 0 : slots_[count_ - 1].start + kSlotSeconds;
}

void PriceSeries::replaceKeepingOlder(uint32_t keepFrom, const PriceSeries& newer) {
    size_t first = lowerBound(keepFrom);
    size_t last = newer.count_ == 0 ? count_ : lowerBound(newer.slots_[0].start);
    if (last < first) last = first;
    size_t kept = last - first;
    if (kept > 0 && first > 0) {
        memmove(slots_, slots_ + first, kept * sizeof(PriceSlot));
    }
    count_ = kept;
    for (size_t i = 0; i < newer.count_ && count_ < kMaxSlots; ++i) {
        slots_[count_++] = newer.slots_[i];
    }
}

void PriceSeries::prependOlder(const PriceSeries& older) {
    const size_t available = count_ == 0 ? older.count_ : older.lowerBound(slots_[0].start);
    // Не хватает места — берутся самые поздние, примыкающие к своему ряду без разрыва
    const size_t added = available < kMaxSlots - count_ ? available : kMaxSlots - count_;
    if (added == 0) return;
    memmove(slots_ + added, slots_, count_ * sizeof(PriceSlot));
    memcpy(slots_, older.slots_ + (available - added), added * sizeof(PriceSlot));
    count_ += added;
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
