#include "WifiConnector.h"

#include <WiFi.h>
#include <string.h>

WifiConnector::WifiConnector()
    : thread_(osPriorityNormal, kStackSize, stack_, "wifi") {}

// Приоритет как у loop() и FetchWorker: RTX делит время по очереди (round-robin).
bool WifiConnector::begin() {
    started_ = thread_.start(mbed::callback(this, &WifiConnector::run)) == osOK;
    return started_;
}

void WifiConnector::run() {
    for (;;) {
        flags_.wait_any(kJobFlag);
        mutex_.lock();
        const bool queued = state_ == JobState::Queued;
        if (queued) {
            state_ = JobState::Running;
            runningSinceMs_ = millis();
        }
        mutex_.unlock();
        if (!queued) continue;

        uint32_t durationMs = 0;
        const bool ok = connect(durationMs);
        mutex_.lock();
        durationMs_ = durationMs;
        state_ = ok ? JobState::Done : JobState::Failed;
        mutex_.unlock();
    }
}

// Строки не меняются, пока попытка идет: start() их не пишет, пока state_ не Idle.
bool WifiConnector::connect(uint32_t& durationMs) {
    const uint32_t startMs = millis();
    const int status = WiFi.begin(ssid_, password_);
    durationMs = millis() - startMs;
    return status == WL_CONNECTED;
}

bool WifiConnector::start(const char* ssid, const char* password) {
    mutex_.lock();
    const bool idle = state_ == JobState::Idle;
    if (idle) {
        strncpy(ssid_, ssid, sizeof(ssid_) - 1);
        ssid_[sizeof(ssid_) - 1] = '\0';
        strncpy(password_, password, sizeof(password_) - 1);
        password_[sizeof(password_) - 1] = '\0';
        state_ = JobState::Queued;
    }
    mutex_.unlock();
    if (!idle) return false;

    if (started_) {
        flags_.set(kJobFlag);
        return true;
    }
    // Поток не запустился: подключение блокирует основной поток, как раньше
    uint32_t durationMs = 0;
    const bool ok = connect(durationMs);
    mutex_.lock();
    durationMs_ = durationMs;
    state_ = ok ? JobState::Done : JobState::Failed;
    mutex_.unlock();
    return true;
}

WifiConnector::Result WifiConnector::poll() {
    mutex_.lock();
    Result result = Result::Idle;
    switch (state_) {
        case JobState::Idle: result = Result::Idle; break;
        case JobState::Queued:
        case JobState::Running: result = Result::Busy; break;
        case JobState::Done: state_ = JobState::Idle; result = Result::Connected; break;
        case JobState::Failed: state_ = JobState::Idle; result = Result::Failed; break;
    }
    mutex_.unlock();
    return result;
}

uint32_t WifiConnector::lastDurationMs() {
    mutex_.lock();
    const uint32_t result = durationMs_;
    mutex_.unlock();
    return result;
}

bool WifiConnector::stuck(uint32_t nowMs) {
    mutex_.lock();
    const bool result = state_ == JobState::Running && nowMs - runningSinceMs_ >= kStuckMs;
    mutex_.unlock();
    return result;
}
