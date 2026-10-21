// Ответчик mDNS для имени <hostLabel>.local на одном интерфейсе (Wi-Fi или Ethernet).
// Сборка ответа — lib/dnspacket, здесь — UDP-сокет группы 224.0.0.251:5353 и лог [mDNS].
// Неблокирующий: service() обрабатывает не больше одного пакета за вызов.
#pragma once

#include <Arduino.h>

class MdnsResponder {
public:
    MdnsResponder(arduino::UDP& udp, const char* hostLabel, const char* interfaceName)
        : udp_(udp), hostLabel_(hostLabel), interfaceName_(interfaceName) {}

    // Подписка на группу mDNS. При неудаче сокет не закрывается: решает вызывающий.
    bool begin();
    // Закрыть сокет (даже если begin() не удался) и сбросить признак запуска.
    void stop();
    bool started() const { return started_; }

    // Ответить на запрос своего имени адресом address.
    void service(const IPAddress& address);

private:
    arduino::UDP& udp_;
    const char* hostLabel_;
    const char* interfaceName_;
    bool started_ = false;
};
