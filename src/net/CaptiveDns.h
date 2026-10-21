// DNS точки настройки (captive portal): на любой A-запрос отвечает адресом точки,
// чтобы телефон сам открыл страницу настроек. Сборка ответа — lib/dnspacket.
// Неблокирующий: service() обрабатывает не больше одного пакета за вызов.
#pragma once

#include <Arduino.h>

class CaptiveDns {
public:
    explicit CaptiveDns(arduino::UDP& udp) : udp_(udp) {}

    // Открыть UDP-порт 53, если еще не открыт.
    bool begin();
    // Закрыть порт, если открыт.
    void stop();
    // Ответить на запрос адресом portalIp.
    void service(const IPAddress& portalIp);

private:
    arduino::UDP& udp_;
    bool started_ = false;
};
