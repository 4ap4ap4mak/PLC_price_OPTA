// Выходы каналов: реле D0–D3 и их светодиоды. Хранит текущее состояние выходов
// и печатает каждое переключение. Историю переключений пишет Controller.
#pragma once

#include <stdint.h>

#include <ChannelConfig.h>
#include <ControllerPorts.h>

class RelayOutputBank : public controller::Outputs {
public:
    // Пины выходов и светодиодов — на вывод. Состояние в RAM — «выключено».
    void begin();
    // Пишет выход и светодиод всегда, даже если состояние не меняется.
    // Канал вне диапазона игнорируется.
    void set(uint8_t channel, bool on) override;
    bool get(uint8_t channel) const override;

private:
    bool states_[channelconfig::kChannelCount] = {};
};
