#include <Arduino.h>
#include <ArduinoJson.h>
#include <drivers/Watchdog.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <kvstore_global_api.h>
#include <Ethernet.h>

// =============================================================================
// КОНФИГУРАЦИЯ СЕТИ
// =============================================================================
// Активировано подключение по Wi-Fi для Arduino Opta WiFi
#define USE_WIFI

#ifdef USE_WIFI
  #include <WiFi.h>
  #include <WiFiSSLClient.h>
  #include <WiFiServer.h>
  #include <WiFiUdp.h>
  arduino::WiFiSSLClient client;
  arduino::WiFiUDP ntpUdp;
  arduino::WiFiUDP dnsUdp;
  arduino::WiFiUDP mdnsUdp;
  arduino::EthernetUDP wiredMdnsUdp;
#else
  #include <Portenta_Ethernet.h>
  #include <EthernetSSLClient.h>
  #include <EthernetUdp.h>
  arduino::EthernetSSLClient client;
  arduino::EthernetUDP ntpUdp;
#endif

// =============================================================================
// НАСТРОЙКИ СЕРВЕРА И ЛОГИКИ РЕЛЕ
// =============================================================================
const char* serverHost = "dashboard.elering.ee";
const int   serverPort = 443;
const char* apiPath    = "/api/nps/price/lv/current";

const unsigned long PRICE_RETRY_INTERVAL_MS = 30UL * 1000UL;
const unsigned long TIME_SYNC_RETRY_INTERVAL_MS = 60UL * 1000UL;
const unsigned long TIME_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
const uint32_t SECONDS_PER_PRICE_INTERVAL = 15UL * 60UL;
// Без новой цены дольше этого времени автоматические реле выключаются
const unsigned long PRICE_FAILSAFE_TIMEOUT_MS = 60UL * 60UL * 1000UL;
// Цена из API принимается, только если ее timestamp не старше часа
const uint32_t PRICE_MAX_AGE_SECONDS = 60UL * 60UL;
// В режиме точки настройки повторять подключение к сохраненной сети
const unsigned long PORTAL_RECONNECT_INTERVAL_MS = 5UL * 60UL * 1000UL;
// Не прерывать точку настройки, пока пользователь недавно открывал страницу
const unsigned long PORTAL_ACTIVITY_GRACE_MS = 2UL * 60UL * 1000UL;
const unsigned long HTTP_REQUEST_TIMEOUT_MS = 3000UL;
const size_t HTTP_MAX_LINE_LENGTH = 512;
const uint8_t HTTP_MAX_HEADER_COUNT = 32;
unsigned long lastUpdateMillis = 0;
bool hasAttemptedPriceUpdate = false;
// true с самого старта: первая цена запрашивается сразу после синхронизации времени
bool awaitingNewPriceTimestamp = true;
uint32_t synchronizedEpochSeconds = 0;
unsigned long epochSyncMillis = 0;
unsigned long lastTimeSyncAttemptMillis = 0;
bool hasAttemptedTimeSync = false;
bool timeSynchronized = false;
uint32_t lastScheduledPriceInterval = 0;
bool hasScheduledPriceInterval = false;
bool ntpUdpStarted = false;
bool dnsUdpStarted = false;
bool portalMode = false;
unsigned long lastPortalReconnectMillis = 0;
unsigned long lastPortalActivityMillis = 0;
bool wifiCredentialsVisible = false;
const char* CONFIG_AP_SSID = "Opta-Config-AP";
const char* CONFIG_AP_PASSWORD = "opta1234";
const char* WIFI_CREDENTIALS_KEY = "/kv/wifi";
const char* DEVICE_CONFIG_KEY = "/kv/device_config";
const uint16_t DEVICE_CONFIG_VERSION = 5;
const uint8_t RELAY_COUNT = 4;
const char* DEVICE_HOSTNAME = "opta-settings";
const IPAddress MDNS_MULTICAST_IP(224, 0, 0, 251);
const IPAddress CONFIG_AP_IP(192, 168, 4, 1);

struct WifiCredentials {
    char ssid[33];
    char password[64];
};

struct DeviceConfig {
    uint16_t version;
    float safetyCeilingEurKwh;
    float alwaysOnBelowPriceEurKwh;
    bool nightPrepEnabled;
    uint16_t nightStartMinuteOfDay;
    uint16_t nightEndMinuteOfDay;
    uint8_t nightCheapHours;
    bool consecutiveBlockEnabled;
    uint8_t consecutiveBlockHours;
    bool holidayEnabled;
    uint32_t holidayReturnEpoch;
    uint8_t holidayHeatHours;
    bool smartThresholdEnabled;
    uint8_t smartThresholdHours;
    bool wakeupEnabled;
    uint8_t wakeupHours;
    uint16_t wakeupTimeMinuteOfDay;
};

struct LegacyDeviceConfig {
    uint16_t version;
    float safetyCeilingEurKwh;
    float alwaysOnBelowPriceEurKwh;
    bool nightPrepEnabled;
    uint16_t nightStartMinuteOfDay;
    uint16_t nightEndMinuteOfDay;
    uint8_t nightCheapHours;
    bool consecutiveBlockEnabled;
    uint8_t consecutiveBlockHours;
    bool holidayEnabled;
    uint32_t holidayReturnEpoch;
    uint8_t holidayHeatHours;
};

struct LegacyRelayConfigs {
    LegacyDeviceConfig configs[RELAY_COUNT];
};

struct ForecastSlot {
    uint32_t timestamp;
    float priceEurKwh;
};

const size_t MAX_FORECAST_SLOTS = 200;
DeviceConfig relayConfigs[RELAY_COUNT] = {};
uint8_t selectedRelayIndex = 0;
uint8_t selectedChartDay = 0;
bool relayStates[RELAY_COUNT] = {};
bool manualOverrides[RELAY_COUNT] = {};
ForecastSlot forecastSlots[MAX_FORECAST_SLOTS];
size_t forecastSlotCount = 0;
bool settingsServerStarted = false;
bool mdnsUdpStarted = false;
bool wiredServerStarted = false;
bool wiredMdnsStarted = false;
bool wiredDhcpAttempted = false;
unsigned long nextEthernetAttemptMillis = 0;
unsigned long nextWiredServerAttemptMillis = 0;
IPAddress wiredServerAddress;
uint32_t lockedConsecutiveBlockStart[RELAY_COUNT] = {};
uint32_t lockedConsecutiveBlockEnd[RELAY_COUNT] = {};
// Интервал краткого вывода статуса в монитор (раз в 10 секунд)
const unsigned long HEARTBEAT_INTERVAL_MS = 10000UL;
inline void kickWatchdog() {
    mbed::Watchdog::get_instance().kick();
}

unsigned long lastHeartbeatMillis = 0;

class CheckedWiFiServer : public arduino::WiFiServer {
public:
    using arduino::WiFiServer::WiFiServer;

    void end() {
        if (sock) {
            sock->close();
            delete sock;
            sock = nullptr;
        }
    }

    nsapi_error_t beginOnAddress(const IPAddress& ipAddress) {
        end();
        sock = new TCPSocket();
        if (sock == nullptr) {
            return NSAPI_ERROR_NO_MEMORY;
        }

        nsapi_error_t result = sock->open(WiFi.getNetwork());
        if (result != NSAPI_ERROR_OK) {
            end();
            return result;
        }

        int reuseAddress = 1;
        sock->setsockopt(NSAPI_SOCKET, NSAPI_REUSEADDR,
            &reuseAddress, sizeof(reuseAddress));
        String addressText = ipAddress.toString();
        SocketAddress address(addressText.c_str(), _port);
        result = sock->bind(address);
        if (result == NSAPI_ERROR_OK) {
            result = sock->listen(5);
        }
        if (result == NSAPI_ERROR_OK) {
            sock->set_blocking(false);
        } else {
            end();
        }
        return result;
    }
};

class CheckedEthernetServer : public arduino::EthernetServer {
public:
    using arduino::EthernetServer::EthernetServer;

    void end() {
        if (sock) {
            sock->close();
            delete sock;
            sock = nullptr;
        }
    }

    nsapi_error_t beginListening() {
        end();
        sock = new TCPSocket();
        if (sock == nullptr) {
            errorStage = "allocate";
            return NSAPI_ERROR_NO_MEMORY;
        }

        nsapi_error_t result = sock->open(Ethernet.getNetwork());
        if (result != NSAPI_ERROR_OK) {
            errorStage = "open";
            end();
            return result;
        }

        int reuseAddress = 1;
        sock->setsockopt(NSAPI_SOCKET, NSAPI_REUSEADDR,
            &reuseAddress, sizeof(reuseAddress));
        errorStage = "bind";
        result = sock->bind(_port);
        if (result == NSAPI_ERROR_OK) {
            errorStage = "listen";
            result = sock->listen(5);
        }
        if (result == NSAPI_ERROR_OK) {
            sock->set_blocking(false);
        } else {
            end();
        }
        return result;
    }

    const char* lastErrorStage() const {
        return errorStage;
    }

private:
    const char* errorStage = "not started";
};

#ifdef USE_WIFI
CheckedWiFiServer portalServer(80);
CheckedEthernetServer wiredServer(80);
#endif

const pin_size_t RELAY_PINS[RELAY_COUNT] = {D0, D1, D2, D3};
const pin_size_t RELAY_LEDS[RELAY_COUNT] = {LED_D0, LED_D1, LED_D2, LED_D3};

// Кнопка USER на корпусе Opta для ручного управления
const pin_size_t BUTTON_PIN  = BTN_USER;

// Состояние реле, режима управления и последняя цена.
// Цена может быть отрицательной, поэтому наличие данных хранится отдельным флагом.
bool  hasLatestPrice = false;
float latestPriceEurKwh   = 0.0f;
long  latestPriceTimestamp = 0;
unsigned long latestPriceReceivedMillis = 0;

// Объявления функций
void setRelay(uint8_t relayIndex, bool state);
bool fetchNordPoolPrice(float &currentPriceEurKwh, long &priceTimestamp);
bool syncNetworkTime();
uint32_t getCurrentEpochSeconds();
void servicePriceSchedule();
bool fetchPriceForecast();
bool getAutomaticRelayDecision(uint8_t relayIndex, float currentPrice, bool forecastAvailable, bool& relayOn);
void buildRelayWorkSlotsForChart(uint8_t relayIndex, uint32_t chartLocalDay,
    bool workSlots[MAX_FORECAST_SLOTS]);
template <typename Client>
void sendPriceChart(Client& client);
void applyCurrentPriceToConfiguredControl(uint8_t relayIndex);
bool loadWifiCredentials(WifiCredentials& credentials);
bool saveWifiCredentials(const WifiCredentials& credentials);
bool startWifiManager();
void serviceWifiManager();
void handleDnsRequests();
bool loadDeviceConfigs();
bool saveDeviceConfigs(const DeviceConfig* configs);
bool startSettingsWebServer();
void serviceSettingsWebServer();
void serviceWiredWebServer();
void serviceMDNS();
void startHardwareWatchdog();
void updatePriceAndRelay();
void printStatusReport();
void handleSerialCommands();
void servicePriceFailsafe();
void servicePortalReconnect();

