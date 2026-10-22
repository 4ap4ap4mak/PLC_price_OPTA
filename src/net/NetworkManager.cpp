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
// Новая сеть со страницы: короче, чем после запуска (модуль уже работает), — при
// ошибке в пароле точка настройки возвращается примерно через минуту (D-031)
static const unsigned long NEW_CREDENTIALS_RETRY_DELAYS_MS[] = {5000UL, 15000UL};
static const uint8_t NEW_CREDENTIALS_ATTEMPTS =
    sizeof(NEW_CREDENTIALS_RETRY_DELAYS_MS) / sizeof(NEW_CREDENTIALS_RETRY_DELAYS_MS[0]) + 1;
// Ответ на сохранение успевает уйти в браузер до остановки точки настройки
static const unsigned long NEW_CREDENTIALS_DELAY_MS = 1500UL;
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
// Повтор DHCP Ethernet без адреса; повтор запуска HTTP-сервера и mDNS после ошибки
static const unsigned long ETHERNET_RETRY_INTERVAL_MS = 15000UL;
static const unsigned long HTTP_SERVER_RETRY_INTERVAL_MS = 5000UL;
static const unsigned long MDNS_RETRY_INTERVAL_MS = 5000UL;

NetworkManager::NetworkManager(configstore::WifiCredentialStore& credentials)
    : credentials_(credentials),
      captiveDns_(dnsUdp_),
      mdns_(mdnsUdp_, DEVICE_HOSTNAME) {}

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
    if (wiredReady_ || state_ == WifiState::Online || state_ == WifiState::Portal) {
        ensureHttpServer(false);
    }
    serviceMdns(nowMs);
    if (state_ == WifiState::Portal) {
        captiveDns_.service(CONFIG_AP_IP);
    }
    serviceHttpClients();
    if (state_ == WifiState::Portal) {
        servicePortalReconnect();
    }
    return uplink() != Uplink::None;
}

Uplink NetworkManager::uplink() const {
    if (state_ == WifiState::Online) return Uplink::WiFi;
    if (wiredReady_) return Uplink::Ethernet;
    return Uplink::None;
}

