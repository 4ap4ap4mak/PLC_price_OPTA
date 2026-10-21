// Сеть устройства: Wi-Fi (сохраненная сеть, точка настройки Opta-Config-AP с captive
// DNS, возврат из точки раз в 5 минут, переподключение), Ethernet (DHCP, смена адреса),
// HTTP-сокеты порта 80 и mDNS на обоих интерфейсах. Принятые HTTP-соединения
// передаются HttpClientHandler (WebServer).
//
// Сокеты живут здесь, а не в WebServer: их запуск и остановка привязаны к переходам
// сети (точка настройки ↔ домашняя сеть, смена IP Ethernet).
//
// Wi-Fi — явный автомат состояний (D-029): попытка подключения идет в потоке
// WifiConnector, DHCP Ethernet — в фоне ядра mbed, loop() их не ждет (KI-006).
// После запуска — несколько попыток с паузами, и только потом точка настройки (KI-001).
// Блокирует еще запуск и остановка точки настройки (WiFi.beginAP/disconnect).
#pragma once

#include <Arduino.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <WifiCredentialStore.h>

#include "CaptiveDns.h"
#include "ListeningSockets.h"
#include "MdnsResponder.h"
#include "WifiConnector.h"

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

    // setup(): поток подключения, DHCP Ethernet, первая попытка Wi-Fi через 3 с (или
    // раньше, когда Ethernet получит адрес). Не ждет подключения: о нем сообщает
    // service().
    void begin(HttpClientHandler& handler);

    // loop(): Ethernet, шаги автомата Wi-Fi, mDNS, captive DNS, прием HTTP. true —
    // Wi-Fi подключен к сохраненной сети. false — подключение идет, точка настройки
    // или Wi-Fi нет: остаток loop() (статус раз в 10 с, расписание цены) пропускается.
    bool service(uint32_t nowMs);

    // Долгое нажатие USER: точка настройки или поля Wi-Fi на странице по Ethernet.
    // Если идет попытка подключения, точка запускается после нее.
    void requestPortal();

    bool wifiConnected() const { return WiFi.status() == WL_CONNECTED; }
    // Показывать на странице поля SSID и пароля Wi-Fi.
    bool wifiFieldsVisible() const { return wifiCredentialsVisible_; }
    // Попытка подключения Wi-Fi идет дольше WifiConnector::kStuckMs.
    bool stuck(uint32_t nowMs) { return connector_.stuck(nowMs); }

private:
    enum class WifiState : uint8_t {
        Off,         // Wi-Fi не используется: модуль не найден или смена режима
        Waiting,     // пауза перед следующей попыткой
        Connecting,  // попытка в потоке WifiConnector
        Online,      // подключен к сохраненной сети
        Portal,      // точка настройки Opta-Config-AP
    };
    // Чем закончится неудачная попытка
    enum class Series : uint8_t {
        Boot,          // после запуска: следующая попытка с паузой, после последней —
                       // точка настройки или поля Wi-Fi по Ethernet
        Reconnect,     // связь потеряна или не появилась после запуска: раз в 10 с,
                       // после трех неудач — точка настройки, если нет Ethernet
        PortalReturn,  // из точки настройки раз в 5 минут (D-002): неудача — снова точка
    };

    void serviceWifi(uint32_t nowMs);
    void startAttempt();
    void finishAttempt(bool connected);
    void onConnected();
    void onAttemptFailed(uint32_t durationMs);
    void onNoCredentials();
    void openSettingsAccess(bool afterBoot);
    void startSeries(Series series);
    void waitThen(unsigned long waitMs, bool wiredEndsWait);

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
    WifiConnector connector_;

    arduino::WiFiUDP dnsUdp_;
    arduino::WiFiUDP mdnsUdp_;
    arduino::EthernetUDP wiredMdnsUdp_;
    CheckedWiFiServer portalServer_{80};
    CheckedEthernetServer wiredServer_{80};
    CaptiveDns captiveDns_;
    MdnsResponder wifiMdns_;
    MdnsResponder wiredMdns_;

    WifiState state_ = WifiState::Off;
    Series series_ = Series::Boot;
    uint8_t attempts_ = 0;            // попыток в текущей серии
    unsigned long waitStartMillis_ = 0;
    unsigned long waitMillis_ = 0;
    bool wiredEndsWait_ = false;      // пауза заканчивается, как только Ethernet получил адрес
    bool portalRequested_ = false;    // долгое нажатие USER во время попытки

    unsigned long lastPortalReconnectMillis_ = 0;
    unsigned long lastPortalActivityMillis_ = 0;
    bool wifiCredentialsVisible_ = false;
    bool settingsServerStarted_ = false;
    bool wiredServerStarted_ = false;
    bool wiredDhcpAttempted_ = false;
    // Интервалы считаются от момента попытки: now - start >= interval (KI-012)
    unsigned long lastEthernetAttemptMillis_ = 0;
    bool wiredServerRetryPending_ = false;
    unsigned long wiredServerFailedMillis_ = 0;
    IPAddress wiredServerAddress_;
};
