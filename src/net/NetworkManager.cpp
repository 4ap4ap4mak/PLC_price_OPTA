#include "NetworkManager.h"

#include <string.h>

#include "../platform/Log.h"
#include "../platform/Watchdog.h"

using configstore::WifiCredentials;

// В режиме точки настройки повторять подключение к сохраненной сети
static const unsigned long PORTAL_RECONNECT_INTERVAL_MS = 5UL * 60UL * 1000UL;
// Не прерывать точку настройки, пока пользователь недавно открывал страницу
static const unsigned long PORTAL_ACTIVITY_GRACE_MS = 2UL * 60UL * 1000UL;
// После запуска: паузы между попытками подключиться к сохраненной сети, после
// последней — точка настройки (KI-001, D-029). Сразу после прошивки модуль Wi-Fi
// какое-то время не подключается, хотя сеть доступна.
static const unsigned long BOOT_RETRY_DELAYS_MS[] = {5000UL, 15000UL, 30000UL, 60000UL};
static const uint8_t BOOT_ATTEMPTS =
    sizeof(BOOT_RETRY_DELAYS_MS) / sizeof(BOOT_RETRY_DELAYS_MS[0]) + 1;
// Связь потеряна: попытка раз в 10 с, после трех неудач — точка настройки
static const unsigned long RECONNECT_INTERVAL_MS = 10000UL;
static const uint8_t RECONNECT_ATTEMPTS_BEFORE_PORTAL = 3;
// После запуска первая попытка Wi-Fi ждет адрес Ethernet не дольше этого: с ним
// точка настройки не нужна (как прежнее ожидание DHCP в setup())
static const unsigned long WIRED_WAIT_MS = 3000UL;
static const char* CONFIG_AP_SSID = "Opta-Config-AP";
static const char* CONFIG_AP_PASSWORD = "opta1234";
static const char* DEVICE_HOSTNAME = "opta-settings";
static const IPAddress CONFIG_AP_IP(192, 168, 4, 1);
// Повтор DHCP Ethernet без адреса и повтор запуска HTTP-сервера Ethernet после ошибки
static const unsigned long ETHERNET_RETRY_INTERVAL_MS = 15000UL;
static const unsigned long WIRED_SERVER_RETRY_INTERVAL_MS = 5000UL;

NetworkManager::NetworkManager(configstore::WifiCredentialStore& credentials)
    : credentials_(credentials),
      captiveDns_(dnsUdp_),
      wifiMdns_(mdnsUdp_, DEVICE_HOSTNAME, "Wi-Fi"),
      wiredMdns_(wiredMdnsUdp_, DEVICE_HOSTNAME, "Ethernet") {}

void NetworkManager::configureEthernetHostname() {
    NetworkInterface* network = Ethernet.getNetwork();
    if (network == nullptr) {
        Serial.println("[Ethernet] Не удалось задать имя: сетевой интерфейс недоступен.");
        return;
    }
    nsapi_error_t result = network->set_hostname(DEVICE_HOSTNAME);
    if (result == NSAPI_ERROR_OK) {
        Serial.print("[Ethernet] Имя устройства: ");
        Serial.println(DEVICE_HOSTNAME);
    } else {
        Serial.print("[Ethernet] Не удалось задать имя устройства, код ошибки: ");
        Serial.println(result);
    }
}

void NetworkManager::begin(HttpClientHandler& handler) {
    handler_ = &handler;
    if (!connector_.begin()) {
        Serial.println("[ERROR] Не удалось запустить поток подключения Wi-Fi; подключение будет останавливать loop().");
    }

    Serial.println("[Ethernet] Проверка проводного подключения...");
    configureEthernetHostname();
    serviceWiredNetwork(true);

    if (WiFi.status() == WL_NO_SHIELD) {
        Serial.println("[ERROR] Модуль Wi-Fi не обнаружен на плате!");
        return;
    }
    startSeries(Series::Boot);
    waitThen(WIRED_WAIT_MS, true);
}

bool NetworkManager::service(uint32_t nowMs) {
    serviceWiredNetwork();
    serviceWifi(nowMs);
    if (state_ == WifiState::Portal) {
        serviceWifiManager();
        servicePortalReconnect();
        return false;
    }
    const bool online = state_ == WifiState::Online;
    if (online) {
        serviceMDNS();
        serviceSettingsWebServer();
    }
    serviceWiredWebServer();
    return online;
}

