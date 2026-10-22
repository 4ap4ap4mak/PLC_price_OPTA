// Сеть устройства: Wi-Fi (сохраненная сеть, точка настройки Opta-Config-AP с captive
// DNS, возврат из точки раз в 5 минут, переподключение), Ethernet (DHCP, смена адреса),
// HTTP-сервер порта 80 и mDNS. Принятые HTTP-соединения передаются HttpClientHandler
// (WebServer).
//
// Один HTTP-сервер и один mDNS на все интерфейсы (D-033): сокетов в ядре mbed 4 на
// устройство (KI-017). Сервер слушает порт на всех адресах общего стека и после
// запуска не перезапускается; mDNS отвечает адресом Ethernet, если он есть, иначе
// адресом Wi-Fi или точки настройки, и перезапускается при смене этого адреса.
//
// Wi-Fi — явный автомат состояний (D-029): попытка подключения идет в потоке
// WifiConnector, DHCP Ethernet — в фоне ядра mbed, loop() их не ждет (KI-006).
// После запуска — несколько попыток с паузами, и только потом точка настройки (KI-001).
// Новая сеть со страницы — без перезагрузки, такой же серией попыток (D-031).
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
#include "Uplink.h"
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
    // есть связь с интернетом (uplink() не None): Wi-Fi подключен к сохраненной сети
    // или у Ethernet есть адрес (D-036). false — запросы цены и времени не ставятся.
    bool service(uint32_t nowMs);

    // Канал для запросов в интернет: Wi-Fi, если подключен к сохраненной сети, иначе
    // Ethernet с адресом (D-036).
    Uplink uplink() const;
    // Wi-Fi подключен к сохраненной сети и не занят потоком подключения: можно
    // спрашивать WiFi.SSID() и RSSI().
    bool wifiOnline() const { return state_ == WifiState::Online; }
    // Адрес Ethernet; 0.0.0.0 — нет.
    IPAddress wiredAddress() const { return wiredAddress_; }

    // Долгое нажатие USER: точка настройки или поля Wi-Fi на странице по Ethernet.
    // Если идет попытка подключения, точка запускается после нее.
    void requestPortal();
    // Новые учетные данные уже записаны: подключиться к ним без перезагрузки (D-031).
    // Переход — через 1,5 с, чтобы ответ страницы успел уйти до остановки точки
    // настройки, и не раньше конца идущей попытки; итог той попытки отбрасывается.
    void applyNewCredentials();

    // Показывать на странице поля SSID и пароля Wi-Fi.
    bool wifiFieldsVisible() const { return wifiCredentialsVisible_; }
    // Попытка подключения Wi-Fi идет дольше WifiConnector::kStuckMs.
    bool stuck(uint32_t nowMs) { return connector_.stuck(nowMs); }
    // Команда sockets (D-031): сколько сокетов ядра mbed свободно — проверка открытием
    // временных UDP-сокетов, они сразу закрываются, — и какие заняты прошивкой
    // постоянно. fetchInProgress — идет запрос цены. Во время запроса цены и подключения
    // Wi-Fi проверка не идет: временные сокеты помешали бы им. Сокет NTP открыт только
    // на время синхронизации (~1,5 с, D-032) и в отчет не входит.
    void printSocketReport(bool fetchInProgress);

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
        NewCredentials,  // новая сеть со страницы (D-031): несколько попыток с паузами,
                         // после последней — точка настройки или поля Wi-Fi по Ethernet
    };

    // Команда пользователя, которая ждет конца попытки подключения
    enum class Request : uint8_t {
        None,
        Portal,          // долгое нажатие USER: точка настройки
        NewCredentials,  // новая сеть со страницы (D-031)
    };

    void serviceWifi(uint32_t nowMs);
    bool servicePendingRequest(uint32_t nowMs);
    void switchToNewCredentials();
    // Серия со счетом попыток: паузы между ними, после последней — доступ к настройкам.
    struct RetryPlan {
        const unsigned long* delaysMs;  // attempts - 1 пауз
        uint8_t attempts;
    };
    // План серии со счетом попыток (Boot, NewCredentials); nullptr — у серии его нет.
    const RetryPlan* retryPlan() const;
    void resetStation();
    void stopWifiMdns();
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
    // Открыть HTTP-сервер, если еще не открыт; после ошибки — не чаще раза в 5 с,
    // force — сразу. true — сервер слушает.
    bool ensureHttpServer(bool force);
    // Адрес для ответов mDNS: Ethernet, точка настройки или Wi-Fi; 0.0.0.0 — нет сети.
    void serviceMdns(uint32_t nowMs);
    void stopMdns();
    void serviceWiredNetwork(bool forceAttempt = false);
    void serviceHttpClients();

    configstore::WifiCredentialStore& credentials_;
    HttpClientHandler* handler_ = nullptr;
    WifiConnector connector_;

    arduino::WiFiUDP dnsUdp_;
    // Через стек Ethernet: он общий с Wi-Fi, а интерфейс Ethernet есть всегда, поэтому
    // открытие сокета не трогает WiFi, пока его держит поток подключения
    arduino::EthernetUDP mdnsUdp_;
    CheckedWiFiServer httpServer_{80};
    CaptiveDns captiveDns_;
    MdnsResponder mdns_;

    WifiState state_ = WifiState::Off;
    Series series_ = Series::Boot;
    uint8_t attempts_ = 0;            // попыток в текущей серии
    unsigned long waitStartMillis_ = 0;
    unsigned long waitMillis_ = 0;
    bool wiredEndsWait_ = false;      // пауза заканчивается, как только Ethernet получил адрес
    // Последняя команда пользователя побеждает: новая заменяет ждущую
    Request pending_ = Request::None;
    unsigned long pendingSinceMillis_ = 0;

    unsigned long lastPortalReconnectMillis_ = 0;
    unsigned long lastPortalActivityMillis_ = 0;
    bool wifiCredentialsVisible_ = false;
    bool httpServerStarted_ = false;
    bool httpServerRetryPending_ = false;
    // У Ethernet есть адрес: страница и mDNS — по проводу, точка настройки не нужна
    bool wiredReady_ = false;
    IPAddress wiredAddress_;
    bool wiredDhcpAttempted_ = false;
    // Интервалы считаются от момента попытки: now - start >= interval (KI-012)
    unsigned long lastEthernetAttemptMillis_ = 0;
    unsigned long httpServerFailedMillis_ = 0;
    // Адрес в ответах mDNS; 0.0.0.0 — mDNS не запущен
    IPAddress mdnsAddress_;
    bool mdnsRetryPending_ = false;
    unsigned long mdnsFailedMillis_ = 0;
};
