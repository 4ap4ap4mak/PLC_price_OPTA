#include "NetworkManager.h"

#include <string.h>

#include "../platform/Watchdog.h"

using configstore::WifiCredentials;

// В режиме точки настройки повторять подключение к сохраненной сети
static const unsigned long PORTAL_RECONNECT_INTERVAL_MS = 5UL * 60UL * 1000UL;
// Не прерывать точку настройки, пока пользователь недавно открывал страницу
static const unsigned long PORTAL_ACTIVITY_GRACE_MS = 2UL * 60UL * 1000UL;
static const char* CONFIG_AP_SSID = "Opta-Config-AP";
static const char* CONFIG_AP_PASSWORD = "opta1234";
static const char* DEVICE_HOSTNAME = "opta-settings";
static const IPAddress CONFIG_AP_IP(192, 168, 4, 1);

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

bool NetworkManager::begin(HttpClientHandler& handler) {
    handler_ = &handler;
    bool wifiOnline = false;

    Serial.println("[Ethernet] Проверка проводного подключения...");
    configureEthernetHostname();
    serviceWiredNetwork(true);

    if (WiFi.status() == WL_NO_SHIELD) {
        Serial.println("[ERROR] Модуль Wi-Fi не обнаружен на плате!");
    } else {
        WifiCredentials credentials = {};
        bool credentialsValid = credentials_.load(credentials);
        if (credentialsValid && strlen(credentials.ssid) > 0) {
            Serial.print("[WiFi] Подключение к сохраненной сети: \"");
            Serial.print(credentials.ssid);
            Serial.println("\"...");
            WiFi.setHostname(DEVICE_HOSTNAME);
            WiFi.begin(credentials.ssid, credentials.password);

            int attempts = 0;
            const int maxAttempts = 20;
            while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
                kickWatchdog();
                delay(500);
                Serial.print(".");
                attempts++;
            }
        } else {
            Serial.println("[WiFi] Сохраненные учетные данные не найдены или повреждены.");
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n[WiFi] Успешно подключено к Wi-Fi!");
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
            wifiOnline = true;
        } else {
            Serial.println("\n[WiFi] Переход в режим конфигурации...");
            serviceWiredNetwork();
            if (wiredServerStarted_) {
                wifiCredentialsVisible_ = true;
                Serial.print("[WEB] Точка доступа не запускается: настройки доступны по LAN адресу http://");
                Serial.println(Ethernet.localIP());
            } else if (startWifiManager()) {
                Serial.println("[WiFi] Открыт портал настройки.");
            }
        }
    }
    return wifiOnline;
}

bool NetworkManager::service(uint32_t nowMs) {
    serviceWiredNetwork();
    // Автоматическое переподключение к Wi-Fi при потере связи
    if (portalMode_) {
        serviceWifiManager();
        servicePortalReconnect();
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) {
        serviceMDNS();
        serviceSettingsWebServer();
    }
    serviceWiredWebServer();

    if (WiFi.status() != WL_CONNECTED && WiFi.status() != WL_NO_SHIELD) {
        if (nowMs - lastReconnectAttempt_ > 10000) {
            lastReconnectAttempt_ = nowMs;
            reconnectAttempts_++;
            Serial.println("[WiFi] Соединение потеряно, повторное подключение...");
            WiFi.disconnect();
            WifiCredentials credentials = {};
            if (credentials_.load(credentials)) {
                // На случай, если до этого не запустилась точка доступа со статическим IP
                WiFi.config(INADDR_NONE);
                WiFi.setHostname(DEVICE_HOSTNAME);
                WiFi.begin(credentials.ssid, credentials.password);
            } else if (!wiredServerStarted_) {
                startWifiManager();
            } else {
                wifiCredentialsVisible_ = true;
            }
            if (reconnectAttempts_ >= 3 && WiFi.status() != WL_CONNECTED && !wiredServerStarted_) {
                startWifiManager();
            }
        }
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) {
        reconnectAttempts_ = 0;
    }
    return true;
}

void NetworkManager::requestPortal() {
    if (wiredServerStarted_) {
        wifiCredentialsVisible_ = true;
        Serial.println("\n[WiFi] Настройки Wi-Fi доступны через Ethernet; точка доступа не запускается.");
    } else {
        Serial.println("\n[WiFi] USER удерживается 5 секунд; запуск режима настройки Wi-Fi.");
    }
    if (!wiredServerStarted_ && !startWifiManager()) {
        Serial.println("[ERROR] Не удалось запустить точку настройки Wi-Fi по кнопке USER.");
    }
}

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
    portalMode_ = true;
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
    portalMode_ = false;
}

// Точка настройки не должна оставаться навсегда после временного сбоя роутера:
// периодически пробуем вернуться к сохраненной сети.
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
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.begin(credentials.ssid, credentials.password);
    for (int attempts = 0; WiFi.status() != WL_CONNECTED && attempts < 20; ++attempts) {
        kickWatchdog();
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiCredentialsVisible_ = false;
        Serial.print("[WiFi] Подключено, точка настройки закрыта. IP-адрес: ");
        Serial.println(WiFi.localIP());
        if (!wiredServerStarted_) {
            startSettingsWebServer();
        }
        return;
    }

    Serial.println("[WiFi] Сохраненная сеть недоступна; точка настройки запускается снова.");
    if (!startWifiManager()) {
        Serial.println("[ERROR] Не удалось повторно запустить точку настройки.");
    }
}

bool NetworkManager::startSettingsWebServer() {
    if (!settingsServerStarted_) {
        IPAddress bindAddress = portalMode_ ? CONFIG_AP_IP : WiFi.localIP();
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
            nextWiredServerAttemptMillis_ = millis() + 5000UL;
            return;
        }
        wiredServerStarted_ = true;
        nextWiredServerAttemptMillis_ = 0;
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

// Сравнения с дедлайнами millis() ниже ломаются около переполнения (KI-012).
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
            (nextWiredServerAttemptMillis_ == 0 ||
             millis() >= nextWiredServerAttemptMillis_)) {
            startWiredWebServer();
        }
        return;
    }

    unsigned long now = millis();
    if (!forceAttempt && now < nextEthernetAttemptMillis_) {
        return;
    }
    nextEthernetAttemptMillis_ = now + 15000UL;

    if (Ethernet.linkStatus() == LinkON || forceAttempt) {
        kickWatchdog();
        if (wiredDhcpAttempted_) {
            Ethernet.disconnect();
        }
        wiredDhcpAttempted_ = true;
        int connected = Ethernet.begin(nullptr, forceAttempt ? 3000UL : 1000UL, 1000UL);
        kickWatchdog();
        Serial.print("[Ethernet] DHCP/link init result: ");
        Serial.print(connected);
        Serial.print(", IP: ");
        Serial.println(Ethernet.localIP());
    } else {
        // Кабель Ethernet не подключен (LinkOFF / LinkUnknown)
        wiredDhcpAttempted_ = false;
    }
}

void NetworkManager::serviceMDNS() {
    if (wifiMdns_.started()) {
        IPAddress address = portalMode_ ? CONFIG_AP_IP : WiFi.localIP();
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