// Шаг автомата Wi-Fi: итог попытки, команда пользователя, конец паузы, потеря связи.
void NetworkManager::serviceWifi(uint32_t nowMs) {
    if (state_ == WifiState::Connecting) {
        const WifiConnector::Result result = connector_.poll();
        if (result == WifiConnector::Result::Busy) return;
        if (pending_ == Request::None) {
            // Idle на месте итога — итог потерян: считаем неудачей, иначе автомат встанет
            finishAttempt(result == WifiConnector::Result::Connected);
            return;
        }
        // Команда пользователя пришла во время попытки: итог попытки уже не нужен
        state_ = WifiState::Off;
    }
    if (pending_ != Request::None && servicePendingRequest(nowMs)) return;
    switch (state_) {
        case WifiState::Connecting:
            return;
        case WifiState::Waiting:
            if (nowMs - waitStartMillis_ >= waitMillis_ ||
                (wiredEndsWait_ && wiredReady_)) {
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

void NetworkManager::applyNewCredentials() {
    pending_ = Request::NewCredentials;
    pendingSinceMillis_ = millis();
}

// Команда пользователя, отложенная до конца попытки подключения (WiFi занят потоком).
// true — команда выполнена, шаг автомата закончен.
bool NetworkManager::servicePendingRequest(uint32_t nowMs) {
    switch (pending_) {
        case Request::Portal:
            pending_ = Request::None;
            if (!startWifiManager()) {
                Serial.println("[ERROR] Не удалось запустить точку настройки Wi-Fi по кнопке USER.");
                startSeries(Series::Reconnect);
                waitThen(RECONNECT_INTERVAL_MS, false);
            }
            return true;
        case Request::NewCredentials:
            // Ответ страницы должен успеть уйти до остановки точки настройки; пока ждем,
            // новых попыток со старыми шагами автомата нет
            if (nowMs - pendingSinceMillis_ < NEW_CREDENTIALS_DELAY_MS) return true;
            pending_ = Request::None;
            switchToNewCredentials();
            return true;
        case Request::None:
            break;
    }
    return false;
}

// Вызывается, только когда попытка подключения не идет: WiFi не занят потоком.
void NetworkManager::switchToNewCredentials() {
    Serial.println("\n[WiFi] Сохранена новая сеть; подключение без перезагрузки...");
    if (state_ == WifiState::Portal) {
        stopWifiManager();
    } else {
        // mDNS на Wi-Fi отвечает прежним адресом: после подключения он запустится
        // с новым. HTTP-сервер слушает все адреса и не трогается (D-033)
        stopWifiMdns();
        resetStation();
    }
    kickWatchdog();
    startSeries(Series::NewCredentials);
    startAttempt();
}

const NetworkManager::RetryPlan* NetworkManager::retryPlan() const {
    static const RetryPlan kBoot = {BOOT_RETRY_DELAYS_MS, BOOT_ATTEMPTS};
    static const RetryPlan kNewCredentials = {
        NEW_CREDENTIALS_RETRY_DELAYS_MS, NEW_CREDENTIALS_ATTEMPTS};
    switch (series_) {
        case Series::Boot: return &kBoot;
        case Series::NewCredentials: return &kNewCredentials;
        case Series::Reconnect:
        case Series::PortalReturn: break;
    }
    return nullptr;
}

// Сброс прежнего подключения станции. INADDR_NONE — на случай, если до этого не
// запустилась точка доступа со статическим IP.
void NetworkManager::resetStation() {
    WiFi.disconnect();
    WiFi.config(INADDR_NONE);
}

// mDNS на Wi-Fi закрывается до смены режима Wi-Fi: подписка на группу привязана к
// поднятым интерфейсам. С Ethernet mDNS отвечает его адресом и не трогается (D-033).
void NetworkManager::stopWifiMdns() {
    if (!wiredReady_) {
        stopMdns();
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
    // Серия Reconnect при Ethernet идет бесконечно: счетчик не переполняется
    if (attempts_ < UINT8_MAX) ++attempts_;
    const RetryPlan* plan = retryPlan();
    if (plan != nullptr) {
        Serial.print("[WiFi] Подключение к сохраненной сети: \"");
        Serial.print(credentials.ssid);
        if (attempts_ > 1) {
            Serial.print("\", попытка ");
            Serial.print(attempts_);
            Serial.print(" из ");
            Serial.print(plan->attempts);
            Serial.println("...");
        } else {
            Serial.println("\"...");
        }
    }
    // Сброс прежней попытки. Первую попытку после запуска, возврат из точки настройки
    // и первую попытку новой сети (сброс уже сделан) не трогаем.
    if (series_ == Series::Reconnect || (plan != nullptr && attempts_ > 1)) {
        resetStation();
    }
    WiFi.setHostname(DEVICE_HOSTNAME);
    // false не бывает: новая попытка начинается только после итога прежней, а итог
    // прежней тогда придет в serviceWifi()
    connector_.start(credentials.ssid, credentials.password);
    state_ = WifiState::Connecting;
}

void NetworkManager::finishAttempt(bool connected) {
    if (connected) {
        onConnected();
    } else {
        onAttemptFailed(connector_.lastDurationMs());
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
            break;
        case Series::Reconnect:
            Serial.print("[WiFi] Подключено снова. IP-адрес: ");
            Serial.println(WiFi.localIP());
            break;
        case Series::PortalReturn:
            wifiCredentialsVisible_ = false;
            Serial.print("[WiFi] Подключено, точка настройки закрыта. IP-адрес: ");
            Serial.println(WiFi.localIP());
            break;
        case Series::NewCredentials:
            wifiCredentialsVisible_ = false;
            Serial.print("[WiFi] Подключено к новой сети. IP-адрес: ");
            Serial.println(WiFi.localIP());
            break;
    }
    // Сервер один на все интерфейсы: страница доступна и по Wi-Fi при работающем
    // Ethernet (D-033, KI-009)
    if (ensureHttpServer(true)) {
        Serial.print("[WEB] Настройки по Wi-Fi: http://");
        Serial.println(WiFi.localIP());
    }
}

void NetworkManager::onAttemptFailed(uint32_t durationMs) {
    switch (series_) {
        case Series::Boot:
        case Series::NewCredentials: {
            const RetryPlan& plan = *retryPlan();
            if (attempts_ < plan.attempts) {
                const unsigned long waitMs = plan.delaysMs[attempts_ - 1];
                logLine("WiFi", "Попытка %u из %u не удалась (%lu мс); следующая через %lu с.",
                    static_cast<unsigned>(attempts_), static_cast<unsigned>(plan.attempts),
                    static_cast<unsigned long>(durationMs),
                    waitMs / 1000UL);
                waitThen(waitMs, false);
            } else {
                logLine("WiFi", "Попытка %u из %u не удалась (%lu мс).",
                    static_cast<unsigned>(attempts_), static_cast<unsigned>(plan.attempts),
                    static_cast<unsigned long>(durationMs));
                if (series_ == Series::NewCredentials) {
                    Serial.println("[WiFi] К новой сети подключиться не удалось; проверьте имя и пароль.");
                }
                openSettingsAccess(series_ == Series::Boot);
            }
            return;
        }
        case Series::Reconnect:
            logLine("WiFi", "Повторное подключение не удалось (попытка %u, %lu мс).",
                static_cast<unsigned>(attempts_), static_cast<unsigned long>(durationMs));
            if (attempts_ >= RECONNECT_ATTEMPTS_BEFORE_PORTAL && !wiredReady_) {
                openSettingsAccess(false);
            } else {
                waitThen(RECONNECT_INTERVAL_MS, false);
            }
            return;
        case Series::PortalReturn:
            Serial.println("[WiFi] Сохраненная сеть недоступна; точка настройки запускается снова.");
            if (!startWifiManager()) {
                Serial.println("[ERROR] Не удалось повторно запустить точку настройки.");
                startSeries(Series::Reconnect);
                waitThen(RECONNECT_INTERVAL_MS, false);
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
    if (wiredReady_) {
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
    waitThen(RECONNECT_INTERVAL_MS, false);
}

void NetworkManager::requestPortal() {
    if (wiredReady_) {
        wifiCredentialsVisible_ = true;
        Serial.println("\n[WiFi] Настройки Wi-Fi доступны через Ethernet; точка доступа не запускается.");
        return;
    }
    Serial.println("\n[WiFi] USER удерживается 5 секунд; запуск режима настройки Wi-Fi.");
    if (state_ == WifiState::Connecting) {
        // WiFi занят потоком подключения: точку запустит servicePendingRequest()
        pending_ = Request::Portal;
        Serial.println("[WiFi] Точка настройки запустится после текущей попытки подключения.");
        return;
    }
    if (!startWifiManager()) {
        Serial.println("[ERROR] Не удалось запустить точку настройки Wi-Fi по кнопке USER.");
    }
}

// Вызывается, только когда попытка подключения не идет: WiFi не занят потоком.
// false — точка не запущена (состояние автомата прежнее); true — точка работает.
bool NetworkManager::startWifiManager() {
    Serial.println("[WiFi] Запуск точки настройки Opta-Config-AP...");
    captiveDns_.stop();
    stopWifiMdns();
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
    // Сервер один на все интерфейсы и повторяется в service() раз в 5 с: его ошибка
    // не отменяет уже запущенную точку
    if (!ensureHttpServer(true)) {
        Serial.println("[ERROR] Не удалось запустить HTTP-сервер точки настройки; повтор через 5 с.");
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
    captiveDns_.stop();
    stopWifiMdns();
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

bool NetworkManager::ensureHttpServer(bool force) {
    if (httpServerStarted_) return true;
    const unsigned long now = millis();
    if (!force && httpServerRetryPending_ &&
        now - httpServerFailedMillis_ < HTTP_SERVER_RETRY_INTERVAL_MS) {
        return false;
    }
    // Стек общий: сокет слушает и Wi-Fi, и точку настройки, и Ethernet
    NetworkInterface* network = Ethernet.getNetwork();
    if (network == nullptr) {
        network = WiFi.getNetwork();
    }
    const nsapi_error_t result = network != nullptr
        ? httpServer_.beginAnyAddress(network)
        : NSAPI_ERROR_NO_CONNECTION;
    if (result != NSAPI_ERROR_OK) {
        Serial.print("[ERROR] Не удалось открыть HTTP-порт 80; этап: ");
        Serial.print(network != nullptr ? httpServer_.lastErrorStage() : "network");
        Serial.print(", код Mbed: ");
        Serial.println(result);
        httpServerRetryPending_ = true;
        httpServerFailedMillis_ = now;
        return false;
    }
    httpServerStarted_ = true;
    httpServerRetryPending_ = false;
    Serial.println("[WEB] HTTP-сервер слушает порт 80 на всех интерфейсах.");
    return true;
}

void NetworkManager::serviceWiredNetwork(bool forceAttempt) {
    IPAddress address = Ethernet.localIP();
    if (wiredReady_ && (address[0] == 0 || address != wiredAddress_)) {
        wiredReady_ = false;
        wiredAddress_ = IPAddress();
        Serial.println("[Ethernet] IP изменился или потерян.");
    }
    if (address[0] != 0) {
        if (!wiredReady_) {
            wiredReady_ = true;
            wiredAddress_ = address;
            Serial.print("[Ethernet] IP-адрес: ");
            Serial.println(address);
            if (ensureHttpServer(true)) {
                Serial.print("[WEB] Настройки по Ethernet: http://");
                Serial.println(address);
            }
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

// Лимит ядра mbed Opta на все устройство (MBED_CONF_LWIP_SOCKET_MAX)
static const uint8_t CORE_SOCKET_LIMIT = 4;
// Больше лимита: если ядро позволит открыть все, это тоже станет видно
static const uint8_t SOCKET_PROBE_MAX = 8;

static void printSocketUser(const char* name, bool open, uint8_t& count) {
    Serial.print("[NET]   ");
    Serial.print(name);
    Serial.println(open ? ": занят" : ": нет");
    if (open) ++count;
}

void NetworkManager::printSocketReport(bool fetchInProgress) {
    if (state_ == WifiState::Connecting || fetchInProgress) {
        Serial.println("[NET] Идет подключение Wi-Fi или запрос цены; проверка сокетов не выполнена, повторите через минуту.");
        return;
    }
    NetworkInterface* network = WiFi.getNetwork();
    if (network == nullptr) {
        network = Ethernet.getNetwork();
    }
    if (network == nullptr) {
        Serial.println("[NET] Сетевой интерфейс недоступен; проверка сокетов не выполнена.");
        return;
    }

    UDPSocket probes[SOCKET_PROBE_MAX];
    uint8_t freeCount = 0;
    while (freeCount < SOCKET_PROBE_MAX && probes[freeCount].open(network) == NSAPI_ERROR_OK) {
        ++freeCount;
    }
    for (uint8_t i = 0; i < freeCount; ++i) {
        probes[i].close();
    }

    Serial.println("\n[NET] Сокеты сетевого стека:");
    uint8_t used = 0;
    printSocketUser("HTTP-сервер (все интерфейсы)", httpServerStarted_, used);
    printSocketUser("mDNS", mdns_.started(), used);
    printSocketUser("DNS точки настройки", captiveDns_.started(), used);
    logLine("NET", "Занято прошивкой: %u, свободно (проверено открытием): %u, лимит ядра: %u.",
        static_cast<unsigned>(used), static_cast<unsigned>(freeCount),
        static_cast<unsigned>(CORE_SOCKET_LIMIT));
    if (used + freeCount != CORE_SOCKET_LIMIT) {
        Serial.println("[NET] Сумма не равна лимиту: лимит другой или сокет занят чем-то еще.");
    }
}

void NetworkManager::serviceMdns(uint32_t nowMs) {
    IPAddress address;
    const char* interfaceName = "";
    if (wiredReady_) {
        address = wiredAddress_;
        interfaceName = "Ethernet";
    } else if (state_ == WifiState::Portal) {
        address = CONFIG_AP_IP;
        interfaceName = "точка настройки";
    } else if (state_ == WifiState::Online) {
        address = WiFi.localIP();
        interfaceName = "Wi-Fi";
    }
    if (mdns_.started() && address != mdnsAddress_) {
        stopMdns();
    }
    if (address[0] == 0) {
        return;
    }
    if (!mdns_.started()) {
        if (mdnsRetryPending_ && nowMs - mdnsFailedMillis_ < MDNS_RETRY_INTERVAL_MS) {
            return;
        }
        // Подписка на группу — на интерфейсах, поднятых сейчас: поэтому перезапуск
        // при каждой смене адреса
        if (!mdns_.begin(interfaceName)) {
            mdns_.stop();
            mdnsRetryPending_ = true;
            mdnsFailedMillis_ = nowMs;
            Serial.println("[ERROR] Не удалось включить mDNS UDP multicast; HTTP доступен по IP.");
            return;
        }
        mdnsRetryPending_ = false;
        mdnsAddress_ = address;
        Serial.print("[mDNS] opta-settings.local отвечает адресом ");
        Serial.print(address);
        Serial.print(" (");
        Serial.print(interfaceName);
        Serial.println(").");
    }
    mdns_.service(address);
}

void NetworkManager::stopMdns() {
    if (mdns_.started()) {
        mdns_.stop();
    }
    mdnsAddress_ = IPAddress();
}

void NetworkManager::serviceHttpClients() {
    if (!httpServerStarted_) {
        return;
    }
    arduino::WiFiClient client = httpServer_.accept();
    if (client) {
        Serial.print("[WEB] Принято HTTP-соединение от ");
        Serial.println(client.remoteIP());
        lastPortalActivityMillis_ = millis();
        handler_->handleClient(client);
    }
}
