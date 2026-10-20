#include "ForceOnTimer.h"

#include <stdio.h>

namespace forceon {

void ForceOnTimer::start(uint32_t nowMs, uint8_t hours) {
    if (hours < kMinHours) hours = kMinHours;
    if (hours > kMaxHours) hours = kMaxHours;
    active_ = true;
    startedMs_ = nowMs;
    durationMs_ = static_cast<uint32_t>(hours) * 3600UL * 1000UL;
}

bool ForceOnTimer::expire(uint32_t nowMs) {
    if (!active_ || nowMs - startedMs_ < durationMs_) return false;
    active_ = false;
    return true;
}

uint32_t ForceOnTimer::remainingMs(uint32_t nowMs) const {
    if (!active_) return 0;
    const uint32_t elapsed = nowMs - startedMs_;
    return elapsed >= durationMs_ ? 0 : durationMs_ - elapsed;
}

void formatRemaining(uint32_t remainingMs, char* out, size_t cap) {
    const uint32_t minutes = (remainingMs + 59999UL) / 60000UL;
    if (minutes >= 60) {
        snprintf(out, cap, "%lu ч %02lu мин", static_cast<unsigned long>(minutes / 60),
            static_cast<unsigned long>(minutes % 60));
    } else {
        snprintf(out, cap, "%lu мин", static_cast<unsigned long>(minutes));
    }
}

}  // namespace forceon
