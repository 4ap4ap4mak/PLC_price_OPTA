// Внешний пульт управления — Home Assistant через MQTT (D-042). Пока он подключен,
// страница только для просмотра: плашка «Смотрите Home Assistant», кнопки и поля
// серые, запросы на изменение (POST) отклоняются. Без пульта страница полная.
// Реализует соединение с брокером MQTT; вызывается только из основного потока.
#pragma once

class RemoteControlStatus {
public:
    // true — пульт подключен (соединение с брокером открыто).
    virtual bool remoteControlActive() = 0;

protected:
    ~RemoteControlStatus() = default;
};
