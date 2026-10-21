#include "SystemClock.h"

void SystemClock::apply(uint32_t epoch, uint32_t atMs) {
    syncedEpoch_ = epoch;
    syncedAtMs_ = atMs;
    if (!synced_ && onFirstSync_ != nullptr) {
        onFirstSync_(epoch, atMs / 1000UL);
    }
    synced_ = true;
}

uint32_t SystemClock::nowUtc() const {
    return syncedEpoch_ + (millis() - syncedAtMs_) / 1000UL;
}