void configureEthernetHostname() {
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

// =============================================================================
// СИНХРОНИЗАЦИЯ ЧАСОВ ПО NTP И РАСПИСАНИЕ ОПРОСА ЦЕНЫ
// =============================================================================
bool syncNetworkTime() {
    lastTimeSyncAttemptMillis = millis();
    hasAttemptedTimeSync = true;

    if (!ntpUdpStarted) {
        if (!ntpUdp.begin(2390)) {
            Serial.println("[NTP] Не удалось открыть UDP-порт для синхронизации времени.");
            return false;
        }
        ntpUdpStarted = true;
    }

    uint8_t packet[48] = {0};
    packet[0] = 0b11100011;
    packet[1] = 0;
    packet[2] = 6;
    packet[3] = 0xEC;
    packet[12] = 49;
    packet[13] = 0x4E;
    packet[14] = 49;
    packet[15] = 52;

    while (ntpUdp.parsePacket() > 0) {
        uint8_t discard[48];
        while (ntpUdp.available() > 0) {
            ntpUdp.read(discard, sizeof(discard));
        }
    }

    if (!ntpUdp.beginPacket("pool.ntp.org", 123) ||
        ntpUdp.write(packet, sizeof(packet)) != sizeof(packet) ||
        !ntpUdp.endPacket()) {
        Serial.println("[NTP] Не удалось отправить запрос серверу времени.");
        return false;
    }

    unsigned long startedAt = millis();
    while (millis() - startedAt < 1500UL) {
        int packetSize = ntpUdp.parsePacket();
        if (packetSize >= static_cast<int>(sizeof(packet))) {
            int bytesRead = ntpUdp.read(packet, sizeof(packet));
            if (bytesRead < static_cast<int>(sizeof(packet)) ||
                (packet[0] & 0x07) != 4 ||
                packet[1] == 0) {
                Serial.println("[NTP] Получен некорректный ответ сервера времени.");
                return false;
            }

            const uint32_t NTP_TO_UNIX_EPOCH_SECONDS = 2208988800UL;
            uint32_t secondsSince1900 =
                (static_cast<uint32_t>(packet[40]) << 24) |
                (static_cast<uint32_t>(packet[41]) << 16) |
                (static_cast<uint32_t>(packet[42]) << 8) |
                static_cast<uint32_t>(packet[43]);
            uint32_t epochSeconds = secondsSince1900 - NTP_TO_UNIX_EPOCH_SECONDS;
            if (epochSeconds < 1735689600UL) {
                Serial.println("[NTP] В ответе указано некорректное время.");
                return false;
            }

            synchronizedEpochSeconds = epochSeconds;
            epochSyncMillis = millis();
            timeSynchronized = true;
            Serial.print("[NTP] Время синхронизировано (UTC epoch ");
            Serial.print(epochSeconds);
            Serial.println(").");
            return true;
        }
        delay(10);
    }

    Serial.println("[NTP] Сервер времени не ответил за 1.5 секунды.");
    return false;
}

uint32_t getCurrentEpochSeconds() {
    return synchronizedEpochSeconds + (millis() - epochSyncMillis) / 1000UL;
}

#ifdef USE_WIFI
bool loadWifiCredentials(WifiCredentials& credentials) {
    size_t storedSize = 0;
    int result = kv_get(WIFI_CREDENTIALS_KEY, &credentials, sizeof(credentials), &storedSize);
    if (result != 0 || storedSize != sizeof(credentials) ||
        credentials.ssid[0] == '\0' ||
        memchr(credentials.ssid, '\0', sizeof(credentials.ssid)) == nullptr ||
        memchr(credentials.password, '\0', sizeof(credentials.password)) == nullptr) {
        return false;
    }
    return true;
}

bool saveWifiCredentials(const WifiCredentials& credentials) {
    int result = kv_set(WIFI_CREDENTIALS_KEY, &credentials, sizeof(credentials), 0);
    return result == 0;
}
#endif

bool parseTimeOfDay(const String& text, uint16_t& minuteOfDay) {
    if (text.length() != 5 || text[2] != ':') return false;
    if (!isDigit(text[0]) || !isDigit(text[1]) ||
        !isDigit(text[3]) || !isDigit(text[4])) {
        return false;
    }
    int hour = (text[0] - '0') * 10 + text[1] - '0';
    int minute = (text[3] - '0') * 10 + text[4] - '0';
    if (hour > 23 || minute > 59) return false;
    minuteOfDay = static_cast<uint16_t>(hour * 60 + minute);
    return true;
}

String formatTimeOfDay(uint16_t minuteOfDay) {
    char value[8];
    snprintf(value, sizeof(value), "%02u:%02u",
        static_cast<unsigned>(minuteOfDay / 60),
        static_cast<unsigned>(minuteOfDay % 60));
    return String(value);
}

void setDefaultDeviceConfig(DeviceConfig& config) {
    memset(&config, 0, sizeof(config));
    config.version = DEVICE_CONFIG_VERSION;
    config.safetyCeilingEurKwh = 0.3f;
    config.alwaysOnBelowPriceEurKwh = 0.0f;
    config.nightStartMinuteOfDay = 23 * 60;
    config.nightEndMinuteOfDay = 7 * 60;
    config.nightCheapHours = 4;
    config.consecutiveBlockHours = 3;
    config.holidayHeatHours = 3;
    config.smartThresholdHours = 4;
    config.wakeupHours = 3;
    config.wakeupTimeMinuteOfDay = 7 * 60;
}

void importLegacyDeviceConfig(const LegacyDeviceConfig& legacy, DeviceConfig& config) {
    config.version = legacy.version;
    config.safetyCeilingEurKwh = legacy.safetyCeilingEurKwh;
    config.alwaysOnBelowPriceEurKwh = legacy.alwaysOnBelowPriceEurKwh;
    config.nightPrepEnabled = legacy.nightPrepEnabled;
    config.nightStartMinuteOfDay = legacy.nightStartMinuteOfDay;
    config.nightEndMinuteOfDay = legacy.nightEndMinuteOfDay;
    config.nightCheapHours = legacy.nightCheapHours;
    config.consecutiveBlockEnabled = legacy.consecutiveBlockEnabled;
    config.consecutiveBlockHours = legacy.consecutiveBlockHours;
    config.holidayEnabled = legacy.holidayEnabled;
    config.holidayReturnEpoch = legacy.holidayReturnEpoch;
    config.holidayHeatHours = legacy.holidayHeatHours;
}

bool isValidDeviceConfig(const DeviceConfig& config) {
    uint16_t nightWindowMinutes = config.nightStartMinuteOfDay < config.nightEndMinuteOfDay
        ? config.nightEndMinuteOfDay - config.nightStartMinuteOfDay
        : 1440 - config.nightStartMinuteOfDay + config.nightEndMinuteOfDay;
    return config.version == DEVICE_CONFIG_VERSION &&
        isfinite(config.safetyCeilingEurKwh) &&
        isfinite(config.alwaysOnBelowPriceEurKwh) &&
        config.safetyCeilingEurKwh > config.alwaysOnBelowPriceEurKwh &&
        config.nightStartMinuteOfDay < 1440 && config.nightEndMinuteOfDay < 1440 &&
        config.nightStartMinuteOfDay != config.nightEndMinuteOfDay &&
        config.nightCheapHours > 0 && config.nightCheapHours <= 24 &&
        (!config.nightPrepEnabled || config.nightCheapHours <= nightWindowMinutes / 60) &&
        config.consecutiveBlockHours > 0 && config.consecutiveBlockHours <= 24 &&
        config.holidayHeatHours > 0 && config.holidayHeatHours <= 24 &&
        (!config.holidayEnabled || config.holidayReturnEpoch != 0) &&
        config.smartThresholdHours > 0 && config.smartThresholdHours <= 24 &&
        config.wakeupHours > 0 && config.wakeupHours <= 24 &&
        config.wakeupTimeMinuteOfDay < 1440;
}

bool saveDeviceConfigs(const DeviceConfig* configs) {
    return kv_set(DEVICE_CONFIG_KEY, configs,
        sizeof(DeviceConfig) * RELAY_COUNT, 0) == 0;
}

bool loadDeviceConfigs() {
    kv_info_t info = {};
    int result = kv_get_info(DEVICE_CONFIG_KEY, &info);
    if (result != 0) {
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
            setDefaultDeviceConfig(relayConfigs[i]);
        }
        if (!saveDeviceConfigs(relayConfigs)) {
            Serial.println("[ERROR] Не удалось сохранить настройки реле по умолчанию в KVStore.");
            return false;
        }
        Serial.print("[CONFIG] Конфигурация не найдена (KVStore: ");
        Serial.print(result);
        Serial.println("); сохранены настройки по умолчанию для всех реле.");
        return true;
    }

    bool migrated = false;
    if (info.size == sizeof(LegacyDeviceConfig)) {
        LegacyDeviceConfig legacy = {};
        size_t actualSize = 0;
        result = kv_get(DEVICE_CONFIG_KEY, &legacy, sizeof(legacy), &actualSize);
        if (result != 0 || actualSize != sizeof(legacy) ||
            (legacy.version != 2 && legacy.version != 3)) {
            Serial.println("[ERROR] Старая конфигурация реле повреждена.");
            for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
                setDefaultDeviceConfig(relayConfigs[i]);
            }
            if (!saveDeviceConfigs(relayConfigs)) {
                Serial.println("[ERROR] Не удалось заменить поврежденную конфигурацию настройками по умолчанию.");
                return false;
            }
            return true;
        }
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
            setDefaultDeviceConfig(relayConfigs[i]);
        }
        importLegacyDeviceConfig(legacy, relayConfigs[0]);
        if (legacy.version == 2) {
            relayConfigs[0].safetyCeilingEurKwh /= 1000.0f;
            relayConfigs[0].alwaysOnBelowPriceEurKwh /= 1000.0f;
        }
        relayConfigs[0].version = DEVICE_CONFIG_VERSION;
        migrated = true;
    } else if (info.size == sizeof(LegacyRelayConfigs)) {
        LegacyRelayConfigs legacyConfigs = {};
        size_t actualSize = 0;
        result = kv_get(DEVICE_CONFIG_KEY, &legacyConfigs, sizeof(legacyConfigs), &actualSize);
        if (result != 0 || actualSize != sizeof(legacyConfigs)) {
            Serial.println("[ERROR] Не удалось прочитать старые настройки четырех реле.");
            return false;
        }
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
            setDefaultDeviceConfig(relayConfigs[i]);
            importLegacyDeviceConfig(legacyConfigs.configs[i], relayConfigs[i]);
            relayConfigs[i].version = DEVICE_CONFIG_VERSION;
        }
        migrated = true;
    } else if (info.size == sizeof(relayConfigs)) {
        size_t actualSize = 0;
        result = kv_get(DEVICE_CONFIG_KEY, relayConfigs, sizeof(relayConfigs), &actualSize);
        if (result != 0 || actualSize != sizeof(relayConfigs)) {
            Serial.println("[ERROR] Не удалось прочитать конфигурации всех реле.");
            return false;
        }
    } else {
        Serial.println("[ERROR] Размер конфигурации реле не распознан; загружены настройки по умолчанию.");
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
            setDefaultDeviceConfig(relayConfigs[i]);
        }
        if (!saveDeviceConfigs(relayConfigs)) {
            Serial.println("[ERROR] Не удалось записать настройки по умолчанию для всех реле.");
            return false;
        }
        return true;
    }

    for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
        if (!isValidDeviceConfig(relayConfigs[i])) {
            Serial.print("[ERROR] Некорректная конфигурация реле ");
            Serial.println(i + 1);
            setDefaultDeviceConfig(relayConfigs[i]);
            migrated = true;
        }
    }
    if (migrated) {
        if (!saveDeviceConfigs(relayConfigs)) {
            Serial.println("[ERROR] Не удалось сохранить обновленные конфигурации реле.");
            return false;
        }
        Serial.println("[CONFIG] Конфигурации обновлены до формата для четырех реле.");
    }
    return true;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
    const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
}

void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    year = static_cast<int>(yearOfEra) + static_cast<int>(era * 400);
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    month = monthPrime < 10 ? monthPrime + 3 : monthPrime - 9;
    year += month <= 2;
}

unsigned lastSundayOfMonth(int year, unsigned month) {
    static const uint8_t daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned lastDay = daysPerMonth[month - 1];
    bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leapYear) {
        lastDay = 29;
    }
    int64_t lastDayEpoch = daysFromCivil(year, month, lastDay);
    unsigned weekday = static_cast<unsigned>((lastDayEpoch + 4) % 7);
    return lastDay - weekday;
}

bool rigaDaylightSaving(uint32_t epoch) {
    int year;
    unsigned month;
    unsigned day;
    civilFromDays(epoch / 86400UL, year, month, day);
    uint32_t dstStart = static_cast<uint32_t>(
        daysFromCivil(year, 3, lastSundayOfMonth(year, 3)) * 86400 + 3600);
    uint32_t dstEnd = static_cast<uint32_t>(
        daysFromCivil(year, 10, lastSundayOfMonth(year, 10)) * 86400 + 3600);
    return epoch >= dstStart && epoch < dstEnd;
}

bool parseRigaDateTime(const String& value, uint32_t& epoch) {
    if (value.length() != 16 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':') {
        return false;
    }
    int year = value.substring(0, 4).toInt();
    unsigned month = static_cast<unsigned>(value.substring(5, 7).toInt());
    unsigned day = static_cast<unsigned>(value.substring(8, 10).toInt());
    unsigned hour = static_cast<unsigned>(value.substring(11, 13).toInt());
    unsigned minute = static_cast<unsigned>(value.substring(14, 16).toInt());
    if (year < 2024 || year > 2038 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59) {
        return false;
    }
    int64_t localAsUtc = daysFromCivil(year, month, day) * 86400 +
        hour * 3600 + minute * 60;
    int normalizedYear;
    unsigned normalizedMonth;
    unsigned normalizedDay;
    civilFromDays(localAsUtc / 86400, normalizedYear, normalizedMonth, normalizedDay);
    if (year != normalizedYear || month != normalizedMonth || day != normalizedDay) {
        return false;
    }
    uint32_t standardTimeCandidate = static_cast<uint32_t>(localAsUtc - 2 * 3600);
    epoch = static_cast<uint32_t>(localAsUtc -
        (rigaDaylightSaving(standardTimeCandidate) ? 3 * 3600 : 2 * 3600));
    return rigaDaylightSaving(epoch) ? epoch + 3 * 3600 == localAsUtc
                                    : epoch + 2 * 3600 == localAsUtc;
}

String formatRigaDateTime(uint32_t epoch) {
    uint32_t localEpoch = epoch + (rigaDaylightSaving(epoch) ? 3UL : 2UL) * 3600UL;
    int year;
    unsigned month;
    unsigned day;
    civilFromDays(localEpoch / 86400UL, year, month, day);
    uint32_t secondsInDay = localEpoch % 86400UL;
    char formatted[17];
    snprintf(formatted, sizeof(formatted), "%04d-%02u-%02uT%02u:%02u",
        year, month, day, static_cast<unsigned>((secondsInDay / 3600UL)),
        static_cast<unsigned>((secondsInDay % 3600UL) / 60UL));
    return String(formatted);
}

#ifdef USE_WIFI
String htmlEscape(const char* value) {
    String escaped;
    while (*value != '\0') {
        switch (*value) {
            case '&': escaped += F("&amp;"); break;
            case '<': escaped += F("&lt;"); break;
            case '>': escaped += F("&gt;"); break;
            case '"': escaped += F("&quot;"); break;
            case '\'': escaped += F("&#39;"); break;
            default: escaped += *value; break;
        }
        ++value;
    }
    return escaped;
}

String decodeFormValue(const String& value) {
    String decoded;
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (c == '+') {
            decoded += ' ';
        } else if (c == '%' && i + 2 < value.length()) {
            char hex[3] = {value[i + 1], value[i + 2], '\0'};
            decoded += static_cast<char>(strtoul(hex, nullptr, 16));
            i += 2;
        } else {
            decoded += c;
        }
    }
    return decoded;
}

String formField(const String& body, const char* fieldName) {
    String key = String(fieldName) + "=";
    int start = body.indexOf(key);
    if (start < 0) {
        return String();
    }
    start += key.length();
    int end = body.indexOf('&', start);
    if (end < 0) {
        end = body.length();
    }
    return decodeFormValue(body.substring(start, end));
}

template <typename Client>
bool writeClientData(Client& client, const uint8_t* data, size_t length) {
    size_t sent = 0;
    unsigned long startedAt = millis();
    while (sent < length && millis() - startedAt < 5000UL) {
        kickWatchdog();
        size_t chunkSize = length - sent;
        if (chunkSize > 256) chunkSize = 256;
        size_t written = client.write(data + sent, chunkSize);
        if (written > 0) {
            sent += written;
            startedAt = millis();
        } else {
            delay(2);
        }
    }
    if (sent != length) {
        Serial.print("[WEB] Ошибка передачи HTTP-ответа: отправлено ");
        Serial.print(sent);
        Serial.print(" из ");
        Serial.print(length);
        Serial.println(" байт.");
        return false;
    }
    return true;
}

class StringPageWriter : public Print {
public:
    explicit StringPageWriter(String& output) : page(output) {}

    size_t write(uint8_t value) override {
        if (failed || !page.concat(static_cast<char>(value))) {
            failed = true;
            return 0;
        }
        return 1;
    }

    size_t write(const uint8_t* data, size_t length) override {
        if (failed || !page.concat(data, static_cast<unsigned int>(length))) {
            failed = true;
            return 0;
        }
        return length;
    }

    bool hasFailed() const {
        return failed;
    }

private:
    String& page;
    bool failed = false;
};

template <typename Client>
bool readHttpResponseBody(Client& client, String& response, size_t maxBodySize) {
    const unsigned long totalTimeoutMs = 20000UL;
    const unsigned long idleTimeoutMs = 5000UL;
    const size_t maxHeaderSize = 2048;
    const char headerTerminator[] = "\r\n\r\n";
    size_t terminatorMatch = 0;
    size_t headerSize = 0;
    bool headersComplete = false;
    unsigned long startedAt = millis();
    unsigned long lastByteAt = startedAt;

    response = "";
    if (!response.reserve(maxBodySize)) {
        Serial.println("[ERROR] Недостаточно памяти для HTTP-ответа.");
        return false;
    }

    while (client.connected() || client.available()) {
        kickWatchdog();
        unsigned long now = millis();
        if (now - startedAt >= totalTimeoutMs ||
            now - lastByteAt >= idleTimeoutMs) {
            Serial.println("[ERROR] Таймаут чтения HTTP-ответа.");
            return false;
        }
        if (client.available() > 0) {
            int value = client.read();
            if (value < 0) {
                delay(1);
                continue;
            }
            lastByteAt = millis();
            char byte = static_cast<char>(value);
            if (!headersComplete) {
                if (++headerSize > maxHeaderSize) {
                    Serial.println("[ERROR] HTTP-заголовки превышают допустимый размер.");
                    return false;
                }
                if (byte == headerTerminator[terminatorMatch]) {
                    ++terminatorMatch;
                    if (terminatorMatch == sizeof(headerTerminator) - 1) {
                        headersComplete = true;
                    }
                } else {
                    terminatorMatch = byte == headerTerminator[0] ? 1 : 0;
                }
            } else {
                if (response.length() >= maxBodySize) {
                    Serial.println("[ERROR] Тело HTTP-ответа превышает допустимый размер.");
                    return false;
                }
                response += byte;
            }
        } else {
            delay(1);
        }
    }

    if (!headersComplete) {
        Serial.println("[ERROR] HTTP-ответ не содержит полного заголовка.");
        return false;
    }
    return true;
}

