#include "PressDetector.h"

namespace userbutton {

Event PressDetector::update(uint32_t nowMs, bool pressed) {
    Event event = Event::None;
    if (pressed != lastReading_) {
        lastChangeMs_ = nowMs;
    }

    if (nowMs - lastChangeMs_ > kDebounceMs && pressed != stablePressed_) {
        stablePressed_ = pressed;
        if (stablePressed_) {
            pressedAtMs_ = nowMs;
            pressActive_ = true;
            longReported_ = false;
            event = Event::Pressed;
        } else {
            if (pressActive_ && !longReported_) {
                event = Event::ShortPress;
            }
            pressActive_ = false;
        }
    }
    lastReading_ = pressed;

    if (pressActive_ && !longReported_ && nowMs - pressedAtMs_ >= kLongPressMs) {
        longReported_ = true;
        event = Event::LongPress;
    }
    return event;
}

}  // namespace userbutton