// Шаг автомата Wi-Fi: итог попытки, конец паузы, потеря связи.
void NetworkManager::serviceWifi(uint32_t nowMs) {
    switch (state_) {
        case WifiState::Connecting: {
            const WifiConnector::Result result = connector_.poll();
            if (result == WifiConnector::Result::Busy) return;
            // Idle на месте итога — итог потерян: считаем неудачей, иначе автомат встанет
            finishAttempt(result == WifiConnector::Result::Connected);
            return;
        }
        case WifiState::Waiting:
            if (nowMs - waitStartMillis_ >= waitMillis_ ||
                (wiredEndsWait_ && wiredServerStarted_)) {
                startAttempt();
            }
            return;
        case WifiState::Online:
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WiFi] Соединение потеряно, повторное подключение...");
                startSeries(Series::Reconnect);
                startAttempt();
            }
            return;
        case WifiState::Off:
        case WifiState::Portal:
            return;
    }
}

void NetworkManager::startSeries(Series series) {
    series_ = series;
    attempts_ = 0;
}

void NetworkManager::waitThen(unsigned long waitMs, bool wiredEndsWait) {
    state_ = WifiState::Waiting;
    waitStartMillis_ = millis();
    waitMillis_ = waitMs;
    wiredEndsWait_ = wiredEndsWait;
}

// Следующая попытка текущей серии. Без сохраненной сети — сразу доступ к настройкам.
void NetworkManager::startAttempt() {
    WifiCredentials credentials = {};
    if (!credentials_.load(credentials)) {
        onNoCredentials();
        return;
    }
    ++attempts_;
    if (series_ == Series::Boot) {
        Serial.print("[WiFi] Подключение к сохраненной сети: \"");
        Serial.print(credentials.ssid);
        if (attempts_ > 1) {
            Serial.print("\", попытка ");
            Serial.print(attempts_);
            Serial.print(" из ");
            Serial.print(BOOT_ATTEMPTS);
            Serial.println("...");
        } else {
            Serial.println("\"...");
        }
    }
    // Сброс прежней попытки. INADDR_NONE — на случай, если до этого не запустилась
    // точка доступа со статическим IP. Первую попытку после запуска и возврат из точки
    // настройки (ее уже остановил stopWifiManager) не трогаем, как раньше.
    if (series_ == Series::Reconnect || (series_ == Series::Boot && attempts_ > 1)) {
        WiFi.disconnect();
        WiFi.config(INADDR_NONE);
    }
    WiFi.setHostname(DEVICE_HOSTNAME);
    // false не бывает: новая попытка начинается только после итога прежней, а итог
    // прежней тогда придет в serviceWifi()
    connector_.start(credentials.ssid, credentials.password);
    state_ = WifiState::Connecting;
}

void NetworkManager::finishAttempt(bool connected) {
    const uint32_t durationMs = connector_.lastDurationMs();
    if (portalRequested_) {
        // Долгое нажатие USER во время попытки: точка настройки важнее подключения
        portalRequested_ = false;
        if (startWifiManager()) return;
        Serial.println("[ERROR] Не удалось запустить точку настройки Wi-Fi по кнопке USER.");
        if (state_ == WifiState::Portal) return;
        startSeries(Series::Reconnect);
        waitThen(RECONNECT_INTERVAL_MS, false);
        return;
    }
    if (connected) {
        onConnected();
    } else {
        onAttemptFailed(durationMs);
    }
}

void NetworkManager::onConnected() {
    state_ = WifiState::Online;
    attempts_ = 0;
    switch (series_) {
        case Series::Boot:
            Serial.println("[WiFi] Успешно подключено к Wi-Fi!");
            Serial.print("[WiFi] IP-адрес: ");
            Serial.println(WiFi.localIP());
            Serial.print("[WiFi] Уровень сигнала (RSSI): ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");
            if (wiredServerStarted_) {
                Serial.println("[WEB] Используется Ethernet-сервер, чтобы сохранить ресурсы сетевого стека.");
                Serial.print("[WEB] Настройки доступны по адресу http://");
                Serial.println(Ethernet.localIP());
            } else {
                startSettingsWebServer();
            }
            break;
        case Series::Reconnect:
            Serial.print("[WiFi] Подключено снова. IP-адрес: ");
            Serial.println(WiFi.localIP());
            break;
        case Series::PortalReturn:
            wifiCredentialsVisible_ = false;
            Serial.print("[WiFi] Подключено, точка настройки закрыта. IP-адрес: ");
            Serial.println(WiFi.localIP());
            if (!wiredServerStarted_) {
                startSettingsWebServer();
            }
            break;
    }
}

