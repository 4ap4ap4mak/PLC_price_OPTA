// Ответчик mDNS для имени <hostLabel>.local. Один на устройство (D-033): работает на
// Ethernet, если у него есть адрес, иначе на Wi-Fi; interfaceName — для лога.
// Сборка ответа — lib/dnspacket, здесь — UDP-сокет группы 224.0.0.251:5353 и лог [mDNS].
// Неблокирующий: service() обрабатывает не больше одного пакета за вызов.
#pragma once

#include <Arduino.h>

class MdnsResponder {
public:
    MdnsResponder(arduino::UDP& udp, const char* hostLabel)
        : udp_(udp), hostLabel_(hostLabel) {}

    // Подписка на группу mDNS. Подписка идет на интерфейсах, поднятых к этому моменту,
    // поэтому при смене интерфейса нужен stop() и новый begin(). При неудаче сокет не
    // закрывается: решает вызывающий.
    bool begin(const char* interfaceName);
    // Закрыть сокет (даже если begin() не удался) и сбросить признак запуска.
    void stop();
    bool started() const { return started_; }

    // Ответить на запрос своего имени адресом address.
    void service(const IPAddress& address);

private:
    arduino::UDP& udp_;
    const char* hostLabel_;
    const char* interfaceName_ = "";
    bool started_ = false;
};
