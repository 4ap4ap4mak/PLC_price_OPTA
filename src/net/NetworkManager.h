// Сеть устройства: Wi-Fi (сохраненная сеть, точка настройки Opta-Config-AP с captive
// DNS, возврат из точки раз в 5 минут, переподключение), Ethernet (DHCP, смена адреса),
// HTTP-сокеты порта 80 и mDNS на обоих интерфейсах. Принятые HTTP-соединения
// передаются HttpClientHandler (WebServer).
//
// Сокеты живут здесь, а не в WebServer: их запуск и остановка привязаны к переходам
// сети (точка настройки ↔ домашняя сеть, смена IP Ethernet).
// Блокирует: подключение Wi-Fi до 10 с, DHCP Ethernet 1–3 с (KI-006); внутри циклов
// ожидания сбрасывает watchdog. Перенесено из монолита без изменения поведения (шаг 2f).
#pragma once

#include <Arduino.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <WifiCredentialStore.h>

#include "CaptiveDns.h"
#include "ListeningSockets.h"
#include "MdnsResponder.h"

// Обработчик одного принятого HTTP-соединения: читает запрос, отвечает, закрывает.
class HttpClientHandler {
public:
    virtual void handleClient(arduino::Client& client) = 0;

protected:
    ~HttpClientHandler() = default;
};

class NetworkManager {
public:
    explicit NetworkManager(configstore::WifiCredentialStore& credentials);

    // setup(): Ethernet, затем сохраненная сеть Wi-Fi (до 10 с), иначе точка настройки
    // или страница по Ethernet. true — Wi-Fi подключен: можно синхронизировать часы и
    // запросить цену. Место для исправления KI-001.
    bool begin(HttpClientHandler& handler);

    // loop(): Ethernet, mDNS, captive DNS, прием HTTP, возврат из точки настройки,
    // переподключение Wi-Fi. false — точка настройки или Wi-Fi потерян: остаток loop()
    // (статус раз в 10 с, расписание цены) пропускается, как в монолите.
    bool service(uint32_t nowMs);

    // Долгое нажатие USER: точка настройки или поля Wi-Fi на странице по Ethernet.
    void requestPortal();

    bool wifiConnected() const { return WiFi.status() == WL_CONNECTED; }
    // Показывать на странице поля SSID и пароля Wi-Fi.
    bool wifiFieldsVisible() const { return wifiCredentialsVisible_; }

private:
    void configureEthernetHostname();
    bool startWifiManager();
    void stopWifiManager();
    void servicePortalReconnect();
    bool startSettingsWebServer();
    void startWiredWebServer();
    void serviceWiredNetwork(bool forceAttempt = false);
    void serviceMDNS();
    void serviceWifiManager();
    void serviceSettingsWebServer();
    void serviceWiredWebServer();

    configstore::WifiCredentialStore& credentials_;
    HttpClientHandler* handler_ = nullptr;

    arduino::WiFiUDP dnsUdp_;
    arduino::WiFiUDP mdnsUdp_;
    arduino::EthernetUDP wiredMdnsUdp_;
    CheckedWiFiServer portalServer_{80};
    CheckedEthernetServer wiredServer_{80};
    CaptiveDns captiveDns_;
    MdnsResponder wifiMdns_;
    MdnsResponder wiredMdns_;

    bool portalMode_ = false;
    unsigned long lastPortalReconnectMillis_ = 0;
    unsigned long lastPortalActivityMillis_ = 0;
    bool wifiCredentialsVisible_ = false;
    bool settingsServerStarted_ = false;
    bool wiredServerStarted_ = false;
    bool wiredDhcpAttempted_ = false;
    unsigned long nextEthernetAttemptMillis_ = 0;
    unsigned long nextWiredServerAttemptMillis_ = 0;
    IPAddress wiredServerAddress_;
    unsigned long lastReconnectAttempt_ = 0;
    uint8_t reconnectAttempts_ = 0;
};
