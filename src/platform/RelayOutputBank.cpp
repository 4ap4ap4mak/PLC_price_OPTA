#include "RelayOutputBank.h"

#include <Arduino.h>

#include "Log.h"

using channelconfig::kChannelCount;

namespace {

const pin_size_t kRelayPins[kChannelCount] = {D0, D1, D2, D3};
const pin_size_t kRelayLeds[kChannelCount] = {LED_D0, LED_D1, LED_D2, LED_D3};

}  // namespace

void RelayOutputBank::begin() {
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        pinMode(kRelayPins[i], OUTPUT);
        pinMode(kRelayLeds[i], OUTPUT);
    }
}

void RelayOutputBank::set(uint8_t channel, bool on) {
    if (channel >= kChannelCount) return;
    states_[channel] = on;
    digitalWrite(kRelayPins[channel], on ? HIGH : LOW);
    digitalWrite(kRelayLeds[channel], on ? HIGH : LOW);
    logLine("RELAY", "Реле %u: %s", channel + 1U, on ? "ВКЛЮЧЕНО" : "ВЫКЛЮЧЕНО");
}

bool RelayOutputBank::get(uint8_t channel) const {
    return channel < kChannelCount && states_[channel];
}