bool parseFormFloat(const String& text, float minimum, float maximum, float& value) {
    if (text.length() == 0) {
        return false;
    }
    char* end = nullptr;
    value = strtof(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && isfinite(value) &&
        value >= minimum && value <= maximum;
}

bool parseFormInteger(const String& text, int minimum, int maximum, int& value) {
    if (text.length() == 0) {
        return false;
    }
    char* end = nullptr;
    long parsed = strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

template <typename Client>
void streamPriceChartSvg(Client& client) {
    const float plotLeft = 62.0f;
    const float plotTop = 24.0f;
    const float plotWidth = 600.0f;
    const float plotHeight = 220.0f;
    const float plotBottom = plotTop + plotHeight;
    const uint32_t now = timeSynchronized ? getCurrentEpochSeconds() : 0;
    const uint32_t currentLocalTime = timeSynchronized
        ? now + (rigaDaylightSaving(now) ? 3UL : 2UL) * 3600UL
        : 0;
    const uint32_t chartLocalDay = currentLocalTime / 86400UL + selectedChartDay;
    const uint32_t currentLocalSecondOfDay = currentLocalTime % 86400UL;

    client.print(F("<svg xmlns='http://www.w3.org/2000/svg' class='price-graph' "
                   "width='700' height='300' viewBox='0 0 700 300' role='img' "
                   "aria-label='График котировок за сутки от 0 до 50 центов за киловатт-час'>"
                   "<style>.grid-line{stroke:#d9dee5;stroke-width:1}"
                   ".axis-label{font:12px sans-serif;fill:#4b5563}"
                   ".axis-title{font:13px sans-serif;fill:#202124}"
                   ".price-line{fill:none;stroke:#1769aa;stroke-width:3}"
                   ".work-period{fill:#d1d5db;fill-opacity:.7}"
                   ".now-line{stroke:#d93025;stroke-width:2;stroke-dasharray:5 4}"
                   ".now-label{font:12px sans-serif;fill:#d93025}"
                   ".current-price{fill:#d93025;stroke:white;stroke-width:2}</style>"));

    bool workSlots[MAX_FORECAST_SLOTS] = {};
    buildRelayWorkSlotsForChart(selectedRelayIndex, chartLocalDay, workSlots);
    for (size_t i = 0; i < forecastSlotCount;) {
        if (!workSlots[i]) {
            ++i;
            continue;
        }
        uint32_t firstTimestamp = forecastSlots[i].timestamp;
        uint32_t endTimestamp = firstTimestamp + 900UL;
        size_t next = i + 1;
        while (next < forecastSlotCount && workSlots[next] &&
            forecastSlots[next].timestamp == endTimestamp) {
            endTimestamp += 900UL;
            ++next;
        }
        uint32_t localTimestamp = firstTimestamp +
            (rigaDaylightSaving(firstTimestamp) ? 3UL : 2UL) * 3600UL;
        float x = plotLeft + static_cast<float>(localTimestamp % 86400UL) /
            86400.0f * plotWidth;
        float width = plotWidth * static_cast<float>(endTimestamp - firstTimestamp) /
            86400.0f;
        client.print(F("<rect class='work-period' x='"));
        client.print(x, 1);
        client.print(F("' y='"));
        client.print(plotTop, 1);
        client.print(F("' width='"));
        client.print(width + 0.4f, 1);
        client.print(F("' height='"));
        client.print(plotHeight, 1);
        client.print(F("'/>"));
        i = next;
    }

    for (uint8_t price = 0; price <= 50; price += 10) {
        float y = plotBottom - static_cast<float>(price) / 50.0f * plotHeight;
        client.print(F("<line class='grid-line' x1='"));
        client.print(plotLeft, 1);
        client.print(F("' y1='"));
        client.print(y, 1);
        client.print(F("' x2='"));
        client.print(plotLeft + plotWidth, 1);
        client.print(F("' y2='"));
        client.print(y, 1);
        client.print(F("'/><text class='axis-label' x='"));
        client.print(plotLeft - 10, 1);
        client.print(F("' y='"));
        client.print(y + 4, 1);
        client.print(F("' text-anchor='end'>"));
        client.print(price);
        client.print(F("</text>"));
    }

    for (uint8_t hours = 0; hours <= 24; hours += 2) {
        float x = plotLeft + static_cast<float>(hours) / 24.0f * plotWidth;
        client.print(F("<line class='grid-line' x1='"));
        client.print(x, 1);
        client.print(F("' y1='"));
        client.print(plotTop, 1);
        client.print(F("' x2='"));
        client.print(x, 1);
        client.print(F("' y2='"));
        client.print(plotBottom, 1);
        client.print(F("'/><text class='axis-label' x='"));
        client.print(x, 1);
        client.print(F("' y='"));
        client.print(plotBottom + 19, 1);
        client.print(F("' text-anchor='middle'>"));
        if (hours < 10) client.print('0');
        client.print(hours);
        client.print(F(":00</text>"));
    }

    float currentX = plotLeft + static_cast<float>(currentLocalSecondOfDay) /
        86400.0f * plotWidth;
    if (selectedChartDay == 0) {
        client.print(F("<line class='now-line' x1='"));
        client.print(currentX, 1);
        client.print(F("' y1='"));
        client.print(plotTop, 1);
        client.print(F("' x2='"));
        client.print(currentX, 1);
        client.print(F("' y2='"));
        client.print(plotBottom, 1);
        client.print(F("'/><text class='now-label' x='"));
        client.print(currentX, 1);
        client.print(F("' y='15' text-anchor='middle'>Сейчас</text>"));
    }

    if (timeSynchronized && now != 0) {
        bool firstPoint = true;
        for (size_t i = 0; i < forecastSlotCount; ++i) {
            uint32_t timestamp = forecastSlots[i].timestamp;
            uint32_t localTimestamp = timestamp +
                (rigaDaylightSaving(timestamp) ? 3UL : 2UL) * 3600UL;
            if (localTimestamp / 86400UL != chartLocalDay) {
                continue;
            }
            if (firstPoint) {
                client.print(F("<polyline class='price-line' points='"));
                firstPoint = false;
            } else {
                client.print(' ');
            }
            float x = plotLeft + static_cast<float>(localTimestamp % 86400UL) /
                86400.0f * plotWidth;
            float price = forecastSlots[i].priceEurKwh * 100.0f;
            if (price < 0.0f) price = 0.0f;
            if (price > 50.0f) price = 50.0f;
            float y = plotBottom - price / 50.0f * plotHeight;
            client.print(x, 1);
            client.print(',');
            client.print(y, 1);
        }
        if (!firstPoint) {
            client.print(F("'/>"));
        }
    }

    if (selectedChartDay == 0 && hasLatestPrice) {
        float price = latestPriceEurKwh * 100.0f;
        if (price < 0.0f) price = 0.0f;
        if (price > 50.0f) price = 50.0f;
        float y = plotBottom - price / 50.0f * plotHeight;
        client.print(F("<circle class='current-price' cx='"));
        client.print(currentX, 1);
        client.print(F("' cy='"));
        client.print(y, 1);
        client.print(F("' r='5'/>"));
    }

    client.print(F("<text class='axis-title' x='"));
    client.print(plotLeft + plotWidth / 2, 1);
    client.print(F("' y='"));
    client.print(plotBottom + 47, 1);
    client.print(F("' text-anchor='middle'>Местное время (Europe/Riga)</text>"
                   "<text class='axis-title' transform='translate(15 135) rotate(-90)' "
                   "text-anchor='middle'>Цена, центов/кВт·ч</text></svg>"));

}

template <typename Client>
void sendPriceChart(Client& networkClient) {
    String chart;
    if (!chart.reserve(9000)) {
        Serial.println("[WEB] Недостаточно памяти для формирования графика.");
        networkClient.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n"));
        return;
    }
    StringPageWriter writer(chart);
    streamPriceChartSvg(writer);
    if (writer.hasFailed()) {
        Serial.println("[WEB] Не удалось сформировать полный SVG графика.");
        networkClient.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n"));
        return;
    }
    networkClient.println(F("HTTP/1.1 200 OK"));
    networkClient.println(F("Content-Type: image/svg+xml; charset=utf-8"));
    networkClient.println(F("Cache-Control: no-store"));
    networkClient.println(F("Connection: close"));
    networkClient.print(F("Content-Length: "));
    networkClient.println(chart.length());
    networkClient.println();
    writeClientData(networkClient,
        reinterpret_cast<const uint8_t*>(chart.c_str()), chart.length());
}

template <typename Client>
void sendPortalPage(Client& networkClient, const String& message) {
    const DeviceConfig& deviceConfig = relayConfigs[selectedRelayIndex];
    String page;
    if (!page.reserve(18000)) {
        Serial.println("[WEB] Недостаточно памяти для формирования страницы настроек.");
        networkClient.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n"));
        return;
    }
    StringPageWriter client(page);

    client.print(F("<!doctype html><html lang='ru'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Opta Settings</title>"
        "<style>body{font:16px sans-serif;max-width:1100px;margin:2em auto;padding:0 1em;color:#202124}"
        ".settings-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}"
        "fieldset{margin:0;padding:1em;border:1px solid #bbb;border-radius:8px;min-width:0}"
        "input,select,button{box-sizing:border-box;padding:10px;margin:5px 0;max-width:100%}"
        "input[type=number],input[type=datetime-local],select{width:100%}"
        "label{display:block;margin:10px 0}.row{display:flex;gap:12px}"
        ".row label{flex:1}button{background:#1769aa;color:white;border:0;padding:12px 20px}"
        ".price-chart,.form-actions{grid-column:1/-1}.price-graph{display:block;width:100%;height:auto}"
        ".grid-line{stroke:#d9dee5;stroke-width:1}.axis-label{font-size:12px;fill:#4b5563}"
        ".axis-title{font-size:13px;fill:#202124}.price-line{fill:none;stroke:#1769aa;stroke-width:3}"
        ".work-period{fill:#d1d5db;fill-opacity:.7}"
        ".now-line{stroke:#d93025;stroke-width:2;stroke-dasharray:5 4}"
        ".now-label{font-size:12px;fill:#d93025}"
        ".current-price{fill:#d93025;stroke:white;stroke-width:2}.chart-note{margin:0;color:#5f6368}"
        ".work-legend{display:inline-block;width:14px;height:14px;margin-right:6px;"
        "vertical-align:-2px;background:#d1d5db;border:1px solid #9ca3af}"
        ".day-switch{display:flex;gap:8px;margin:8px 0 12px}.day-switch a{padding:8px 14px;"
        "border:1px solid #1769aa;border-radius:5px;color:#1769aa;text-decoration:none}"
        ".day-switch a.active{background:#1769aa;color:white}"
        ".form-actions{display:flex;gap:10px;flex-wrap:wrap}"
        "@media(max-width:640px){.settings-grid{grid-template-columns:1fr}"
        ".price-chart,.form-actions{grid-column:auto}}</style>"
        "</head><body><h1>Настройки Opta</h1><p>Устройство: "
        "<a href='http://opta-settings.local'>http://opta-settings.local</a></p>"
        "<form method='GET' action='/'><label>Настраиваемое реле"
        "<select name='relay' onchange='this.form.submit()'>"));

    for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
        client.print(F("<option value='"));
        client.print(i + 1);
        client.print(F("'"));
        if (i == selectedRelayIndex) client.print(F(" selected"));
        client.print(F(">Реле "));
        client.print(i + 1);
        client.print(F(" (D"));
        client.print(i);
        client.print(F(")</option>"));
    }

    client.print(F("</select></label></form><form method='POST' action='/save'><input type='hidden' name='relay' value='"));
    client.print(selectedRelayIndex + 1);
    client.print(F("'><div class='settings-grid'><fieldset class='price-chart'><legend>График цены</legend>"
        "<div class='day-switch'>"));

    for (uint8_t day = 0; day <= 1; ++day) {
        client.print(F("<a href='/?relay="));
        client.print(selectedRelayIndex + 1);
        client.print(F("&amp;day="));
        client.print(day);
        client.print(F("'"));
        if (selectedChartDay == day) client.print(F(" class='active'"));
        client.print(F(">"));
        client.print(day == 0 ? F("Сегодня") : F("Завтра"));
        client.print(F("</a>"));
    }
    client.print(F("</div><img class='price-graph' src='/chart.svg?relay="));
    client.print(selectedRelayIndex + 1);
    client.print(F("&amp;day="));
    client.print(selectedChartDay);
    client.print(F("' alt='График цены за выбранный день'>"
        "<p class='chart-note'>Синяя линия — котировки. Серым отмечено запланированное "
        "время работы выбранного реле.</p>"));

    client.print(F("</fieldset>"));
    if (wifiCredentialsVisible) {
        client.print(F("<fieldset><legend>Подключение Wi-Fi</legend>"
                       "<label>Имя сети (SSID)<input name='manual_ssid' maxlength='32' required></label>"
                       "<label>Пароль Wi-Fi<input name='password' type='password' maxlength='63' "
                       "autocomplete='new-password'></label></fieldset>"));
    }

    client.print(F("<fieldset><legend>Максимально допустимая цена (EUR/kWh)</legend>"
        "<label>Аварийный предел<input name='safety' type='number' step='0.00001' min='0' max='100' value='"));
    client.print(deviceConfig.safetyCeilingEurKwh, 5);
    client.print(F("'></label><small>Выше этого значения реле всегда выключено.</small></fieldset>"
        "<fieldset><legend>Порог включения (EUR/kWh)</legend>"
        "<label>Порог включения<input name='threshold' type='number' step='0.00001' min='-100' max='100' value='"));
    client.print(deviceConfig.alwaysOnBelowPriceEurKwh, 5);
    client.print(F("'></label><small>Постоянно включен, когда цена ниже этого порога.</small></fieldset>"
        "<fieldset><legend>Умный порог включения</legend><label><input type='checkbox' name='smart_enabled' value='1'"));
    if (deviceConfig.smartThresholdEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Количество самых дешевых часов за день"
        "<input name='smart_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.smartThresholdHours);
    client.print(F("'></label><small>Включает реле в выбранные самые дешевые часы текущего дня.</small></fieldset>"
        "<fieldset><legend>Включение перед пробуждением</legend>"
        "<label><input type='checkbox' name='wakeup_enabled' value='1'"));
    if (deviceConfig.wakeupEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Количество часов до пробуждения"
        "<input name='wakeup_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.wakeupHours);
    client.print(F("'></label><label>Время, к которому нужно закончить нагрев"
        "<input name='wakeup_time' type='time' value='"));
    client.print(formatTimeOfDay(deviceConfig.wakeupTimeMinuteOfDay));
    client.print(F("'></label></fieldset>"
        "<fieldset><legend>Включение ночью</legend><label><input type='checkbox' name='night_enabled' value='1'"));
    if (deviceConfig.nightPrepEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><div class='row'><label>Начало окна"
        "<input name='night_start' type='time' value='"));
    client.print(formatTimeOfDay(deviceConfig.nightStartMinuteOfDay));
    client.print(F("'></label><label>Конец окна<input name='night_end' type='time' value='"));
    client.print(formatTimeOfDay(deviceConfig.nightEndMinuteOfDay));
    client.print(F("'></label></div><label>Количество самых дешевых часов<input name='night_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.nightCheapHours);
    client.print(F("'></label></fieldset>"
        "<fieldset><legend>Непрерывный нагрев</legend><label><input type='checkbox' name='block_enabled' value='1'"));
    if (deviceConfig.consecutiveBlockEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Длительность блока, часов<input name='block_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.consecutiveBlockHours);
    client.print(F("'></label></fieldset>"
        "<fieldset><legend>Календарь / отпуск</legend><label><input type='checkbox' name='holiday_enabled' value='1'"));
    if (deviceConfig.holidayEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Дата и время возвращения (Europe/Riga)"
        "<input name='return_local' type='datetime-local' value='"));
    if (deviceConfig.holidayReturnEpoch != 0) {
        client.print(formatRigaDateTime(deviceConfig.holidayReturnEpoch));
    }
    client.print(F("'></label><label>Часы нагрева перед возвращением"
        "<input name='holiday_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.holidayHeatHours);
    client.print(F("'></label><small>До начала подготовки нагрев отключен. "
        "Приоритет: аварийный предел, отпуск, постоянный порог, умный порог, пробуждение, включение ночью, непрерывный нагрев.</small></fieldset>"
        "<div class='form-actions'><button type='submit'>Сохранить настройки</button>"
        "<button type='submit' name='action' value='save_restart'>Сохранить и перезапустить</button></div>"
        "</div></form>"));

    if (message.length() > 0) {
        client.print(F("<p><strong>"));
        client.print(htmlEscape(message.c_str()));
        client.print(F("</strong></p>"));
    }
    client.println(F("</body></html>"));

    if (client.hasFailed()) {
        Serial.println("[WEB] Не удалось сформировать полную страницу настроек.");
        networkClient.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n"));
        return;
    }

    networkClient.println(F("HTTP/1.1 200 OK"));
    networkClient.println(F("Content-Type: text/html; charset=utf-8"));
    networkClient.println(F("Cache-Control: no-store"));
    networkClient.println(F("Connection: close"));
    networkClient.print(F("Content-Length: "));
    networkClient.println(page.length());
    networkClient.println();
    writeClientData(networkClient,
        reinterpret_cast<const uint8_t*>(page.c_str()), page.length());
}

bool startWifiManager() {
    Serial.println("[WiFi] Запуск точки настройки Opta-Config-AP...");
    portalServer.end();
    settingsServerStarted = false;
    if (dnsUdpStarted) {
        dnsUdp.stop();
        dnsUdpStarted = false;
    }
    if (mdnsUdpStarted) {
        mdnsUdp.stop();
        mdnsUdpStarted = false;
    }
    WiFi.disconnect();
    WiFi.config("192.168.4.1", "255.255.255.0", "192.168.4.1");
    if (WiFi.beginAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD) != WL_AP_LISTENING) {
        Serial.println("[ERROR] Не удалось запустить точку доступа конфигурации.");
        return false;
    }
    if (!dnsUdpStarted) {
        if (!dnsUdp.begin(53)) {
            Serial.println("[ERROR] Не удалось открыть DNS-порт для Captive Portal.");
            WiFi.end();
            return false;
        }
        dnsUdpStarted = true;
    }
    portalMode = true;
    lastPortalReconnectMillis = millis();
    wifiCredentialsVisible = true;
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

void stopWifiManager() {
    portalServer.end();
    settingsServerStarted = false;
    if (dnsUdpStarted) {
        dnsUdp.stop();
        dnsUdpStarted = false;
    }
    if (mdnsUdpStarted) {
        mdnsUdp.stop();
        mdnsUdpStarted = false;
    }
    // disconnect() останавливает точку доступа, end() переключает стек обратно на STA
    WiFi.disconnect();
    WiFi.end();
    // Сброс статического 192.168.4.1, заданного для точки доступа: в домашней сети нужен DHCP
    WiFi.config(INADDR_NONE);
    portalMode = false;
}

// Точка настройки не должна оставаться навсегда после временного сбоя роутера:
// периодически пробуем вернуться к сохраненной сети.
void servicePortalReconnect() {
    unsigned long now = millis();
    if (now - lastPortalReconnectMillis < PORTAL_RECONNECT_INTERVAL_MS ||
        now - lastPortalActivityMillis < PORTAL_ACTIVITY_GRACE_MS) {
        return;
    }
    lastPortalReconnectMillis = now;

    WifiCredentials credentials = {};
    if (!loadWifiCredentials(credentials)) {
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
        wifiCredentialsVisible = false;
        Serial.print("[WiFi] Подключено, точка настройки закрыта. IP-адрес: ");
        Serial.println(WiFi.localIP());
        if (!wiredServerStarted) {
            startSettingsWebServer();
        }
        return;
    }

    Serial.println("[WiFi] Сохраненная сеть недоступна; точка настройки запускается снова.");
    if (!startWifiManager()) {
        Serial.println("[ERROR] Не удалось повторно запустить точку настройки.");
    }
}

bool startSettingsWebServer() {
    if (!settingsServerStarted) {
        IPAddress bindAddress = portalMode ? CONFIG_AP_IP : WiFi.localIP();
        nsapi_error_t result = portalServer.beginOnAddress(bindAddress);
        if (result != NSAPI_ERROR_OK) {
            Serial.print("[ERROR] Не удалось открыть HTTP-порт 80 на ");
            Serial.print(bindAddress);
            Serial.print("; код Mbed: ");
            Serial.println(result);
            return false;
        }
        settingsServerStarted = true;
        Serial.print("[WEB] HTTP-сервер слушает ");
        Serial.print(bindAddress);
        Serial.println(":80.");
    }
    if (!mdnsUdpStarted) {
        if (!mdnsUdp.beginMulticast(MDNS_MULTICAST_IP, 5353)) {
            mdnsUdp.stop();
            Serial.println("[ERROR] Не удалось включить mDNS UDP multicast; HTTP доступен по IP.");
            return false;
        }
        mdnsUdpStarted = true;
    }
    Serial.println("[WEB] Настройки: http://opta-settings.local");
    return true;
}

void startWiredWebServer() {
    if (!wiredServerStarted) {
        IPAddress address = Ethernet.localIP();
        nsapi_error_t result = wiredServer.beginListening();
        if (result != NSAPI_ERROR_OK) {
            Serial.print("[ERROR] Не удалось открыть HTTP-порт 80 на Ethernet ");
            Serial.print(address);
            Serial.print("; этап: ");
            Serial.print(wiredServer.lastErrorStage());
            Serial.print(", код Mbed: ");
            Serial.println(result);
            nextWiredServerAttemptMillis = millis() + 5000UL;
            return;
        }
        wiredServerStarted = true;
        nextWiredServerAttemptMillis = 0;
        wiredServerAddress = address;
        Serial.print("[WEB] HTTP-сервер Ethernet слушает ");
        Serial.print(address);
        Serial.println(":80.");
        Serial.print("[Ethernet] IP-адрес: ");
        Serial.println(address);
        if (!wiredMdnsStarted) {
            if (wiredMdnsUdp.beginMulticast(MDNS_MULTICAST_IP, 5353)) {
                wiredMdnsStarted = true;
                Serial.println("[mDNS] opta-settings.local доступен через Ethernet.");
            } else {
                Serial.println("[WARN] Ethernet HTTP работает по IP; запуск mDNS не удался.");
            }
        }
    }
}

void serviceWiredNetwork(bool forceAttempt = false) {
    IPAddress address = Ethernet.localIP();
    if (wiredServerStarted &&
        (address[0] == 0 || address != wiredServerAddress)) {
        wiredServer.end();
        wiredServerStarted = false;
        wiredServerAddress = IPAddress();
        if (wiredMdnsStarted) {
            wiredMdnsUdp.stop();
            wiredMdnsStarted = false;
        }
        Serial.println("[Ethernet] IP изменился или потерян; перезапуск HTTP-сервера.");
    }
    if (address[0] != 0) {
        if (!wiredServerStarted &&
            (nextWiredServerAttemptMillis == 0 ||
             millis() >= nextWiredServerAttemptMillis)) {
            startWiredWebServer();
        }
        return;
    }

    unsigned long now = millis();
    if (!forceAttempt && now < nextEthernetAttemptMillis) {
        return;
    }
    nextEthernetAttemptMillis = now + 15000UL;

    if (Ethernet.linkStatus() == LinkON || forceAttempt) {
        kickWatchdog();
        if (wiredDhcpAttempted) {
            Ethernet.disconnect();
        }
        wiredDhcpAttempted = true;
        int connected = Ethernet.begin(nullptr, forceAttempt ? 3000UL : 1000UL, 1000UL);
        kickWatchdog();
        Serial.print("[Ethernet] DHCP/link init result: ");
        Serial.print(connected);
        Serial.print(", IP: ");
        Serial.println(Ethernet.localIP());
    } else {
        // Кабель Ethernet не подключен (LinkOFF / LinkUnknown)
        wiredDhcpAttempted = false;
    }
}

template <typename Udp>
void serviceMDNSOn(Udp& udp, const IPAddress& address, const char* interfaceName) {
    int packetSize = udp.parsePacket();
    if (packetSize < 17 || packetSize > 512) {
        return;
    }
    IPAddress querySource = udp.remoteIP();
    uint16_t querySourcePort = udp.remotePort();
    Serial.print("[mDNS] Пакет ");
    Serial.print(packetSize);
    Serial.print(" байт на ");
    Serial.print(interfaceName);
    Serial.print(" от ");
    Serial.print(querySource);
    Serial.print(":");
    Serial.println(querySourcePort);
    uint8_t packet[512];
    int receivedSize = udp.read(packet, sizeof(packet));
    if (receivedSize < 17 || (packet[2] & 0x80) != 0 ||
        (packet[4] == 0 && packet[5] == 0) || (packet[2] & 0x78) != 0) {
        return;
    }

    bool requestUnicastResponse = querySourcePort != 5353;
    size_t questionEnd = 12;
    size_t matchingNameOffset = 0;
    bool isDeviceQuery = false;
    uint16_t questionCount = (static_cast<uint16_t>(packet[4]) << 8) | packet[5];
    for (uint16_t question = 0; question < questionCount; ++question) {
        if (questionEnd >= static_cast<size_t>(receivedSize)) {
            return;
        }
        size_t nameOffset = questionEnd;
        uint8_t labelIndex = 0;
        bool matches = true;
        while (questionEnd < static_cast<size_t>(receivedSize) && packet[questionEnd] != 0) {
            uint8_t labelLength = packet[questionEnd++];
            if ((labelLength & 0xC0) != 0 ||
                questionEnd + labelLength > static_cast<size_t>(receivedSize)) {
                return;
            }
            const char* expected = labelIndex == 0 ? "opta-settings" :
                (labelIndex == 1 ? "local" : nullptr);
            if (expected == nullptr || labelLength != strlen(expected)) {
                matches = false;
            } else {
                for (size_t i = 0; i < labelLength; ++i) {
                    if (tolower(packet[questionEnd + i]) != expected[i]) {
                        matches = false;
                    }
                }
            }
            ++labelIndex;
            questionEnd += labelLength;
        }
        if (questionEnd >= static_cast<size_t>(receivedSize) ||
            questionEnd + 5 > static_cast<size_t>(receivedSize)) {
            return;
        }
        ++questionEnd; // пропускаем нулевой байт терминатора имени
        uint16_t queryType = (static_cast<uint16_t>(packet[questionEnd]) << 8) |
            packet[questionEnd + 1];
        uint16_t queryClass = (static_cast<uint16_t>(packet[questionEnd + 2]) << 8) |
            packet[questionEnd + 3];
        if (matches && labelIndex == 2 && queryClass % 0x8000 == 1 &&
            (queryType == 1 || queryType == 255)) {
            isDeviceQuery = true;
            matchingNameOffset = nameOffset;
            requestUnicastResponse = requestUnicastResponse ||
                (queryClass & 0x8000) != 0;
        }
        questionEnd += 4;
    }
    if (!isDeviceQuery || questionEnd > static_cast<size_t>(receivedSize) ||
        matchingNameOffset >= questionEnd || matchingNameOffset >= 0x4000) {
        Serial.println("[mDNS] Запрос не содержит поддерживаемое имя opta-settings.local.");
        return;
    }

    packet[2] = 0x84;
    packet[3] = 0x00;
    packet[6] = 0;
    packet[7] = 1;
    packet[8] = 0;
    packet[9] = 0;
    packet[10] = 0;
    packet[11] = 0;
    const uint8_t answer[] = {
        0xC0, 0x00, 0x00, 0x01, 0x80, 0x01,
        0x00, 0x00, 0x00, 0x78, 0x00, 0x04,
        0, 0, 0, 0
    };
    uint8_t answerPacket[512];
    if (questionEnd + sizeof(answer) > sizeof(answerPacket)) {
        return;
    }
    memcpy(answerPacket, packet, questionEnd);
    memcpy(answerPacket + questionEnd, answer, sizeof(answer));
    answerPacket[questionEnd] = static_cast<uint8_t>(0xC0 | (matchingNameOffset >> 8));
    answerPacket[questionEnd + 1] = static_cast<uint8_t>(matchingNameOffset & 0xFF);
    answerPacket[questionEnd + sizeof(answer) - 4] = address[0];
    answerPacket[questionEnd + sizeof(answer) - 3] = address[1];
    answerPacket[questionEnd + sizeof(answer) - 2] = address[2];
    answerPacket[questionEnd + sizeof(answer) - 1] = address[3];
    IPAddress responseAddress = requestUnicastResponse ? querySource : MDNS_MULTICAST_IP;
    uint16_t responsePort = requestUnicastResponse ? querySourcePort : 5353;
    if (udp.beginPacket(responseAddress, responsePort)) {
        udp.write(answerPacket, questionEnd + sizeof(answer));
        int result = udp.endPacket();
        if (result <= 0) {
            Serial.println("[mDNS] Ошибка отправки ответа opta-settings.local.");
        } else {
            Serial.print("[mDNS] Ответ opta-settings.local отправлен через ");
            Serial.print(interfaceName);
            Serial.print(" на ");
            Serial.print(responseAddress);
            Serial.print(":");
            Serial.println(responsePort);
        }
    } else {
        Serial.println("[mDNS] Не удалось подготовить UDP-ответ.");
    }
}

void serviceMDNS() {
    if (mdnsUdpStarted) {
        IPAddress address = portalMode ? CONFIG_AP_IP : WiFi.localIP();
        serviceMDNSOn(mdnsUdp, address, "Wi-Fi");
    }
}

void handleDnsRequests() {
    int packetSize = dnsUdp.parsePacket();
    if (packetSize < 17 || packetSize > 512) {
        return;
    }
    uint8_t packet[512];
    int receivedSize = dnsUdp.read(packet, sizeof(packet));
    if (receivedSize < 17 || (packet[2] & 0x80) != 0) {
        return;
    }

    size_t questionEnd = 12;
    while (questionEnd < static_cast<size_t>(receivedSize) && packet[questionEnd] != 0) {
        uint8_t labelLength = packet[questionEnd++];
        if ((labelLength & 0xC0) != 0 || questionEnd + labelLength > static_cast<size_t>(receivedSize)) {
            return;
        }
        questionEnd += labelLength;
    }
    if (questionEnd >= static_cast<size_t>(receivedSize) ||
        questionEnd + 5 > static_cast<size_t>(receivedSize)) {
        return;
    }
    questionEnd += 5; // Пропуск нулевого терминатора и QTYPE/QCLASS
    packet[2] = 0x81;
    packet[3] = 0x80;
    packet[6] = 0;
    packet[7] = 1;
    packet[8] = 0;
    packet[9] = 0;
    packet[10] = 0;
    packet[11] = 0;

    const uint8_t answer[] = {
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
        192, 168, 4, 1
    };
    if (questionEnd + sizeof(answer) > sizeof(packet)) {
        return;
    }
    memcpy(packet + questionEnd, answer, sizeof(answer));
    if (dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort())) {
        dnsUdp.write(packet, questionEnd + sizeof(answer));
        dnsUdp.endPacket();
    }
}

// Читает одну строку HTTP-запроса с общим таймаутом и ограничением длины,
// чтобы медленный или некорректный клиент не блокировал loop() и память.
template <typename Client>
bool readHttpLine(Client& client, String& line, unsigned long requestStartedAt) {
    line = "";
    while (millis() - requestStartedAt < HTTP_REQUEST_TIMEOUT_MS) {
        kickWatchdog();
        if (client.available() > 0) {
            int value = client.read();
            if (value < 0) {
                delay(1);
                continue;
            }
            if (value == '\n') {
                line.trim();
                return true;
            }
            if (line.length() >= HTTP_MAX_LINE_LENGTH) {
                return false;
            }
            line += static_cast<char>(value);
        } else if (!client.connected()) {
            return false;
        } else {
            delay(1);
        }
    }
    return false;
}

template <typename Client>
void handlePortalClient(Client& client) {
    lastPortalActivityMillis = millis();
    unsigned long requestStartedAt = millis();
    String requestLine;
    if (!readHttpLine(client, requestLine, requestStartedAt)) {
        Serial.println("[WEB] Запрос отклонен: таймаут или слишком длинная строка.");
        client.stop();
        return;
    }
    int firstSpace = requestLine.indexOf(' ');
    int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    String method = firstSpace >= 0 ? requestLine.substring(0, firstSpace) : String();
    String path = secondSpace > firstSpace
        ? requestLine.substring(firstSpace + 1, secondSpace)
        : String("/");
    Serial.print("[WEB] Запрос: ");
    Serial.print(method);
    Serial.println(path);

    int contentLength = 0;
    bool headersComplete = false;
    for (uint8_t headerCount = 0; headerCount <= HTTP_MAX_HEADER_COUNT; ++headerCount) {
        String header;
        if (!readHttpLine(client, header, requestStartedAt)) {
            break;
        }
        if (header.length() == 0) {
            headersComplete = true;
            break;
        }
        header.toLowerCase();
        if (header.startsWith("content-length:")) {
            contentLength = header.substring(15).toInt();
        }
    }
    if (!headersComplete) {
        Serial.println("[WEB] Запрос отклонен: заголовки не получены полностью.");
        client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n"));
        client.stop();
        return;
    }

    int queryStart = path.indexOf("?relay=");
    if (method == "GET" && queryStart >= 0) {
        int relayNumber = path.substring(queryStart + 7).toInt();
        if (relayNumber >= 1 && relayNumber <= RELAY_COUNT) {
            selectedRelayIndex = static_cast<uint8_t>(relayNumber - 1);
        }
    }
    int chartDayStart = path.indexOf("?day=");
    if (chartDayStart < 0) {
        chartDayStart = path.indexOf("&day=");
    }
    if (method == "GET" && chartDayStart >= 0) {
        int valueStart = chartDayStart + 5;
        int valueEnd = path.indexOf('&', valueStart);
        String dayValue = valueEnd >= 0
            ? path.substring(valueStart, valueEnd)
            : path.substring(valueStart);
        int chartDay;
        if (parseFormInteger(dayValue, 0, 1, chartDay)) {
            selectedChartDay = static_cast<uint8_t>(chartDay);
        }
    }
    if (method == "GET" && path.startsWith("/chart.svg")) {
        sendPriceChart(client);
        client.stop();
        return;
    }

    if (method == "POST" && path == "/save") {
        if (contentLength <= 0 || contentLength > 2048) {
            client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid form"));
            client.stop();
            return;
        }
        String body;
        unsigned long bodyStartedAt = millis();
        while (body.length() < static_cast<unsigned int>(contentLength) &&
               millis() - bodyStartedAt < 5000UL) {
            kickWatchdog();
            while (client.available() && body.length() < static_cast<unsigned int>(contentLength)) {
                body += static_cast<char>(client.read());
            }
            delay(1);
        }
        if (body.length() != static_cast<unsigned int>(contentLength)) {
            client.println(F("HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\nIncomplete form"));
            client.stop();
            return;
        }

        int relayNumber;
        if (!parseFormInteger(formField(body, "relay"), 1, RELAY_COUNT, relayNumber)) {
            client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid relay selection"));
            client.stop();
            return;
        }
        selectedRelayIndex = static_cast<uint8_t>(relayNumber - 1);
        DeviceConfig updated = relayConfigs[selectedRelayIndex];
        float safetyCeiling;
        float alwaysOnThreshold;
        int smartHours, wakeupHours, nightHours, blockHours, holidayHours;
        uint16_t wakeupTime, nightStart, nightEnd;
        if (!parseFormFloat(formField(body, "safety"), 0.0f, 100.0f, safetyCeiling) ||
            !parseFormFloat(formField(body, "threshold"), -100.0f, 100.0f, alwaysOnThreshold) ||
            !parseFormInteger(formField(body, "smart_hours"), 1, 24, smartHours) ||
            !parseFormInteger(formField(body, "wakeup_hours"), 1, 24, wakeupHours) ||
            !parseTimeOfDay(formField(body, "wakeup_time"), wakeupTime) ||
            !parseTimeOfDay(formField(body, "night_start"), nightStart) ||
            !parseTimeOfDay(formField(body, "night_end"), nightEnd) ||
            nightStart == nightEnd ||
            !parseFormInteger(formField(body, "night_hours"), 1, 24, nightHours) ||
            !parseFormInteger(formField(body, "block_hours"), 1, 24, blockHours) ||
            !parseFormInteger(formField(body, "holiday_hours"), 1, 24, holidayHours) ||
            safetyCeiling <= alwaysOnThreshold) {
            client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid scenario settings"));
            client.stop();
            return;
        }
        uint16_t nightWindowMinutes = nightStart < nightEnd
            ? nightEnd - nightStart
            : 1440 - nightStart + nightEnd;
        if (formField(body, "night_enabled") == "1" &&
            nightHours > nightWindowMinutes / 60) {
            client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nNight heating hours exceed the window"));
            client.stop();
            return;
        }

        updated.safetyCeilingEurKwh = safetyCeiling;
        updated.alwaysOnBelowPriceEurKwh = alwaysOnThreshold;
        updated.smartThresholdEnabled = formField(body, "smart_enabled") == "1";
        updated.smartThresholdHours = static_cast<uint8_t>(smartHours);
        updated.wakeupEnabled = formField(body, "wakeup_enabled") == "1";
        updated.wakeupHours = static_cast<uint8_t>(wakeupHours);
        updated.wakeupTimeMinuteOfDay = wakeupTime;
        updated.nightPrepEnabled = formField(body, "night_enabled") == "1";
        updated.nightStartMinuteOfDay = nightStart;
        updated.nightEndMinuteOfDay = nightEnd;
        updated.nightCheapHours = static_cast<uint8_t>(nightHours);
        updated.consecutiveBlockEnabled = formField(body, "block_enabled") == "1";
        updated.consecutiveBlockHours = static_cast<uint8_t>(blockHours);
        updated.holidayEnabled = formField(body, "holiday_enabled") == "1";
        updated.holidayHeatHours = static_cast<uint8_t>(holidayHours);

        String returnDate = formField(body, "return_local");
        if (returnDate.length() > 0 &&
            !parseRigaDateTime(returnDate, updated.holidayReturnEpoch)) {
            client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid Europe/Riga return date/time"));
            client.stop();
            return;
        }
        if (updated.holidayEnabled &&
            (updated.holidayReturnEpoch == 0 ||
             (timeSynchronized && updated.holidayReturnEpoch <= getCurrentEpochSeconds()))) {
            client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nReturn date must be in the future"));
            client.stop();
            return;
        }

        String networkName;
        String networkPassword;
        if (wifiCredentialsVisible) {
            networkName = formField(body, "manual_ssid");
            if (networkName.length() == 0) {
                networkName = formField(body, "ssid");
            }
            networkPassword = formField(body, "password");
            if (networkName.length() > 32 || networkPassword.length() > 63) {
                client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid Wi-Fi credentials"));
                client.stop();
                return;
            }
        }

        DeviceConfig updatedConfigs[RELAY_COUNT];
        memcpy(updatedConfigs, relayConfigs, sizeof(updatedConfigs));
        updatedConfigs[selectedRelayIndex] = updated;
        if (!saveDeviceConfigs(updatedConfigs)) {
            client.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nUnable to save device settings"));
            client.stop();
            Serial.println("[ERROR] Не удалось сохранить настройки устройства в KVStore.");
            return;
        }
        memcpy(relayConfigs, updatedConfigs, sizeof(relayConfigs));
        lockedConsecutiveBlockStart[selectedRelayIndex] = 0;
        lockedConsecutiveBlockEnd[selectedRelayIndex] = 0;

        bool credentialsChanged = networkName.length() > 0;
        bool restartRequested = formField(body, "action") == "save_restart";
        if (credentialsChanged) {
            WifiCredentials credentials = {};
            networkName.toCharArray(credentials.ssid, sizeof(credentials.ssid));
            networkPassword.toCharArray(credentials.password, sizeof(credentials.password));
            if (!saveWifiCredentials(credentials)) {
                client.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nUnable to save Wi-Fi credentials"));
                client.stop();
                Serial.println("[ERROR] Не удалось сохранить Wi-Fi данные в KVStore.");
                return;
            }
        }
        if (!credentialsChanged && WiFi.status() == WL_CONNECTED) {
            applyCurrentPriceToConfiguredControl(selectedRelayIndex);
        }
        if (credentialsChanged || restartRequested) {
            client.println(F("HTTP/1.1 303 See Other"));
            client.println(F("Location: /"));
            client.println(F("Cache-Control: no-store"));
            client.println(F("Connection: close"));
            client.println();
            client.flush();
            client.stop();
            delay(500);
            NVIC_SystemReset();
            return;
        }
        client.println(F("HTTP/1.1 303 See Other"));
        client.println(F("Location: /"));
        client.println(F("Cache-Control: no-store"));
        client.println(F("Connection: close"));
        client.println();
        client.stop();
        return;
    }

    sendPortalPage(client, String());
    client.stop();
}

void serviceWifiManager() {
    serviceWiredNetwork();
    handleDnsRequests();
    serviceMDNS();
    serviceSettingsWebServer();
    serviceWiredWebServer();
}

void serviceSettingsWebServer() {
    arduino::WiFiClient client = portalServer.accept();
    if (client) {
        Serial.println("[WEB] Принято HTTP-соединение.");
        handlePortalClient(client);
    }
}

void serviceWiredWebServer() {
    if (wiredMdnsStarted) {
        serviceMDNSOn(wiredMdnsUdp, Ethernet.localIP(), "Ethernet");
    }
    if (!wiredServerStarted) {
        return;
    }
    arduino::EthernetClient client = wiredServer.accept();
    if (client) {
        Serial.println("[WEB] Принято Ethernet HTTP-соединение.");
        handlePortalClient(client);
    }
}
#endif

struct PriceHour {
    uint32_t timestamp;
    float averagePrice;
    uint8_t quarterCount;
};

bool fetchPriceForecast() {
    forecastSlotCount = 0;
    if (!timeSynchronized) {
        Serial.println("[FORECAST] Нет синхронизированного времени; прогноз не запрошен.");
        return false;
    }

    uint32_t nowEpoch = getCurrentEpochSeconds();
    uint32_t nowLocal = nowEpoch +
        (rigaDaylightSaving(nowEpoch) ? 3UL : 2UL) * 3600UL;
    uint32_t localDayStart = nowLocal / 86400UL * 86400UL;
    uint32_t localRangeEnd = localDayStart + 2UL * 86400UL - 15UL * 60UL;
    uint32_t startCandidate = localDayStart - 2UL * 3600UL;
    uint32_t endCandidate = localRangeEnd - 2UL * 3600UL;
    time_t startEpoch = static_cast<time_t>(localDayStart -
        (rigaDaylightSaving(startCandidate) ? 3UL : 2UL) * 3600UL);
    time_t endEpoch = static_cast<time_t>(localRangeEnd -
        (rigaDaylightSaving(endCandidate) ? 3UL : 2UL) * 3600UL);
    struct tm startUtc = {};
    struct tm endUtc = {};
    if (gmtime_r(&startEpoch, &startUtc) == nullptr) {
        Serial.println("[FORECAST] Не удалось преобразовать время для запроса прогноза.");
        return false;
    }
    if (gmtime_r(&endEpoch, &endUtc) == nullptr) {
        Serial.println("[FORECAST] Не удалось преобразовать конец периода прогноза.");
        return false;
    }
    char startDate[32];
    char endDate[32];
    snprintf(startDate, sizeof(startDate), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
        startUtc.tm_year + 1900, startUtc.tm_mon + 1, startUtc.tm_mday,
        startUtc.tm_hour, startUtc.tm_min, startUtc.tm_sec);
    snprintf(endDate, sizeof(endDate), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
        endUtc.tm_year + 1900, endUtc.tm_mon + 1, endUtc.tm_mday,
        endUtc.tm_hour, endUtc.tm_min, endUtc.tm_sec);
    String forecastPath = String("/api/nps/price?start=") + startDate + "&end=" + endDate;

    Serial.println("[FORECAST] Запрос почасового плана цен Elering...");
    if (!client.connect(serverHost, serverPort)) {
        Serial.println("[ERROR] Не удалось установить TLS-соединение для прогноза.");
        client.stop();
        return false;
    }
    client.print("GET ");
    client.print(forecastPath);
    client.println(" HTTP/1.0");
    client.print("Host: ");
    client.println(serverHost);
    client.println("User-Agent: ArduinoOpta-PLC/1.0");
    client.println("Accept: application/json");
    client.println("Connection: close");
    client.println();

    String response;
    if (!readHttpResponseBody(client, response, 40000)) {
        client.stop();
        return false;
    }
    client.stop();
    response.trim();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    if (error) {
        Serial.print("[ERROR] Ошибка разбора прогноза Elering: ");
        Serial.println(error.c_str());
        return false;
    }
    if (!(doc["success"] | false) || !doc["data"]["lv"].is<JsonArray>()) {
        Serial.println("[ERROR] В ответе Elering отсутствует массив цен LV.");
        return false;
    }

    forecastSlotCount = 0;
    JsonArray prices = doc["data"]["lv"].as<JsonArray>();
    for (JsonObject item : prices) {
        if (forecastSlotCount >= MAX_FORECAST_SLOTS) break;
        uint32_t timestamp = item["timestamp"].as<uint32_t>();
        float price = item["price"].as<float>();
        if (timestamp == 0 || !isfinite(price)) continue;
        forecastSlots[forecastSlotCount++] = {timestamp, price / 1000.0f};
    }
    if (forecastSlotCount == 0) {
        Serial.println("[ERROR] Elering вернул пустой прогноз цен LV.");
        return false;
    }
    Serial.print("[FORECAST] Получено квартальных значений: ");
    Serial.println(forecastSlotCount);
    return true;
}

size_t buildPriceHours(PriceHour* hours, size_t capacity) {
    size_t count = 0;
    for (size_t i = 0; i < forecastSlotCount; ++i) {
        uint32_t hourTimestamp = forecastSlots[i].timestamp / 3600UL * 3600UL;
        if (count == 0 || hours[count - 1].timestamp != hourTimestamp) {
            if (count >= capacity) break;
            hours[count++] = {hourTimestamp, forecastSlots[i].priceEurKwh, 1};
        } else {
            PriceHour& hour = hours[count - 1];
            hour.averagePrice += forecastSlots[i].priceEurKwh;
            ++hour.quarterCount;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        hours[i].averagePrice /= hours[i].quarterCount;
    }
    return count;
}

void localHourAndDay(uint32_t epoch, uint8_t& hour, int64_t& day) {
    uint32_t localEpoch = epoch + (rigaDaylightSaving(epoch) ? 3UL : 2UL) * 3600UL;
    day = localEpoch / 86400UL;
    hour = static_cast<uint8_t>((localEpoch % 86400UL) / 3600UL);
}

int64_t nightWindowKey(uint32_t timestamp, const DeviceConfig& config) {
    uint8_t hour;
    int64_t day;
    localHourAndDay(timestamp, hour, day);
    uint16_t minuteOfDay = static_cast<uint16_t>(hour * 60);
    uint16_t start = config.nightStartMinuteOfDay;
    uint16_t end = config.nightEndMinuteOfDay;
    if (start < end) {
        return minuteOfDay >= start && minuteOfDay + 60 <= end ? day : -1;
    }
    if (minuteOfDay >= start && minuteOfDay + 60 <= 1440) return day;
    if (minuteOfDay + 60 <= end) return day - 1;
    return -1;
}

bool chooseCheapestNightHours(const DeviceConfig& config, const PriceHour* hours, size_t count, bool& selectedNow) {
    uint32_t currentHour = getCurrentEpochSeconds() / 3600UL * 3600UL;
    if (config.nightStartMinuteOfDay == config.nightEndMinuteOfDay) return false;
    int64_t nearestWindow = INT64_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (hours[i].timestamp + 3600UL <= currentHour || hours[i].quarterCount != 4) continue;
        int64_t key = nightWindowKey(hours[i].timestamp, config);
        if (key >= 0 && key < nearestWindow) nearestWindow = key;
    }
    if (nearestWindow == INT64_MAX) return false;

    bool chosen[MAX_FORECAST_SLOTS] = {};
    for (uint8_t n = 0; n < config.nightCheapHours; ++n) {
        size_t cheapest = count;
        for (size_t i = 0; i < count; ++i) {
            if (hours[i].timestamp + 3600UL <= currentHour ||
                hours[i].quarterCount != 4 ||
                nightWindowKey(hours[i].timestamp, config) != nearestWindow || chosen[i]) continue;
            if (cheapest == count || hours[i].averagePrice < hours[cheapest].averagePrice) {
                cheapest = i;
            }
        }
        if (cheapest == count) return false;
        chosen[cheapest] = true;
        if (hours[cheapest].timestamp == currentHour) selectedNow = true;
    }
    return true;
}

bool chooseCheapestDayHours(const DeviceConfig& config, const PriceHour* hours, size_t count, bool& selectedNow) {
    int64_t currentDay;
    uint32_t currentHour = getCurrentEpochSeconds() / 3600UL * 3600UL;
    uint8_t currentHourOfDay;
    localHourAndDay(getCurrentEpochSeconds(), currentHourOfDay, currentDay);
    size_t availableHours = 0;
    for (size_t i = 0; i < count; ++i) {
        uint8_t hourOfDay;
        int64_t day;
        localHourAndDay(hours[i].timestamp, hourOfDay, day);
        if (day == currentDay && hours[i].quarterCount == 4) {
            ++availableHours;
        }
    }
    if (availableHours == 0) return false;

    size_t hoursToSelect = config.smartThresholdHours;
    if (hoursToSelect > availableHours) {
        Serial.print("[WARN] Для умного порога запрошено часов: ");
        Serial.print(hoursToSelect);
        Serial.print(", полных часов прогноза на сегодня доступно: ");
        Serial.print(availableHours);
        Serial.println("; выбраны все доступные часы.");
        hoursToSelect = availableHours;
    }

    bool chosen[MAX_FORECAST_SLOTS] = {};
    for (size_t n = 0; n < hoursToSelect; ++n) {
        size_t cheapest = count;
        for (size_t i = 0; i < count; ++i) {
            uint8_t hourOfDay;
            int64_t day;
            localHourAndDay(hours[i].timestamp, hourOfDay, day);
            if (day != currentDay || hours[i].quarterCount != 4 || chosen[i]) continue;
            if (cheapest == count || hours[i].averagePrice < hours[cheapest].averagePrice) {
                cheapest = i;
            }
        }
        if (cheapest == count) return false;
        chosen[cheapest] = true;
        if (hours[cheapest].timestamp == currentHour) selectedNow = true;
    }
    return true;
}

bool getNextWakeupEpoch(uint16_t wakeupMinute, uint32_t& wakeupEpoch) {
    uint32_t now = getCurrentEpochSeconds();
    uint8_t currentHour;
    int64_t currentDay;
    localHourAndDay(now, currentHour, currentDay);
    for (int dayOffset = 0; dayOffset < 3; ++dayOffset) {
        int year;
        unsigned month;
        unsigned day;
        civilFromDays(currentDay + dayOffset, year, month, day);
        char localDateTime[48];
        snprintf(localDateTime, sizeof(localDateTime), "%04d-%02u-%02uT%02u:%02u",
            year, month, day, wakeupMinute / 60, wakeupMinute % 60);
        uint32_t candidate = 0;
        if (parseRigaDateTime(String(localDateTime), candidate) && candidate > now) {
            wakeupEpoch = candidate;
            return true;
        }
    }
    return false;
}

bool isWakeupHeatingTime(const DeviceConfig& config, bool& selectedNow) {
    uint32_t wakeupEpoch;
    if (!getNextWakeupEpoch(config.wakeupTimeMinuteOfDay, wakeupEpoch)) return false;
    uint32_t endHour = wakeupEpoch / 3600UL * 3600UL;
    uint32_t startHour = endHour - static_cast<uint32_t>(config.wakeupHours) * 3600UL;
    uint32_t currentHour = getCurrentEpochSeconds() / 3600UL * 3600UL;
    selectedNow = currentHour >= startHour && currentHour < endHour;
    return true;
}

bool chooseConsecutiveHours(uint8_t relayIndex, const DeviceConfig& config, const PriceHour* hours, size_t count, bool& selectedNow) {
    size_t duration = config.consecutiveBlockHours;
    uint32_t currentHour = getCurrentEpochSeconds() / 3600UL * 3600UL;
    if (lockedConsecutiveBlockEnd[relayIndex] > currentHour) {
        selectedNow = lockedConsecutiveBlockStart[relayIndex] <= currentHour;
        return true;
    }
    lockedConsecutiveBlockStart[relayIndex] = 0;
    lockedConsecutiveBlockEnd[relayIndex] = 0;
    if (duration > count) return false;
    float lowestCost = INFINITY;
    size_t bestStart = count;
    for (size_t start = 0; start + duration <= count; ++start) {
        float cost = 0.0f;
        bool valid = true;
        for (size_t offset = 0; offset < duration; ++offset) {
            size_t index = start + offset;
            if (hours[index].quarterCount != 4 ||
                hours[index].timestamp < currentHour ||
                (offset > 0 && hours[index].timestamp != hours[index - 1].timestamp + 3600UL)) {
                valid = false;
                break;
            }
            cost += hours[index].averagePrice;
        }
        if (valid && cost < lowestCost) {
            lowestCost = cost;
            bestStart = start;
        }
    }
    if (bestStart == count) return false;
    lockedConsecutiveBlockStart[relayIndex] = hours[bestStart].timestamp;
    lockedConsecutiveBlockEnd[relayIndex] = hours[bestStart + duration - 1].timestamp + 3600UL;
    selectedNow = lockedConsecutiveBlockStart[relayIndex] <= currentHour;
    return true;
}

bool chooseHolidayHours(const DeviceConfig& config, const PriceHour* hours, size_t count, bool& selectedNow) {
    uint32_t now = getCurrentEpochSeconds();
    uint32_t currentHour = now / 3600UL * 3600UL;
    uint32_t returnTime = config.holidayReturnEpoch;
    if (returnTime <= now || count == 0 ||
        hours[count - 1].timestamp + 3600UL < returnTime) {
        return false;
    }
    bool chosen[MAX_FORECAST_SLOTS] = {};
    for (uint8_t n = 0; n < config.holidayHeatHours; ++n) {
        size_t cheapest = count;
        for (size_t i = 0; i < count; ++i) {
            if (hours[i].timestamp < currentHour ||
                hours[i].timestamp + 3600UL > returnTime ||
                hours[i].quarterCount != 4 || chosen[i]) continue;
            if (cheapest == count || hours[i].averagePrice < hours[cheapest].averagePrice) {
                cheapest = i;
            }
        }
        if (cheapest == count) return false;
        chosen[cheapest] = true;
        if (hours[cheapest].timestamp == currentHour) selectedNow = true;
    }
    return true;
}

bool getAutomaticRelayDecision(uint8_t relayIndex, float currentPrice, bool forecastAvailable, bool& relayOn) {
    DeviceConfig& config = relayConfigs[relayIndex];
    if (manualOverrides[relayIndex]) return false;
    if (currentPrice > config.safetyCeilingEurKwh) {
        Serial.println("[DECISION] Сработал аварийный предел цены; реле выключено.");
        relayOn = false;
        return true;
    }
    if (config.holidayEnabled &&
        config.holidayReturnEpoch <= getCurrentEpochSeconds()) {
        config.holidayEnabled = false;
        if (!saveDeviceConfigs(relayConfigs)) {
            Serial.println("[ERROR] Не удалось сохранить завершение режима отпуска.");
        }
        Serial.println("[HOLIDAY] Дата возвращения наступила; режим отпуска завершен.");
    }
    if (config.holidayEnabled) {
        if (forecastAvailable) {
            PriceHour hours[MAX_FORECAST_SLOTS];
            size_t count = buildPriceHours(hours, MAX_FORECAST_SLOTS);
            bool selectedNow = false;
            if (chooseHolidayHours(config, hours, count, selectedNow)) {
                relayOn = selectedNow;
                Serial.println("[DECISION] Сценарий отпуска: включение только в выбранные дешевые часы.");
            } else {
                relayOn = false;
                Serial.println("[WARN] Прогноз не покрывает период до возвращения; нагрев выключен.");
            }
        } else {
            relayOn = false;
            Serial.println("[WARN] Прогноз недоступен в режиме отпуска; нагрев выключен.");
        }
        return true;
    }

    bool selectedByScenario = currentPrice <= config.alwaysOnBelowPriceEurKwh;

    PriceHour hours[MAX_FORECAST_SLOTS];
    size_t hourCount = 0;
    if (forecastAvailable &&
        (config.smartThresholdEnabled || config.nightPrepEnabled ||
         config.consecutiveBlockEnabled)) {
        hourCount = buildPriceHours(hours, MAX_FORECAST_SLOTS);
    }

    if (config.smartThresholdEnabled) {
        bool selectedNow = false;
        bool planAvailable = forecastAvailable &&
            chooseCheapestDayHours(config, hours, hourCount, selectedNow);
        selectedByScenario = selectedByScenario || (planAvailable && selectedNow);
        if (!forecastAvailable) {
            Serial.println("[WARN] Прогноз недоступен для умного порога; остальные сценарии продолжают работать.");
        } else if (!planAvailable) {
            Serial.println("[WARN] Не удалось построить план умного порога на сегодня.");
        } else {
            Serial.println("[DECISION] Решение по сценарию «Умный порог включения».");
        }
    }
    if (config.wakeupEnabled) {
        bool selectedNow = false;
        if (!isWakeupHeatingTime(config, selectedNow)) {
            Serial.println("[WARN] Не удалось вычислить интервал нагрева перед пробуждением.");
        } else {
            selectedByScenario = selectedByScenario || selectedNow;
            Serial.println("[DECISION] Решение по сценарию «Включение перед пробуждением».");
        }
    }
    if (config.nightPrepEnabled) {
        bool selectedNow = false;
        bool planAvailable = forecastAvailable &&
            chooseCheapestNightHours(config, hours, hourCount, selectedNow);
        selectedByScenario = selectedByScenario || (planAvailable && selectedNow);
        if (!forecastAvailable) {
            Serial.println("[WARN] Прогноз недоступен для включения ночью; остальные сценарии продолжают работать.");
        } else if (!planAvailable) {
            Serial.println("[WARN] Не удалось построить план включения ночью.");
        } else {
            Serial.println("[DECISION] Решение по сценарию «Включение ночью».");
        }
    }
    if (config.consecutiveBlockEnabled) {
        bool selectedNow = false;
        bool planAvailable = forecastAvailable &&
            chooseConsecutiveHours(relayIndex, config, hours, hourCount, selectedNow);
        selectedByScenario = selectedByScenario || (planAvailable && selectedNow);
        if (!forecastAvailable) {
            Serial.println("[WARN] Прогноз недоступен для непрерывного нагрева; остальные сценарии продолжают работать.");
        } else if (!planAvailable) {
            Serial.println("[WARN] Не удалось построить план непрерывного нагрева.");
        } else {
            Serial.println("[DECISION] Решение по сценарию «Непрерывный нагрев».");
        }
    }
    relayOn = selectedByScenario;
    return true;
}

void buildRelayWorkSlotsForChart(uint8_t relayIndex, uint32_t chartLocalDay,
    bool workSlots[MAX_FORECAST_SLOTS]) {
    if (relayIndex >= RELAY_COUNT || forecastSlotCount == 0) return;

    static PriceHour hours[MAX_FORECAST_SLOTS];
    static bool chosen[MAX_FORECAST_SLOTS];
    memset(workSlots, 0, MAX_FORECAST_SLOTS * sizeof(workSlots[0]));
    const DeviceConfig& config = relayConfigs[relayIndex];
    uint32_t now = timeSynchronized ? getCurrentEpochSeconds() : 0;
    uint32_t currentHour = now / 3600UL * 3600UL;
    size_t hourCount = buildPriceHours(hours, MAX_FORECAST_SLOTS);

    if (manualOverrides[relayIndex]) {
        if (relayStates[relayIndex]) {
            for (size_t i = 0; i < forecastSlotCount; ++i) {
                uint32_t timestamp = forecastSlots[i].timestamp;
                uint32_t localTimestamp = timestamp +
                    (rigaDaylightSaving(timestamp) ? 3UL : 2UL) * 3600UL;
                workSlots[i] = localTimestamp / 86400UL == chartLocalDay;
            }
        }
        return;
    }

    auto markHour = [&](uint32_t hourTimestamp) {
        for (size_t slot = 0; slot < forecastSlotCount; ++slot) {
            uint32_t timestamp = forecastSlots[slot].timestamp;
            if (timestamp >= hourTimestamp && timestamp < hourTimestamp + 3600UL) {
                uint32_t localTimestamp = timestamp +
                    (rigaDaylightSaving(timestamp) ? 3UL : 2UL) * 3600UL;
                if (localTimestamp / 86400UL == chartLocalDay) {
                    workSlots[slot] = true;
                }
            }
        }
    };

    bool holidayActive = config.holidayEnabled &&
        (!timeSynchronized || config.holidayReturnEpoch > now);
    if (holidayActive) {
        if (!timeSynchronized) return;
        memset(chosen, 0, sizeof(chosen));
        for (uint8_t n = 0; n < config.holidayHeatHours; ++n) {
            size_t cheapest = hourCount;
            for (size_t i = 0; i < hourCount; ++i) {
                if (hours[i].timestamp < currentHour ||
                    hours[i].timestamp + 3600UL > config.holidayReturnEpoch ||
                    hours[i].quarterCount != 4 || chosen[i]) {
                    continue;
                }
                if (cheapest == hourCount ||
                    hours[i].averagePrice < hours[cheapest].averagePrice) {
                    cheapest = i;
                }
            }
            if (cheapest == hourCount) break;
            chosen[cheapest] = true;
            markHour(hours[cheapest].timestamp);
        }
    } else {
        for (size_t i = 0; i < forecastSlotCount; ++i) {
            uint32_t timestamp = forecastSlots[i].timestamp;
            uint32_t localTimestamp = timestamp +
                (rigaDaylightSaving(timestamp) ? 3UL : 2UL) * 3600UL;
            if (localTimestamp / 86400UL == chartLocalDay &&
                forecastSlots[i].priceEurKwh <= config.alwaysOnBelowPriceEurKwh) {
                workSlots[i] = true;
            }
        }

        if (config.smartThresholdEnabled) {
            memset(chosen, 0, sizeof(chosen));
            for (uint8_t n = 0; n < config.smartThresholdHours; ++n) {
                size_t cheapest = hourCount;
                for (size_t i = 0; i < hourCount; ++i) {
                    uint8_t hourOfDay;
                    int64_t day;
                    localHourAndDay(hours[i].timestamp, hourOfDay, day);
                    if (day != chartLocalDay || hours[i].quarterCount != 4 ||
                        chosen[i]) {
                        continue;
                    }
                    if (cheapest == hourCount ||
                        hours[i].averagePrice < hours[cheapest].averagePrice) {
                        cheapest = i;
                    }
                }
                if (cheapest == hourCount) break;
                chosen[cheapest] = true;
                markHour(hours[cheapest].timestamp);
            }
        }

        if (config.wakeupEnabled && timeSynchronized) {
            uint32_t wakeupEpoch;
            if (getNextWakeupEpoch(config.wakeupTimeMinuteOfDay, wakeupEpoch)) {
                uint32_t endHour = wakeupEpoch / 3600UL * 3600UL;
                uint32_t startHour = endHour -
                    static_cast<uint32_t>(config.wakeupHours) * 3600UL;
                for (size_t i = 0; i < forecastSlotCount; ++i) {
                    uint32_t timestamp = forecastSlots[i].timestamp;
                    uint32_t localTimestamp = timestamp +
                        (rigaDaylightSaving(timestamp) ? 3UL : 2UL) * 3600UL;
                    if (timestamp >= startHour && timestamp < endHour &&
                        localTimestamp / 86400UL == chartLocalDay) {
                        workSlots[i] = true;
                    }
                }
            }
        }

        if (config.nightPrepEnabled) {
            int64_t nearestWindow = INT64_MAX;
            for (size_t i = 0; i < hourCount; ++i) {
                if (hours[i].timestamp < currentHour || hours[i].quarterCount != 4) {
                    continue;
                }
                uint8_t hourOfDay;
                int64_t day;
                localHourAndDay(hours[i].timestamp, hourOfDay, day);
                if (day != chartLocalDay) continue;
                int64_t key = nightWindowKey(hours[i].timestamp, config);
                if (key >= 0 && key < nearestWindow) nearestWindow = key;
            }
            if (nearestWindow != INT64_MAX) {
                memset(chosen, 0, sizeof(chosen));
                for (uint8_t n = 0; n < config.nightCheapHours; ++n) {
                    size_t cheapest = hourCount;
                    for (size_t i = 0; i < hourCount; ++i) {
                        if (hours[i].timestamp < currentHour ||
                            hours[i].quarterCount != 4 || chosen[i] ||
                            nightWindowKey(hours[i].timestamp, config) != nearestWindow) {
                            continue;
                        }
                        uint8_t hourOfDay;
                        int64_t day;
                        localHourAndDay(hours[i].timestamp, hourOfDay, day);
                        if (day != chartLocalDay) continue;
                        if (cheapest == hourCount ||
                            hours[i].averagePrice < hours[cheapest].averagePrice) {
                            cheapest = i;
                        }
                    }
                    if (cheapest == hourCount) break;
                    chosen[cheapest] = true;
                    markHour(hours[cheapest].timestamp);
                }
            }
        }

        if (config.consecutiveBlockEnabled) {
            uint32_t blockStart = lockedConsecutiveBlockStart[relayIndex];
            uint32_t blockEnd = lockedConsecutiveBlockEnd[relayIndex];
            if (blockEnd <= currentHour) {
                blockStart = 0;
                blockEnd = 0;
                size_t duration = config.consecutiveBlockHours;
                float lowestCost = INFINITY;
                size_t bestStart = hourCount;
                for (size_t start = 0; start + duration <= hourCount; ++start) {
                    float cost = 0.0f;
                    bool valid = true;
                    for (size_t offset = 0; offset < duration; ++offset) {
                        size_t index = start + offset;
                        if (hours[index].quarterCount != 4 ||
                            hours[index].timestamp < currentHour ||
                            (offset > 0 && hours[index].timestamp !=
                                hours[index - 1].timestamp + 3600UL)) {
                            valid = false;
                            break;
                        }
                        cost += hours[index].averagePrice;
                    }
                    if (valid && cost < lowestCost) {
                        lowestCost = cost;
                        bestStart = start;
                    }
                }
                if (bestStart < hourCount) {
                    blockStart = hours[bestStart].timestamp;
                    blockEnd = hours[bestStart + duration - 1].timestamp + 3600UL;
                }
            }
            if (blockEnd > blockStart) {
                for (size_t i = 0; i < forecastSlotCount; ++i) {
                    uint32_t timestamp = forecastSlots[i].timestamp;
                    uint32_t localTimestamp = timestamp +
                        (rigaDaylightSaving(timestamp) ? 3UL : 2UL) * 3600UL;
                    if (timestamp >= blockStart && timestamp < blockEnd &&
                        localTimestamp / 86400UL == chartLocalDay) {
                        workSlots[i] = true;
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < forecastSlotCount; ++i) {
        if (forecastSlots[i].priceEurKwh > config.safetyCeilingEurKwh) {
            workSlots[i] = false;
        }
    }
}

void applyCurrentPriceToConfiguredControl(uint8_t relayIndex) {
    if (!hasLatestPrice) return;
    const DeviceConfig& config = relayConfigs[relayIndex];
    if (manualOverrides[relayIndex]) return;
    bool requiresForecast = config.smartThresholdEnabled ||
        config.nightPrepEnabled || config.consecutiveBlockEnabled ||
        config.holidayEnabled;
    bool forecastAvailable = !requiresForecast || fetchPriceForecast();
    bool relayDecision = relayStates[relayIndex];
    if (getAutomaticRelayDecision(relayIndex, latestPriceEurKwh, forecastAvailable, relayDecision)) {
        setRelay(relayIndex, relayDecision);
    }
}

void serviceScheduledControl() {
    if (!timeSynchronized || !hasLatestPrice) return;
    static uint32_t lastEvaluatedMinute = 0;
    uint32_t currentMinute = getCurrentEpochSeconds() / 60UL;
    if (currentMinute == lastEvaluatedMinute) return;
    lastEvaluatedMinute = currentMinute;

    for (uint8_t relayIndex = 0; relayIndex < RELAY_COUNT; ++relayIndex) {
        const DeviceConfig& config = relayConfigs[relayIndex];
        if (!config.holidayEnabled && !config.smartThresholdEnabled &&
            !config.wakeupEnabled && !config.nightPrepEnabled &&
            !config.consecutiveBlockEnabled) {
            continue;
        }
        bool relayDecision = relayStates[relayIndex];
        if (getAutomaticRelayDecision(relayIndex, latestPriceEurKwh, forecastSlotCount > 0, relayDecision) &&
            relayDecision != relayStates[relayIndex]) {
            Serial.print("[SCHEDULE] Переключение реле ");
            Serial.print(relayIndex + 1);
            Serial.println(" по сохраненному плану цен.");
            setRelay(relayIndex, relayDecision);
        }
    }
}

void servicePriceSchedule() {
    unsigned long nowMillis = millis();
    if (!timeSynchronized) {
        if (!hasAttemptedTimeSync ||
            nowMillis - lastTimeSyncAttemptMillis >= TIME_SYNC_RETRY_INTERVAL_MS) {
            syncNetworkTime();
        }
        return;
    }

    if (nowMillis - lastTimeSyncAttemptMillis >= TIME_RESYNC_INTERVAL_MS) {
        syncNetworkTime();
    }

    uint32_t nowEpoch = getCurrentEpochSeconds();
    uint32_t intervalNumber = nowEpoch / SECONDS_PER_PRICE_INTERVAL;
    uint32_t secondsIntoInterval = nowEpoch % SECONDS_PER_PRICE_INTERVAL;

    if (secondsIntoInterval < 60 &&
        (!hasScheduledPriceInterval || intervalNumber != lastScheduledPriceInterval)) {
        lastScheduledPriceInterval = intervalNumber;
        hasScheduledPriceInterval = true;
        awaitingNewPriceTimestamp = true;
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
            manualOverrides[i] = false;
        }
        lastUpdateMillis = millis() - PRICE_RETRY_INTERVAL_MS;
        Serial.println("[SCHEDULE] Начинается проверка цены на границе 15-минутного интервала.");
    }

    if (awaitingNewPriceTimestamp &&
        (!hasAttemptedPriceUpdate ||
         millis() - lastUpdateMillis >= PRICE_RETRY_INTERVAL_MS)) {
        updatePriceAndRelay();
    }
    serviceScheduledControl();
}

// =============================================================================
// ФУНКЦИЯ УПРАВЛЕНИЯ РЕЛЕ
// =============================================================================
void setRelay(uint8_t relayIndex, bool state) {
    if (relayIndex >= RELAY_COUNT) return;
    relayStates[relayIndex] = state;
    digitalWrite(RELAY_PINS[relayIndex], state ? HIGH : LOW);
    digitalWrite(RELAY_LEDS[relayIndex], state ? HIGH : LOW);
    
    Serial.print("[RELAY] Реле ");
    Serial.print(relayIndex + 1);
    Serial.print(": ");
    Serial.println(state ? "ВКЛЮЧЕНО" : "ВЫКЛЮЧЕНО");
}

// =============================================================================
// ЗАПРОС И ПАРСИНГ ЦЕНЫ NORDPOOL ДЛЯ ЛАТВИИ
// =============================================================================
bool fetchNordPoolPrice(float &currentPriceEurKwh, long &priceTimestamp) {
    Serial.print("[HTTP] Подключение к ");
    Serial.print(serverHost);
    Serial.println("...");

    if (!client.connect(serverHost, serverPort)) {
        Serial.println("[ERROR] Не удалось установить защищенное соединение (SSL)!");
        client.stop();
        return false;
    }

    Serial.println("[HTTP] Отправка GET-запроса...");
    client.print("GET ");
    client.print(apiPath);
    client.println(" HTTP/1.0");
    client.print("Host: ");
    client.println(serverHost);
    client.println("User-Agent: ArduinoOpta-PLC/1.0");
    client.println("Accept: application/json");
    client.println("Connection: close");
    client.println();

    String jsonBody;
    if (!readHttpResponseBody(client, jsonBody, 4096)) {
        client.stop();
        return false;
    }
    client.stop();

    jsonBody.trim();

    // Извлечение чистого JSON (от первого '{' до последнего '}')
    int startIdx = jsonBody.indexOf('{');
    int endIdx   = jsonBody.lastIndexOf('}');
    if (startIdx >= 0 && endIdx >= startIdx) {
        jsonBody = jsonBody.substring(startIdx, endIdx + 1);
    }

    Serial.print("[HTTP] Ответ сервера: ");
    Serial.println(jsonBody);

    // Разбор JSON с помощью ArduinoJson
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonBody);

    if (error) {
        Serial.print("[ERROR] Ошибка парсинга JSON: ");
        Serial.println(error.c_str());
        return false;
    }

    bool success = doc["success"] | false;
    if (!success || !doc["data"].is<JsonArray>() || doc["data"].size() == 0) {
        Serial.println("[ERROR] Некорректный формат данных в ответе API!");
        return false;
    }

    // Без явной проверки отсутствующее поле превращается в 0 и может включить реле
    JsonVariant priceValue = doc["data"][0]["price"];
    JsonVariant timestampValue = doc["data"][0]["timestamp"];
    if (!priceValue.is<float>() || !timestampValue.is<long>()) {
        Serial.println("[ERROR] В ответе API нет числовой цены или timestamp!");
        return false;
    }
    float priceEurMwh = priceValue.as<float>();
    long timestamp = timestampValue.as<long>();
    // Гармонизированные пределы рынка: от -500 до 4000 EUR/MWh, берем с запасом
    if (!isfinite(priceEurMwh) || priceEurMwh < -1000.0f || priceEurMwh > 10000.0f) {
        Serial.print("[ERROR] Цена вне допустимого диапазона: ");
        Serial.println(priceEurMwh);
        return false;
    }
    if (timestamp < 1735689600L) {
        Serial.print("[ERROR] Некорректный timestamp цены: ");
        Serial.println(timestamp);
        return false;
    }
    if (timeSynchronized) {
        uint32_t nowEpoch = getCurrentEpochSeconds();
        uint32_t priceEpoch = static_cast<uint32_t>(timestamp);
        if (priceEpoch > nowEpoch + 300UL || nowEpoch - priceEpoch >= PRICE_MAX_AGE_SECONDS) {
            Serial.print("[ERROR] Цена не относится к текущему времени (timestamp ");
            Serial.print(timestamp);
            Serial.print(", сейчас ");
            Serial.print(nowEpoch);
            Serial.println(").");
            return false;
        }
    }

    currentPriceEurKwh = priceEurMwh / 1000.0f;
    priceTimestamp = timestamp;

    Serial.print("[NORDPOOL-LV] Цена из API: ");
    Serial.print(currentPriceEurKwh, 5);
    Serial.print(" EUR/kWh, timestamp: ");
    Serial.println(priceTimestamp);

    return true;
}

// =============================================================================
// ОБНОВЛЕНИЕ ЦЕНЫ И ПРИНЯТИЕ РЕШЕНИЯ ДЛЯ РЕЛЕ
// =============================================================================
void updatePriceAndRelay() {
    lastUpdateMillis = millis();
    hasAttemptedPriceUpdate = true;
    Serial.println("\n--------------------------------------------------");
    Serial.println("[TASK] Запрос котировок NordPool (Латвия)...");

    float currentPrice = 0.0f;
    long currentTimestamp = 0;
    if (fetchNordPoolPrice(currentPrice, currentTimestamp)) {
        if (currentTimestamp <= latestPriceTimestamp) {
            awaitingNewPriceTimestamp = true;
            Serial.print("[WARN] API пока возвращает устаревшую цену (timestamp ");
            Serial.print(currentTimestamp);
            Serial.print(", ожидается значение больше ");
            Serial.print(latestPriceTimestamp);
            Serial.println("). Повторный запрос через 30 секунд; реле не изменяется.");
            Serial.println("--------------------------------------------------");
            return;
        }

        hasLatestPrice = true;
        latestPriceEurKwh = currentPrice;
        latestPriceTimestamp = currentTimestamp;
        latestPriceReceivedMillis = millis();
        awaitingNewPriceTimestamp = false;

        bool forecastAvailable = fetchPriceForecast();
        for (uint8_t relayIndex = 0; relayIndex < RELAY_COUNT; ++relayIndex) {
            if (manualOverrides[relayIndex]) {
                Serial.print("[DECISION] Реле ");
                Serial.print(relayIndex + 1);
                Serial.println(": активен ручной режим, остальные условия не применяются.");
                continue;
            }
            bool nextRelayState = relayStates[relayIndex];
            getAutomaticRelayDecision(relayIndex, currentPrice, forecastAvailable, nextRelayState);
            Serial.print("[DECISION] Цена ");
            Serial.print(currentPrice, 5);
            Serial.print(" EUR/kWh, реле ");
            Serial.print(relayIndex + 1);
            Serial.println(nextRelayState ? " ВКЛЮЧАЕТСЯ." : " ВЫКЛЮЧАЕТСЯ.");
            setRelay(relayIndex, nextRelayState);
        }
    } else {
        awaitingNewPriceTimestamp = true;
        Serial.println("[WARN] Не удалось получить цену. Реле сохраняет предыдущее состояние.");
        Serial.println("[TASK] Повторный запрос через 30 секунд.");
    }
    Serial.println("--------------------------------------------------");
}

// =============================================================================
// АВАРИЙНОЕ ОТКЛЮЧЕНИЕ ПРИ ОТСУТСТВИИ ЦЕНЫ
// =============================================================================
// Вызывается в начале каждого loop(), в том числе без Wi-Fi и в режиме точки
// настройки. Реле в ручном режиме не трогаем: это явное решение пользователя.
void servicePriceFailsafe() {
    if (!hasLatestPrice ||
        millis() - latestPriceReceivedMillis < PRICE_FAILSAFE_TIMEOUT_MS) {
        return;
    }
    hasLatestPrice = false;
    awaitingNewPriceTimestamp = true;
    Serial.println("[FAILSAFE] Новой цены нет больше часа; автоматические реле выключаются.");
    for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
        if (!manualOverrides[i] && relayStates[i]) {
            setRelay(i, false);
        }
    }
}

// =============================================================================
// ПОДРОБНЫЙ ОТЧЕТ О СТАТУСЕ В СЕРИЙНЫЙ МОНИТОР
// =============================================================================
const char* relayModeName(uint8_t relayIndex) {
    const DeviceConfig& config = relayConfigs[relayIndex];
    if (manualOverrides[relayIndex]) return "Ручной";
    if (!hasLatestPrice) {
        return latestPriceTimestamp != 0
            ? "Нет актуальной цены: реле выключено"
            : "Ожидание первой цены";
    }
    if (latestPriceEurKwh > config.safetyCeilingEurKwh) {
        return "Аварийное отключение по верхнему пределу";
    }
    if (config.holidayEnabled &&
        (!timeSynchronized || config.holidayReturnEpoch > getCurrentEpochSeconds())) {
        return "Отпуск";
    }
    if (latestPriceEurKwh <= config.alwaysOnBelowPriceEurKwh) {
        return "Порог включения";
    }
    if (config.smartThresholdEnabled) return "Умный порог включения";
    if (config.wakeupEnabled) return "Включение перед пробуждением";
    if (config.nightPrepEnabled) return "Включение ночью";
    if (config.consecutiveBlockEnabled) return "Непрерывный нагрев";
    return "По цене";
}

void printStatusReport() {
    Serial.println("\n==================================================");
    Serial.println("       СТАТУС ARDUINO OPTA PLC: NORDPOOL          ");
    Serial.println("==================================================");

#ifdef USE_WIFI
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("  [Сеть] Wi-Fi:      ПОДКЛЮЧЕН к \"");
        Serial.print(WiFi.SSID());
        Serial.println("\"");
        Serial.print("  [Сеть] IP-адрес:   ");
        Serial.println(WiFi.localIP());
        Serial.print("  [Сеть] Сигнал:     ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
    } else {
        Serial.println("  [Сеть] Wi-Fi:      ОТКЛЮЧЕН / ПОИСК СЕТИ");
    }
#else
    Serial.print("  [Сеть] Ethernet:   ");
    Serial.println(Ethernet.localIP());
#endif

    Serial.println("  [Реле] Состояния:");
    for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
        Serial.print("    Реле ");
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.println(relayStates[i] ? "ВКЛЮЧЕНО" : "ВЫКЛЮЧЕНО");
    }

    Serial.print("  [Биржа] Цена LV:   ");
    if (hasLatestPrice) {
        Serial.print(latestPriceEurKwh, 5);
        Serial.println(" EUR/kWh");
    } else if (latestPriceTimestamp != 0) {
        Serial.println("нет актуальной цены больше часа; автоматические реле выключены");
    } else {
        Serial.println("Ожидание первого запроса...");
    }

    Serial.print("  [Детали реле ");
    Serial.print(selectedRelayIndex + 1);
    Serial.print("] Режим: ");
    Serial.println(relayModeName(selectedRelayIndex));
    Serial.print("  [Детали реле ");
    Serial.print(selectedRelayIndex + 1);
    Serial.print("] Порог включения: ");
    Serial.print(relayConfigs[selectedRelayIndex].alwaysOnBelowPriceEurKwh, 5);
    Serial.println(" EUR/kWh");
    Serial.print("  [Детали реле ");
    Serial.print(selectedRelayIndex + 1);
    Serial.print("] Аварийный предел: ");
    Serial.print(relayConfigs[selectedRelayIndex].safetyCeilingEurKwh, 5);
    Serial.println(" EUR/kWh");

    if (awaitingNewPriceTimestamp && hasAttemptedPriceUpdate) {
        unsigned long elapsed = millis() - lastUpdateMillis;
        unsigned long remSec = elapsed >= PRICE_RETRY_INTERVAL_MS
            ? 0
            : (PRICE_RETRY_INTERVAL_MS - elapsed) / 1000;
        Serial.print("  [Таймер]:          Повтор цены через ");
        Serial.print(remSec);
        Serial.println(" сек");
    } else if (timeSynchronized) {
        uint32_t secondsToNextInterval =
            SECONDS_PER_PRICE_INTERVAL -
            (getCurrentEpochSeconds() % SECONDS_PER_PRICE_INTERVAL);
        Serial.print("  [Таймер]:          Следующая плановая проверка через ");
        Serial.print(secondsToNextInterval / 60);
        Serial.print(" мин ");
        Serial.print(secondsToNextInterval % 60);
        Serial.println(" сек");
    } else {
        Serial.println("  [Таймер]:          Ожидание синхронизации времени NTP");
    }

    Serial.println("--------------------------------------------------");
    Serial.print("  Дополнительные детали: выбрано реле ");
    Serial.println(selectedRelayIndex + 1);
    Serial.println("  Команды: 'status [1-4]' (или '?'), 'update', 'on [1-4]', 'off [1-4]', 'auto [1-4]'");
    Serial.println("==================================================\n");
}

// =============================================================================
// ОБРАБОТКА КОМАНД ИЗ СЕРИЙНОГО МОНИТОРА
// =============================================================================
void handleSerialCommands() {
    if (Serial.available() > 0) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();
        uint8_t commandRelay = selectedRelayIndex;
        int argumentStart = cmd.indexOf(' ');
        if (argumentStart >= 0) {
            String relayArgument = cmd.substring(argumentStart + 1);
            relayArgument.trim();
            int relayNumber = relayArgument.toInt();
            if (relayNumber >= 1 && relayNumber <= RELAY_COUNT) {
                commandRelay = static_cast<uint8_t>(relayNumber - 1);
                selectedRelayIndex = commandRelay;
            }
            cmd = cmd.substring(0, argumentStart);
            cmd.trim();
        }

        if (cmd == "status" || cmd == "?" || cmd == "info" || cmd == "") {
            printStatusReport();
        } else if (cmd == "update" || cmd == "price") {
            Serial.println("\n[CMD] Принудительный запрос актуальных котировок...");
            updatePriceAndRelay();
            printStatusReport();
        } else if (cmd == "on") {
            manualOverrides[commandRelay] = true;
            setRelay(commandRelay, true);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" принудительно ВКЛЮЧЕНО (ручной режим).");
        } else if (cmd == "off") {
            manualOverrides[commandRelay] = true;
            setRelay(commandRelay, false);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" принудительно ВЫКЛЮЧЕНО (ручной режим).");
        } else if (cmd == "auto") {
            manualOverrides[commandRelay] = false;
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" возвращено в автоматический режим.");
            updatePriceAndRelay();
        } else {
            Serial.print("\n[CMD] Неизвестная команда: \"");
            Serial.print(cmd);
            Serial.println("\". Доступные команды: status [1-4], update, on [1-4], off [1-4], auto [1-4]");
        }
    }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);

    // Ожидание открытия монитора порта до 3 секунд
    unsigned long startSerial = millis();
    while (!Serial && (millis() - startSerial < 3000)) {
        delay(10);
    }
    if (!loadDeviceConfigs()) {
        Serial.println("[WARN] KVStore недоступен; используются настройки по умолчанию из RAM.");
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
            setDefaultDeviceConfig(relayConfigs[i]);
        }
    }
    
    // Инициализация пинов реле, кнопки и светодиодов
    for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
        pinMode(RELAY_PINS[i], OUTPUT);
        pinMode(RELAY_LEDS[i], OUTPUT);
        setRelay(i, false);
    }
    pinMode(LED_USER, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Serial.println("\n==================================================");
    Serial.println("  Arduino Opta PLC: NordPool Latvia Relay Controller");
    Serial.println("==================================================");

#ifdef USE_WIFI
    Serial.println("[Ethernet] Проверка проводного подключения...");
    configureEthernetHostname();
    serviceWiredNetwork(true);

    if (WiFi.status() == WL_NO_SHIELD) {
        Serial.println("[ERROR] Модуль Wi-Fi не обнаружен на плате!");
    } else {
        WifiCredentials credentials = {};
        bool credentialsValid = loadWifiCredentials(credentials);
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
            if (wiredServerStarted) {
                Serial.println("[WEB] Используется Ethernet-сервер, чтобы сохранить ресурсы сетевого стека.");
                Serial.print("[WEB] Настройки доступны по адресу http://");
                Serial.println(Ethernet.localIP());
            } else {
                startSettingsWebServer();
            }
            syncNetworkTime();
            updatePriceAndRelay();
            printStatusReport();
        } else {
            Serial.println("\n[WiFi] Переход в режим конфигурации...");
            serviceWiredNetwork();
            if (wiredServerStarted) {
                wifiCredentialsVisible = true;
                Serial.print("[WEB] Точка доступа не запускается: настройки доступны по LAN адресу http://");
                Serial.println(Ethernet.localIP());
            } else if (startWifiManager()) {
                Serial.println("[WiFi] Открыт портал настройки.");
            }
        }
    }