void NetworkManager::onAttemptFailed(uint32_t durationMs) {
    switch (series_) {
        case Series::Boot:
            if (attempts_ < BOOT_ATTEMPTS) {
                const unsigned long waitMs = BOOT_RETRY_DELAYS_MS[attempts_ - 1];
                logLine("WiFi", "Попытка %u из %u не удалась (%lu мс); следующая через %lu с.",
                    static_cast<unsigned>(attempts_), static_cast<unsigned>(BOOT_ATTEMPTS),
                    static_cast<unsigned long>(durationMs),
                    waitMs / 1000UL);
                waitThen(waitMs, false);
            } else {
                logLine("WiFi", "Попытка %u из %u не удалась (%lu мс).",
                    static_cast<unsigned>(attempts_), static_cast<unsigned>(BOOT_ATTEMPTS),
                    static_cast<unsigned long>(durationMs));
                openSettingsAccess(true);
            }
            return;
        case Series::Reconnect:
            logLine("WiFi", "Повторное подключение не удалось (попытка %u, %lu мс).",
                static_cast<unsigned>(attempts_), static_cast<unsigned long>(durationMs));
            if (attempts_ >= RECONNECT_ATTEMPTS_BEFORE_PORTAL && !wiredServerStarted_) {
                openSettingsAccess(false);
            } else {
                waitThen(RECONNECT_INTERVAL_MS, false);
            }
            return;
        case Series::PortalReturn:
            Serial.println("[WiFi] Сохраненная сеть недоступна; точка настройки запускается снова.");
            if (!startWifiManager()) {
                Serial.println("[ERROR] Не удалось повторно запустить точку настройки.");
                if (state_ != WifiState::Portal) {
                    startSeries(Series::Reconnect);
                    waitThen(RECONNECT_INTERVAL_MS, false);
                }
            }
            return;
    }
}

void NetworkManager::onNoCredentials() {
    if (series_ == Series::Boot) {
        Serial.println("[WiFi] Сохраненные учетные данные не найдены или повреждены.");
    }
    openSettingsAccess(series_ == Series::Boot);
}

// Сохраненной сети нет или она недоступна: поля Wi-Fi на странице по Ethernet, если
// он есть, иначе точка настройки. Пока точка не открыта, попытки идут раз в 10 с.
void NetworkManager::openSettingsAccess(bool afterBoot) {
    if (afterBoot) {
        Serial.println("\n[WiFi] Переход в режим конфигурации...");
    }
    startSeries(Series::Reconnect);
    if (wiredServerStarted_) {
        wifiCredentialsVisible_ = true;
        if (afterBoot) {
            Serial.print("[WEB] Точка доступа не запускается: настройки доступны по LAN адресу http://");
            Serial.println(Ethernet.localIP());
        }
        waitThen(RECONNECT_INTERVAL_MS, false);
        return;
    }
    if (startWifiManager()) {
        if (afterBoot) {
            Serial.println("[WiFi] Открыт портал настройки.");
        }
        return;
    }
    if (state_ != WifiState::Portal) {
        waitThen(RECONNECT_INTERVAL_MS, false);
    }
}

void NetworkManager::requestPortal() {
    if (wiredServerStarted_) {
        wifiCredentialsVisible_ = true;
        Serial.println("\n[WiFi] Настройки Wi-Fi доступны через Ethernet; точка доступа не запускается.");
        return;
    }
    Serial.println("\n[WiFi] USER удерживается 5 секунд; запуск режима настройки Wi-Fi.");
    if (state_ == WifiState::Connecting) {
        // WiFi занят потоком подключения: точку запустит finishAttempt()
        portalRequested_ = true;
        Serial.println("[WiFi] Точка настройки запустится после текущей попытки подключения.");
        return;
    }
    if (!startWifiManager()) {
        Serial.println("[ERROR] Не удалось запустить точку настройки Wi-Fi по кнопке USER.");
    }
}

