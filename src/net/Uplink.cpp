#include "Uplink.h"

#include <Ethernet.h>
#include <WiFi.h>

const char* uplinkName(Uplink uplink) {
    switch (uplink) {
        case Uplink::WiFi: return "Wi-Fi";
        case Uplink::Ethernet: return "Ethernet";
        case Uplink::None: break;
    }
    return "нет";
}

// EMACInterface::set_as_default() ставит интерфейс маршрутом по умолчанию стека lwIP.
// WiFi.getNetwork() не трогает модуль Wi-Fi: только возвращает объект интерфейса.
bool routeThrough(Uplink uplink) {
    NetworkInterface* network = nullptr;
    switch (uplink) {
        case Uplink::WiFi: network = WiFi.getNetwork(); break;
        case Uplink::Ethernet: network = Ethernet.getNetwork(); break;
        case Uplink::None: break;
    }
    if (network == nullptr) return false;
    network->set_as_default();
    return true;
}