#else
    Serial.println("[Ethernet] Подключение по кабелю RJ45 (DHCP)...");
    configureEthernetHostname();
    serviceWiredNetwork(true);
    if (Ethernet.localIP()[0] == 0) {
        Serial.println("[ERROR] Не удалось получить IP по DHCP от Ethernet!");
    } else {
        Serial.println("[Ethernet] Успешно подключено!");
        Serial.print("[Ethernet] IP-адрес: ");
        Serial.println(Ethernet.localIP());
        syncNetworkTime();
        updatePriceAndRelay();
        printStatusReport();
    }
#endif
    startHardwareWatchdog();
}

void startHardwareWatchdog() {
    mbed::Watchdog& watchdog = mbed::Watchdog::get_instance();
    if (!watchdog.is_running() && !watchdog.start()) {
        Serial.println("[ERROR] Не удалось запустить аппаратный watchdog.");
        return;
    }

    Serial.print("[WATCHDOG] Аппаратный watchdog активен, таймаут ");
    Serial.print(watchdog.get_timeout());
    Serial.println(" мс.");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
    mbed::Watchdog::get_instance().kick();
    // До любых ранних return: должно работать и без сети, и в режиме точки настройки
    servicePriceFailsafe();
    unsigned long currentMillis = millis();

    // -------------------------------------------------------------------------
    // 1. Обработка команд из Serial Monitor ('status', 'update', 'on', 'off')
    // -------------------------------------------------------------------------
    handleSerialCommands();

    // -------------------------------------------------------------------------
    // 2. Опрос кнопки на корпусе Opta (BTN_USER) для ручного управления
    // -------------------------------------------------------------------------
    static int lastReading = HIGH;
    static int buttonState = HIGH;
    static unsigned long lastDebounceTime = 0;
    static unsigned long buttonPressedAt = 0;
    static bool buttonPressActive = false;
    static bool wifiPortalRequestedByButton = false;
    const unsigned long debounceDelay = 50; // 50 мс антидребезг

    int reading = digitalRead(BUTTON_PIN);
    if (reading != lastReading) {
        lastDebounceTime = currentMillis;
    }

    if ((currentMillis - lastDebounceTime) > debounceDelay) {
        if (reading != buttonState) {
            buttonState = reading;
            if (buttonState == LOW) {
#ifdef USE_WIFI
                buttonPressedAt = currentMillis;
                buttonPressActive = true;
                wifiPortalRequestedByButton = false;
#else
                manualOverrides[0] = true;
                bool newState = !relayStates[0];
                Serial.println("\n[BUTTON] ==========================================");
                Serial.print("[BUTTON] Нажата кнопка USER! Ручное переключение Реле 1: ");
                Serial.println(newState ? "ВКЛЮЧЕНО" : "ВЫКЛЮЧЕНО");
                setRelay(0, newState);
                Serial.println("[BUTTON] (Реле переведено в РУЧНОЙ режим до следующего цикла)");
                Serial.println("[BUTTON] ==========================================\n");
#endif
            } else {
#ifdef USE_WIFI
                if (buttonPressActive && !wifiPortalRequestedByButton) {
                    manualOverrides[0] = true;
                    bool newState = !relayStates[0];
                    Serial.println("\n[BUTTON] Короткое нажатие USER: ручное переключение Реле 1.");
                    setRelay(0, newState);
                }
                buttonPressActive = false;
#endif
            }
        }
    }
    lastReading = reading;

#ifdef USE_WIFI
    if (buttonPressActive && !wifiPortalRequestedByButton &&
        currentMillis - buttonPressedAt >= 5000UL) {
        wifiPortalRequestedByButton = true;
        if (wiredServerStarted) {
            wifiCredentialsVisible = true;
            Serial.println("\n[WiFi] Настройки Wi-Fi доступны через Ethernet; точка доступа не запускается.");
        } else {
            Serial.println("\n[WiFi] USER удерживается 5 секунд; запуск режима настройки Wi-Fi.");
        }
        if (!wiredServerStarted && !startWifiManager()) {
            Serial.println("[ERROR] Не удалось запустить точку настройки Wi-Fi по кнопке USER.");
        }
    }
#endif

    // -------------------------------------------------------------------------
    // 3. Индикация Wi-Fi светодиодом LED_USER:
    //    - Медленное мигание (1.5 сек): Wi-Fi подключен
    //    - Быстрое мигание (200 мс): нет подключения
    // -------------------------------------------------------------------------
    bool isConnected = false;
#ifdef USE_WIFI
    isConnected = (WiFi.status() == WL_CONNECTED);
#else
    isConnected = (Ethernet.localIP()[0] != 0);
#endif

    static unsigned long lastBlink = 0;
    unsigned long blinkInterval = isConnected ? 1500 : 200;
    if (currentMillis - lastBlink > blinkInterval) {
        lastBlink = currentMillis;
        digitalWrite(LED_USER, !digitalRead(LED_USER));
    }

#ifdef USE_WIFI
    static unsigned long lastReconnectAttempt = 0;
    static uint8_t reconnectAttempts = 0;
    serviceWiredNetwork();
    // Автоматическое переподключение к Wi-Fi при потере связи
    if (portalMode) {
        serviceWifiManager();
        servicePortalReconnect();
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        serviceMDNS();
        serviceSettingsWebServer();
    }
    serviceWiredWebServer();

    if (WiFi.status() != WL_CONNECTED && WiFi.status() != WL_NO_SHIELD) {
        if (currentMillis - lastReconnectAttempt > 10000) {
            lastReconnectAttempt = currentMillis;
            reconnectAttempts++;
            Serial.println("[WiFi] Соединение потеряно, повторное подключение...");
            WiFi.disconnect();
            WifiCredentials credentials = {};
            if (loadWifiCredentials(credentials)) {
                // На случай, если до этого не запустилась точка доступа со статическим IP
                WiFi.config(INADDR_NONE);
                WiFi.setHostname(DEVICE_HOSTNAME);
                WiFi.begin(credentials.ssid, credentials.password);
            } else if (!wiredServerStarted) {
                startWifiManager();
            } else {
                wifiCredentialsVisible = true;
            }
            if (reconnectAttempts >= 3 && WiFi.status() != WL_CONNECTED && !wiredServerStarted) {
                startWifiManager();
            }
        }
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        reconnectAttempts = 0;
    }
#endif

    // -------------------------------------------------------------------------
    // 4. Периодический краткий статус в Serial Monitor (каждые 10 секунд)
    // -------------------------------------------------------------------------
    if (currentMillis - lastHeartbeatMillis >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMillis = currentMillis;
        Serial.print("[STATUS] Реле: ");
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
            if (i > 0) Serial.print(", ");
            Serial.print(i + 1);
            Serial.print(" ");
            Serial.print(relayStates[i] ? "ВКЛ" : "ВЫКЛ");
        }
        Serial.print(" | Цена: ");
        if (hasLatestPrice) {
            Serial.print(latestPriceEurKwh, 5);
            Serial.print(" EUR/kWh");
        } else {
            Serial.print("нет данных");
        }
#ifdef USE_WIFI
        Serial.print(" | Wi-Fi: ");
        if (WiFi.status() == WL_CONNECTED) {
            Serial.print("есть (");
            Serial.print(WiFi.RSSI());
            Serial.print(" dBm)");
        } else {
            Serial.print("нет");
        }
#else
        Serial.print(" | Wi-Fi: не используется");
#endif
        Serial.println();
        Serial.println("Напишите \"STATUS\" для дополнительной информации.");
    }

    // -------------------------------------------------------------------------
    // 5. Опрос по реальному времени: на каждой четверти часа, затем каждые 30 секунд
    //    до появления новой цены в API
    // -------------------------------------------------------------------------
    servicePriceSchedule();
}
