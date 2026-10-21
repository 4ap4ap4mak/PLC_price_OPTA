// Serial-консоль: команды status/update/on/off/auto/force/history (docs/OPERATIONS.md),
// подробный отчет о состоянии и краткий статус раз в 10 с.
// Команды update и auto запрашивают цену синхронно (до 20 с, KI-006).
#pragma once

#include <Arduino.h>

#include <Controller.h>
#include <ControllerPorts.h>
#include <SwitchHistory.h>

class SerialConsole {
public:
    SerialConsole(controller::Controller& channels, const controller::Clock& clock,
        const history::SwitchHistory& switchHistory)
        : channels_(channels), clock_(clock), switchHistory_(switchHistory) {}

    // Выполнить команду, если в Serial пришла строка.
    void service();
    // Краткий статус раз в 10 с; nowMs — millis() начала итерации loop().
    void serviceHeartbeat(uint32_t nowMs);
    void printStatusReport();

private:
    // Последние переключения, новые сверху. channel == kDeviceEvent — все реле.
    void printSwitchHistory(uint8_t channel);

    controller::Controller& channels_;
    const controller::Clock& clock_;
    const history::SwitchHistory& switchHistory_;
    // Реле для команд без номера: последнее названное в консоли, с вебом не связано
    uint8_t selectedChannel_ = 0;
    unsigned long lastHeartbeatMillis_ = 0;
};