// Вызывается, только когда попытка подключения не идет: WiFi не занят потоком.
bool NetworkManager::startWifiManager() {
    Serial.println("[WiFi] Запуск точки настройки Opta-Config-AP...");
    portalServer_.end();
    settingsServerStarted_ = false;
    captiveDns_.stop();
    if (wifiMdns_.started()) {
        wifiMdns_.stop();
    }
    WiFi.disconnect();
    WiFi.config("192.168.4.1", "255.255.255.0", "192.168.4.1");
    if (WiFi.beginAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD) != WL_AP_LISTENING) {
        Serial.println("[ERROR] Не удалось запустить точку доступа конфигурации.");
        return false;
    }
    if (!captiveDns_.begin()) {
        Serial.println("[ERROR] Не удалось открыть DNS-порт для Captive Portal.");
        WiFi.end();
        return false;
    }
    state_ = WifiState::Portal;
    lastPortalReconnectMillis_ = millis();
    wifiCredentialsVisible_ = true;
    if (!startSettingsWebServer()) {
        Serial.println("[ERROR] Не удалось запустить HTTP-сервер точки настройки.");
        return false;
    }
    Serial.print("[WiFi] IP точки настройки: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WiFi] Подключитесь к сети ");
    Serial.print(CONFIG_AP_SSID);
    Serial.print(" (пароль: ");
    Serial.print(CONFIG_AP_PASSWORD);
    Serial.println("), затем откройте http://192.168.4.1");
    return true;
}

void NetworkManager::stopWifiManager() {
    portalServer_.end();
    settingsServerStarted_ = false;
    captiveDns_.stop();
    if (wifiMdns_.started()) {
        wifiMdns_.stop();
    }
    // disconnect() останавливает точку доступа, end() переключает стек обратно на STA
    WiFi.disconnect();
    WiFi.end();
    // Сброс статического 192.168.4.1, заданного для точки доступа: в домашней сети нужен DHCP
    WiFi.config(INADDR_NONE);
    state_ = WifiState::Off;
}

// Точка настройки не должна оставаться навсегда после временного сбоя роутера:
// периодически пробуем вернуться к сохраненной сети. Попытка идет в потоке, итог —
// в onConnected() или onAttemptFailed().
void NetworkManager::servicePortalReconnect() {
    unsigned long now = millis();
    if (now - lastPortalReconnectMillis_ < PORTAL_RECONNECT_INTERVAL_MS ||
        now - lastPortalActivityMillis_ < PORTAL_ACTIVITY_GRACE_MS) {
        return;
    }
    lastPortalReconnectMillis_ = now;

    WifiCredentials credentials = {};
    if (!credentials_.load(credentials)) {
        return;
    }

    Serial.print("[WiFi] Точка настройки: попытка подключения к сохраненной сети \"");
    Serial.print(credentials.ssid);
    Serial.println("\"...");
    stopWifiManager();
    kickWatchdog();
    startSeries(Series::PortalReturn);
    startAttempt();
}

bool NetworkManager::startSettingsWebServer() {
    if (!settingsServerStarted_) {
        IPAddress bindAddress = state_ == WifiState::Portal ? CONFIG_AP_IP : WiFi.localIP();
        nsapi_error_t result = portalServer_.beginOnAddress(bindAddress);
        if (result != NSAPI_ERROR_OK) {
            Serial.print("[ERROR] Не удалось открыть HTTP-порт 80 на ");
            Serial.print(bindAddress);
            Serial.print("; код Mbed: ");
            Serial.println(result);
            return false;
        }
        settingsServerStarted_ = true;
        Serial.print("[WEB] HTTP-сервер слушает ");
        Serial.print(bindAddress);
        Serial.println(":80.");
    }
    if (!wifiMdns_.started()) {
        if (!wifiMdns_.begin()) {
            wifiMdns_.stop();
            Serial.println("[ERROR] Не удалось включить mDNS UDP multicast; HTTP доступен по IP.");
            return false;
        }
    }
    Serial.println("[WEB] Настройки: http://opta-settings.local");
    return true;
}

