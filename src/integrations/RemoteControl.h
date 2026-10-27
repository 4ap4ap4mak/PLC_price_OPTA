// Внешний пульт управления — Home Assistant через MQTT (D-042). Реализует соединение
// с брокером MQTT; вызывается только из основного потока.
//
// Пока пульт подключен, страница только для просмотра: плашка «Смотрите Home
// Assistant», кнопки и поля серые, запросы на изменение (POST) отклоняются. Без пульта
// страница полная. Брокер задают страница (POST /mqtt) и консоль (mqtt): настройки
// сохраняются во flash (MqttSettingsStore) и передаются сюда без перезагрузки.
#pragma once

#include <MqttSettingsStore.h>

class RemoteControl {
public:
    // true — пульт подключен (соединение с брокером открыто).
    virtual bool remoteControlActive() = 0;
    // Новые настройки брокера: переподключиться к нему; пустой адрес — выключить.
    virtual void applyBrokerSettings(const configstore::MqttSettings& settings) = 0;

protected:
    ~RemoteControl() = default;
};
