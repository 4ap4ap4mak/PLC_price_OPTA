// TCP-сервер порта 80 с кодом ошибки запуска. Стандартный begin() ядра mbed не
// сообщает об ошибке; здесь возвращается код Mbed.
//
// Один сервер на все интерфейсы (D-033): Wi-Fi (домашняя сеть и точка настройки)
// и Ethernet делят один сетевой стек ядра mbed (OnboardNetworkStack), поэтому сокет,
// привязанный к порту без адреса, принимает подключения с любого из них. Сокет
// помнит только стек, а не интерфейс: смена режима Wi-Fi и адреса Ethernet его не
// закрывает.
// Неблокирующий: accept() сразу возвращает пустой клиент, если соединений нет.
#pragma once

#include <WiFi.h>
#include <WiFiServer.h>

class CheckedWiFiServer : public arduino::WiFiServer {
public:
    using arduino::WiFiServer::WiFiServer;

    void end();
    // Слушать порт на всех интерфейсах стека, к которому относится network.
    nsapi_error_t beginAnyAddress(NetworkInterface* network);
    // Этап, на котором остановился последний запуск: allocate, open, bind, listen.
    const char* lastErrorStage() const {
        return errorStage;
    }

private:
    const char* errorStage = "not started";
};
