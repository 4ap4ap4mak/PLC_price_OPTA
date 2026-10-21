// TCP-серверы порта 80 для Wi-Fi и Ethernet с кодом ошибки запуска.
// Стандартный begin() ядра mbed не сообщает об ошибке и слушает на всех
// интерфейсах; здесь сокет привязывается явно и возвращается код Mbed.
// Неблокирующие: accept() сразу возвращает пустой клиент, если соединений нет.
#pragma once

#include <Ethernet.h>
#include <WiFi.h>
#include <WiFiServer.h>

class CheckedWiFiServer : public arduino::WiFiServer {
public:
    using arduino::WiFiServer::WiFiServer;

    void end();
    // Слушать только на адресе ipAddress (точка настройки или адрес в сети).
    nsapi_error_t beginOnAddress(const IPAddress& ipAddress);
};

class CheckedEthernetServer : public arduino::EthernetServer {
public:
    using arduino::EthernetServer::EthernetServer;

    void end();
    nsapi_error_t beginListening();
    // Этап, на котором остановился последний запуск: allocate, open, bind, listen.
    const char* lastErrorStage() const {
        return errorStage;
    }

private:
    const char* errorStage = "not started";
};