void NetworkManager::startWiredWebServer() {
    if (!wiredServerStarted_) {
        IPAddress address = Ethernet.localIP();
        nsapi_error_t result = wiredServer_.beginListening();
        if (result != NSAPI_ERROR_OK) {
            Serial.print("[ERROR] Не удалось открыть HTTP-порт 80 на Ethernet ");
            Serial.print(address);
            Serial.print("; этап: ");
            Serial.print(wiredServer_.lastErrorStage());
            Serial.print(", код Mbed: ");
            Serial.println(result);
            wiredServerRetryPending_ = true;
            wiredServerFailedMillis_ = millis();
            return;
        }
        wiredServerStarted_ = true;
        wiredServerRetryPending_ = false;
        wiredServerAddress_ = address;
        Serial.print("[WEB] HTTP-сервер Ethernet слушает ");
        Serial.print(address);
        Serial.println(":80.");
        Serial.print("[Ethernet] IP-адрес: ");
        Serial.println(address);
        if (!wiredMdns_.started()) {
            if (wiredMdns_.begin()) {
                Serial.println("[mDNS] opta-settings.local доступен через Ethernet.");
            } else {
                Serial.println("[WARN] Ethernet HTTP работает по IP; запуск mDNS не удался.");
            }
        }
    }
}

void NetworkManager::serviceWiredNetwork(bool forceAttempt) {
    IPAddress address = Ethernet.localIP();
    if (wiredServerStarted_ &&
        (address[0] == 0 || address != wiredServerAddress_)) {
        wiredServer_.end();
        wiredServerStarted_ = false;
        wiredServerAddress_ = IPAddress();
        if (wiredMdns_.started()) {
            wiredMdns_.stop();
        }
        Serial.println("[Ethernet] IP изменился или потерян; перезапуск HTTP-сервера.");
    }
    if (address[0] != 0) {
        if (!wiredServerStarted_ &&
            (!wiredServerRetryPending_ ||
             millis() - wiredServerFailedMillis_ >= WIRED_SERVER_RETRY_INTERVAL_MS)) {
            startWiredWebServer();
        }
        return;
    }

    unsigned long now = millis();
    if (!forceAttempt && now - lastEthernetAttemptMillis_ < ETHERNET_RETRY_INTERVAL_MS) {
        return;
    }
    lastEthernetAttemptMillis_ = now;

    if (Ethernet.linkStatus() == LinkON || forceAttempt) {
        kickWatchdog();
        if (wiredDhcpAttempted_) {
            Ethernet.disconnect();
        }
        wiredDhcpAttempted_ = true;
        // Ядро mbed запускает DHCP в фоне и только ждет адрес до таймаута. Таймаут 0 —
        // не ждать (KI-006): адрес подхватит этот же опрос, сервер запустится сам
        Ethernet.begin(nullptr, 0UL, 0UL);
        Serial.println("[Ethernet] DHCP запущен, адрес ожидается в фоне.");
    } else {
        // Кабель Ethernet не подключен (LinkOFF / LinkUnknown)
        wiredDhcpAttempted_ = false;
    }
}

void NetworkManager::serviceMDNS() {
    if (wifiMdns_.started()) {
        IPAddress address = state_ == WifiState::Portal ? CONFIG_AP_IP : WiFi.localIP();
        wifiMdns_.service(address);
    }
}

void NetworkManager::serviceWifiManager() {
    serviceWiredNetwork();
    captiveDns_.service(CONFIG_AP_IP);
    serviceMDNS();
    serviceSettingsWebServer();
    serviceWiredWebServer();
}

void NetworkManager::serviceSettingsWebServer() {
    arduino::WiFiClient client = portalServer_.accept();
    if (client) {
        Serial.println("[WEB] Принято HTTP-соединение.");
        lastPortalActivityMillis_ = millis();
        handler_->handleClient(client);
    }
}

void NetworkManager::serviceWiredWebServer() {
    if (wiredMdns_.started()) {
        wiredMdns_.service(Ethernet.localIP());
    }
    if (!wiredServerStarted_) {
        return;
    }
    arduino::EthernetClient client = wiredServer_.accept();
    if (client) {
        Serial.println("[WEB] Принято Ethernet HTTP-соединение.");
        lastPortalActivityMillis_ = millis();
        handler_->handleClient(client);
    }
}
