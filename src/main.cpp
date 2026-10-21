// Прошивка Arduino Opta: управление 4 реле по цене Nord Pool LV (API Elering).
// Перед изменениями прочитайте CLAUDE.md и docs/: логика реле — docs/SCENARIOS.md,
// известные баги — docs/KNOWN_ISSUES.md, план разбиения этого файла на модули —
// docs/ARCHITECTURE.md. Чистая логика (время, цены, настройки, планировщик, разбор
// API, формы и DNS) и состояние каналов (lib/controller: цена, failsafe, ручной режим,
// принудительное включение) вынесены в lib/ и покрыты тестами: pio test -e native.
// Здесь остались сеть, веб-страница, консоль и сборка объектов.
#include <Arduino.h>
#include <drivers/Watchdog.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <Ethernet.h>

#include <ChannelConfig.h>
#include <ConfigStore.h>
#include <Controller.h>
#include <DnsPacket.h>
#include <Elering.h>
#include <ForceOnTimer.h>
#include <HttpParse.h>
#include <Planner.h>
#include <PriceSeries.h>
#include <RigaTime.h>
#include <SwitchHistory.h>
#include <WifiCredentialStore.h>

#include "platform/KvStorage.h"
#include "platform/Log.h"
#include "platform/RelayOutputBank.h"
#include "platform/StatusLed.h"
#include "platform/SystemClock.h"
#include "platform/UserButton.h"

using channelconfig::ChannelConfig;
using channelconfig::kChannelCount;
using configstore::WifiCredentials;

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
const char* serverHost = elering::kHost;
const int   serverPort = elering::kPort;
const char* apiPath    = elering::kCurrentPath;

const unsigned long TIME_SYNC_RETRY_INTERVAL_MS = 60UL * 1000UL;
const unsigned long TIME_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
// В режиме точки настройки повторять подключение к сохраненной сети
const unsigned long PORTAL_RECONNECT_INTERVAL_MS = 5UL * 60UL * 1000UL;
// Не прерывать точку настройки, пока пользователь недавно открывал страницу
const unsigned long PORTAL_ACTIVITY_GRACE_MS = 2UL * 60UL * 1000UL;
const unsigned long HTTP_REQUEST_TIMEOUT_MS = 3000UL;
const size_t HTTP_MAX_LINE_LENGTH = 512;
const uint8_t HTTP_MAX_HEADER_COUNT = 32;
bool dnsUdpStarted = false;
bool portalMode = false;
unsigned long lastPortalReconnectMillis = 0;
unsigned long lastPortalActivityMillis = 0;
bool wifiCredentialsVisible = false;
const char* CONFIG_AP_SSID = "Opta-Config-AP";
const char* CONFIG_AP_PASSWORD = "opta1234";
const char* DEVICE_HOSTNAME = "opta-settings";
const IPAddress MDNS_MULTICAST_IP(224, 0, 0, 251);
const IPAddress CONFIG_AP_IP(192, 168, 4, 1);

// Платформа (L1): flash, выходы, кнопка USER, светодиод LED_USER
KvStorage kvStorage;
configstore::ConfigStore configStore(kvStorage);
configstore::WifiCredentialStore wifiCredentialStore(kvStorage);
RelayOutputBank relayOutputs;
UserButton userButton;
StatusLed statusLed;
SerialLogSink serialLog;

// История переключений до перезапуска (D-019)
history::SwitchHistory switchHistory;

// События с запуска до первой синхронизации получают время задним числом
void fillHistoryEpochs(uint32_t epoch, uint32_t uptimeSeconds) {
    switchHistory.fillMissingEpochs(epoch, uptimeSeconds);
}
SystemClock systemClock(ntpUdp, fillHistoryEpochs);

bool fetchNordPoolPrice(float &currentPriceEurKwh, long &priceTimestamp);
bool fetchPrices(const char* path, prices::PriceSeries& out);

// Цены Elering по TLS: синхронные запросы до шага 2g (KI-006)
class EleringPriceSource : public controller::PriceSource {
public:
    bool fetchCurrent(float& eurKwh, long& timestamp) override {
        return fetchNordPoolPrice(eurKwh, timestamp);
    }
    bool fetchPrices(const char* path, prices::PriceSeries& out) override {
        return ::fetchPrices(path, out);
    }
};
EleringPriceSource eleringPrices;

// Владелец состояния каналов: настройки, ручной режим, принудительное включение,
// цена, прогноз, решения автоматики (lib/controller)
controller::Controller channelController(systemClock, relayOutputs, configStore,
    eleringPrices, switchHistory, serialLog);

uint8_t selectedRelayIndex = 0;
// -1 вчера, 0 сегодня, 1 завтра
int8_t selectedChartDay = 0;
bool settingsServerStarted = false;
bool mdnsUdpStarted = false;
bool wiredServerStarted = false;
bool wiredMdnsStarted = false;
bool wiredDhcpAttempted = false;
unsigned long nextEthernetAttemptMillis = 0;
unsigned long nextWiredServerAttemptMillis = 0;
IPAddress wiredServerAddress;
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

// Объявления функций
bool syncNetworkTime();
void servicePriceSchedule();
template <typename Client>
void sendHistoryPage(Client& client);
bool startWifiManager();
void serviceWifiManager();
void handleDnsRequests();
bool loadDeviceConfigs(ChannelConfig configs[kChannelCount]);
bool startSettingsWebServer();
void serviceSettingsWebServer();
void serviceWiredWebServer();
void serviceMDNS();
void startHardwareWatchdog();
void printStatusReport();
void handleSerialCommands();
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
    return systemClock.sync();
}

// Повтор NTP раз в минуту до первой синхронизации, затем раз в 6 часов. Расписание
// цены идет только при синхронизированных часах.
void servicePriceSchedule() {
    unsigned long nowMillis = millis();
    if (!systemClock.synced()) {
        if (!systemClock.hasAttemptedSync() ||
            nowMillis - systemClock.lastSyncAttemptMs() >= TIME_SYNC_RETRY_INTERVAL_MS) {
            syncNetworkTime();
        }
        return;
    }

    if (nowMillis - systemClock.lastSyncAttemptMs() >= TIME_RESYNC_INTERVAL_MS) {
        syncNetworkTime();
    }
    channelController.servicePriceSchedule();
}

void printTimeOfDay(Print& out, uint16_t minuteOfDay) {
    char text[8];
    rigatime::formatTimeOfDay(minuteOfDay, text, sizeof(text));
    out.print(text);
}

// Загрузка настроек каналов из flash с сообщениями о каждом исходе.
// false — настройки в RAM не гарантированы, вызывающий ставит значения по умолчанию.
bool loadDeviceConfigs(ChannelConfig configs[kChannelCount]) {
    using configstore::LoadOutcome;
    const configstore::LoadReport report = configStore.load(configs);
    switch (report.outcome) {
        case LoadOutcome::Created:
            logLine("CONFIG", "Конфигурация не найдена (KVStore: %d); сохранены настройки по умолчанию для всех реле.",
                report.kvResult);
            break;
        case LoadOutcome::CreateFailed:
            logLine("ERROR", "Не удалось сохранить настройки реле по умолчанию в KVStore.");
            break;
        case LoadOutcome::UnknownSize:
        case LoadOutcome::UnknownSizeSaveFailed:
            logLine("ERROR", "Размер конфигурации реле не распознан; загружены настройки по умолчанию.");
            if (report.outcome == LoadOutcome::UnknownSizeSaveFailed) {
                logLine("ERROR", "Не удалось записать настройки по умолчанию для всех реле.");
            }
            break;
        case LoadOutcome::LegacyMultiReadFailed:
            logLine("ERROR", "Не удалось прочитать старые настройки четырех реле.");
            break;
        case LoadOutcome::CurrentReadFailed:
            logLine("ERROR", "Не удалось прочитать конфигурации всех реле.");
            break;
        case LoadOutcome::Corrupt:
        case LoadOutcome::CorruptSaveFailed:
            logLine("ERROR", "Старая конфигурация реле повреждена.");
            if (report.outcome == LoadOutcome::CorruptSaveFailed) {
                logLine("ERROR", "Не удалось заменить поврежденную конфигурацию настройками по умолчанию.");
            }
            break;
        case LoadOutcome::Loaded:
        case LoadOutcome::Migrated:
        case LoadOutcome::MigratedSaveFailed:
            for (uint8_t i = 0; i < kChannelCount; ++i) {
                if ((report.invalidMask & (1U << i)) != 0) {
                    logLine("ERROR", "Некорректная конфигурация реле %u", i + 1U);
                }
            }
            if (report.outcome == LoadOutcome::Migrated) {
                logLine("CONFIG", "Конфигурации обновлены до формата для четырех реле.");
            } else if (report.outcome == LoadOutcome::MigratedSaveFailed) {
                logLine("ERROR", "Не удалось сохранить обновленные конфигурации реле.");
            }
            break;
    }
    return configstore::loadSucceeded(report.outcome);
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

// Данные графика для скрипта страницы: цены вчера–завтра и план автоматики
// выбранного реле (тот же buildPlan, что и для решения, KI-005). Рисует браузер.
// Отдельного запроса картинки нет: сервер обслуживает одно соединение за раз, и
// параллельный запрос /chart.svg сбрасывался.
//   n  — сейчас, epoch UTC (0 — время не синхронизировано); d — выбранные сутки
//   c  — текущая цена, ц/кВт·ч; t0 — начало первого интервала
//   t  — номер интервала от t0; p — цена, ц/кВт·ч
//   r  — бит 16: план включает реле; младшие 4 бита — planner::Reason
//   rn — названия причин по номеру planner::Reason
//   ro — причины, которые выключают реле (отпуск без интервала, аварийный предел)
//   f  — конец принудительного включения выбранного реле, epoch UTC (0 — не идет)
template <typename Out>
void printChartData(Out& client) {
    const uint32_t now = systemClock.synced() ? systemClock.nowUtc() : 0;
    const planner::ChannelPlan& channelPlan = channelController.chartPlan(selectedRelayIndex, now);
    const prices::PriceSeries& forecast = channelController.forecast();
    const uint32_t first = forecast.size() > 0 ? forecast[0].start : 0;
    client.print(F("<script>var CHART={n:"));
    client.print(now);
    client.print(F(",d:"));
    client.print(static_cast<int>(selectedChartDay));
    client.print(F(",c:"));
    if (channelController.hasLatestPrice()) {
        client.print(channelController.latestPriceEurKwh() * 100.0f, 2);
    } else {
        client.print(F("null"));
    }
    client.print(F(",t0:"));
    client.print(first);
    client.print(F(",t:["));
    for (size_t i = 0; i < forecast.size(); ++i) {
        if (i > 0) client.print(',');
        client.print((forecast[i].start - first) / prices::kSlotSeconds);
    }
    client.print(F("],p:["));
    for (size_t i = 0; i < forecast.size(); ++i) {
        if (i > 0) client.print(',');
        client.print(forecast[i].eurKwh * 100.0f, 2);
    }
    client.print(F("],r:["));
    for (size_t i = 0; i < forecast.size(); ++i) {
        if (i > 0) client.print(',');
        int code = 0;
        if (i < channelPlan.slotCount) {
            const uint8_t mask = channelPlan.mask[i];
            code = static_cast<int>(planner::slotReason(mask)) | (planner::slotOn(mask) ? 16 : 0);
        }
        client.print(code);
    }
    client.print(F("],rn:["));
    for (uint8_t reason = 0; reason <= static_cast<uint8_t>(planner::Reason::Ceiling); ++reason) {
        if (reason > 0) client.print(',');
        client.print('"');
        client.print(planner::reasonName(static_cast<planner::Reason>(reason)));
        client.print('"');
    }
    client.print(F("],ro:["));
    client.print(static_cast<int>(planner::Reason::HolidayOff));
    client.print(',');
    client.print(static_cast<int>(planner::Reason::Ceiling));
    client.print(F("],f:"));
    const uint32_t forceLeftMs = channelController.forceOnRemainingMs(selectedRelayIndex);
    client.print(now != 0 && forceLeftMs > 0 ? now + (forceLeftMs + 999UL) / 1000UL : 0UL);
    client.print(F("};</script>"));
}

// Отрисовка графика в браузере: SVG по данным CHART. Время — по правилам Риги
// (переходы в последнее воскресенье марта/октября в 01:00 UTC), независимо от часового
// пояса браузера. Сутки 23 и 25 часов рисуются по реальной длине.
static const char kChartScript[] = R"js(<script>(function(){
var C=window.CHART,el=document.getElementById('chart'),tip=document.getElementById('chart-tip');
if(!C||!el)return;
if(!C.n){el.textContent='Время еще не синхронизировано, графика пока нет.';return;}
function p2(v){return(v<10?'0':'')+v;}
function ls(y,m){var d=new Date(Date.UTC(y,m+1,0));return d.getUTCDate()-d.getUTCDay();}
function of(e){var y=new Date(e*1e3).getUTCFullYear(),a=Date.UTC(y,2,ls(y,2),1)/1e3,b=Date.UTC(y,9,ls(y,9),1)/1e3;return e>=a&&e<b?10800:7200;}
function lc(e){return e+of(e);}
function ds(d){var e=d*86400-7200;return lc(e)==d*86400?e:d*86400-10800;}
function hm(e){var s=lc(e)%86400;return p2(Math.floor(s/3600))+':'+p2(Math.floor(s%3600/60));}
function fmt(v){return v.toFixed(2).replace('.',',');}
var S=[];for(var i=0;i<C.t.length;i++)S.push({s:C.t0+C.t[i]*900,p:C.p[i],r:C.r[i]});
var today=Math.floor(lc(C.n)/86400),day=C.d,W=700,H=300,L=56,T=22,PW=626,PH=220;
function draw(){
var a=ds(today+day),b=ds(today+day+1),q=S.filter(function(x){return x.s>=a&&x.s<b;});
var lo=0,hi=10;q.forEach(function(x){lo=Math.min(lo,x.p);hi=Math.max(hi,x.p);});
lo=Math.floor(lo/5)*5;hi=Math.ceil(hi/5)*5;var st=hi-lo>40?10:5,i,j,x,y;
function X(e){return L+(e-a)/(b-a)*PW;}
function Y(v){return T+PH-(v-lo)/(hi-lo)*PH;}
var h='<svg class="price-graph" viewBox="0 0 '+W+' '+H+'" role="img" aria-label="График цены и плана реле">';
for(i=0;i<q.length;i++){if(!(q[i].r&16))continue;j=i;while(j+1<q.length&&(q[j+1].r&16)&&q[j+1].s==q[j].s+900)j++;
h+='<rect class="work-period" x="'+X(q[i].s).toFixed(1)+'" y="'+T+'" width="'+(X(q[j].s+900)-X(q[i].s)+0.4).toFixed(1)+'" height="'+PH+'"/>';i=j;}
if(C.f>C.n&&C.f>a&&C.n<b){x=X(Math.max(C.n,a));h+='<rect class="force-period" x="'+x.toFixed(1)+'" y="'+T+'" width="'+(X(Math.min(C.f,b))-x).toFixed(1)+'" height="'+PH+'"/>';}
for(var v=lo;v<=hi;v+=st){y=Y(v).toFixed(1);h+='<line class="'+(v==0?'zero-line':'grid-line')+'" x1="'+L+'" x2="'+(L+PW)+'" y1="'+y+'" y2="'+y+'"/><text class="axis-label" x="'+(L-8)+'" y="'+(+y+4)+'" text-anchor="end">'+v+'</text>';}
for(var e=a,k=0;e<=b;e+=3600,k++){if(k%2)continue;x=X(e).toFixed(1);
h+='<line class="grid-line" x1="'+x+'" x2="'+x+'" y1="'+T+'" y2="'+(T+PH)+'"/><text class="axis-label" x="'+x+'" y="'+(T+PH+18)+'" text-anchor="middle">'+(e==b?'24:00':hm(e))+'</text>';}
if(q.length){var d='';for(i=0;i<q.length;i++){y=Y(q[i].p).toFixed(1);d+=(i&&q[i].s==q[i-1].s+900?'L':'M')+X(q[i].s).toFixed(1)+','+y+'L'+X(q[i].s+900).toFixed(1)+','+y;}
h+='<path class="price-line" d="'+d+'"/>';}
else h+='<text class="axis-title" x="'+(L+PW/2)+'" y="'+(T+PH/2)+'" text-anchor="middle">Цен на эти сутки пока нет</text>';
if(C.n>=a&&C.n<b){x=X(C.n).toFixed(1);h+='<line class="now-line" x1="'+x+'" x2="'+x+'" y1="'+T+'" y2="'+(T+PH)+'"/><text class="now-label" x="'+x+'" y="14" text-anchor="middle">Сейчас</text>';
if(C.c!==null)h+='<circle class="current-price" cx="'+x+'" cy="'+Y(Math.max(lo,Math.min(hi,C.c))).toFixed(1)+'" r="5"/>';}
h+='<text class="axis-title" x="'+(L+PW/2)+'" y="'+(H-6)+'" text-anchor="middle">Местное время (Europe/Riga)</text>';
h+='<text class="axis-title" transform="translate(14 '+(T+PH/2)+') rotate(-90)" text-anchor="middle">Цена, центов/кВт·ч</text>';
h+='<line id="cur" class="hover-line" y1="'+T+'" y2="'+(T+PH)+'" visibility="hidden"/><rect id="hit" x="'+L+'" y="'+T+'" width="'+PW+'" height="'+PH+'" fill="transparent"/></svg>';
el.innerHTML=h;tip.style.display='none';
var svg=el.firstChild,cur=document.getElementById('cur');
function hide(){tip.style.display='none';cur.setAttribute('visibility','hidden');}
function show(ev){var r=svg.getBoundingClientRect(),pt=ev.touches?ev.touches[0]:ev,sx=(pt.clientX-r.left)/r.width*W,t=a+(sx-L)/PW*(b-a),f=null;
for(var n=0;n<q.length;n++)if(t>=q[n].s&&t<q[n].s+900){f=q[n];break;}
if(!f){hide();return;}
var rs=f.r&15,txt=hm(f.s)+'–'+hm(f.s+900)+' · '+fmt(f.p)+' ц/кВт·ч · ';
if(t>=C.n&&t<C.f)txt+='включено: принудительное включение';
else txt+=f.r&16?'включено: '+C.rn[rs]:(C.ro.indexOf(rs)>=0?'выключено: '+C.rn[rs]:'выключено');
tip.textContent=txt;tip.style.display='block';var cx=X(f.s+450);cur.setAttribute('x1',cx);cur.setAttribute('x2',cx);cur.setAttribute('visibility','visible');
var px=cx/W*r.width-tip.offsetWidth/2;tip.style.left=Math.max(0,Math.min(r.width-tip.offsetWidth,px))+'px';}
var hit=document.getElementById('hit');hit.addEventListener('mousemove',show);hit.addEventListener('mouseleave',hide);hit.addEventListener('touchstart',show,{passive:true});
[].forEach.call(document.querySelectorAll('#days button'),function(bt){bt.className=+bt.getAttribute('data-d')==day?'active':'';});}
[].forEach.call(document.querySelectorAll('#days button'),function(bt){bt.onclick=function(){day=+bt.getAttribute('data-d');draw();};});
draw();})();</script>)js";

// Состояние выбранного реле и кнопки принудительного включения (docs/SCENARIOS.md §6).
// Отдельная форма POST /force: не отправляет и не проверяет поля настроек.
template <typename Out>
void printForceOnPanel(Out& client) {
    const uint8_t relay = selectedRelayIndex;
    const bool forceOnActive = channelController.forceOnActive(relay);
    client.print(F("<form method='POST' action='/force'><input type='hidden' name='relay' value='"));
    client.print(relay + 1);
    client.print(F("'><fieldset class='force-box'><legend>Принудительное включение</legend><p>Реле "));
    client.print(relay + 1);
    client.print(F(" сейчас: <strong>"));
    client.print(channelController.outputOn(relay) ? F("включено") : F("выключено"));
    client.print(F("</strong>. Режим: "));
    client.print(channelController.modeName(relay));
    client.print(F(".</p>"));
    if (forceOnActive) {
        char remaining[24];
        forceon::formatRemaining(channelController.forceOnRemainingMs(relay), remaining,
            sizeof(remaining));
        client.print(F("<p>Включено принудительно, осталось <strong>"));
        client.print(remaining);
        client.print(F("</strong>.</p>"));
    }
    client.print(F("<div class='form-actions'><button type='submit' name='action' value='start'>"));
    client.print(forceOnActive ? F("Перезапустить на ") : F("Включить на "));
    client.print(channelController.config(relay).forceOnHours);
    client.print(F(" ч</button><button type='submit' name='action' value='stop' class='secondary'"));
    if (!forceOnActive) client.print(F(" disabled"));
    client.print(F(">Остановить</button></div><small>Реле включается сразу и работает "
        "заданное время, не глядя на цену, аварийный предел, отпуск и наличие интернета, "
        "затем возвращается к автоматике. Ручное включение или выключение отменяет его. "
        "После перезапуска устройства не возобновляется.</small></fieldset></form>"));
}

template <typename Client>
void sendPortalPage(Client& networkClient, const String& message) {
    const ChannelConfig& deviceConfig = channelController.config(selectedRelayIndex);
    String page;
    if (!page.reserve(24000)) {
        Serial.println("[WEB] Недостаточно памяти для формирования страницы настроек.");
        networkClient.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n"));
        return;
    }
    StringPageWriter client(page);

    client.print(F("<!doctype html><html lang='ru'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Opta Settings</title>"
        // Пустая иконка: иначе браузер параллельно просит /favicon.ico, а сервер
        // обслуживает одно соединение за раз
        "<link rel='icon' href='data:,'>"
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
        ".work-period{fill:#d1d5db;fill-opacity:.7}.force-period{fill:#f9ab00;fill-opacity:.35}"
        "button.secondary{background:#5f6368}button:disabled{opacity:.45}"
        ".force-box{margin:0 0 12px}.force-box p{margin:6px 0}"
        ".now-line{stroke:#d93025;stroke-width:2;stroke-dasharray:5 4}"
        ".now-label{font-size:12px;fill:#d93025}"
        ".current-price{fill:#d93025;stroke:white;stroke-width:2}.chart-note{margin:0;color:#5f6368}"
        ".zero-line{stroke:#9ca3af;stroke-width:1}.hover-line{stroke:#5f6368;stroke-dasharray:3 3}"
        ".chart-box{position:relative;min-height:120px}"
        ".chart-tip{display:none;position:absolute;top:0;max-width:100%;box-sizing:border-box;"
        "background:#202124;color:white;padding:4px 8px;border-radius:4px;font-size:13px;"
        "pointer-events:none}"
        ".day-switch{display:flex;gap:8px;margin:8px 0 12px}.day-switch button{margin:0;padding:8px 14px;"
        "background:white;border:1px solid #1769aa;border-radius:5px;color:#1769aa}"
        ".day-switch button.active{background:#1769aa;color:white}"
        ".nav{display:flex;gap:16px;margin:0 0 16px}.nav a{color:#1769aa}"
        ".form-actions{display:flex;gap:10px;flex-wrap:wrap}"
        "@media(max-width:640px){.settings-grid{grid-template-columns:1fr}"
        ".price-chart,.form-actions{grid-column:auto}}</style>"
        "</head><body><h1>Настройки Opta</h1><p>Устройство: "
        "<a href='http://opta-settings.local'>http://opta-settings.local</a></p>"
        "<nav class='nav'><strong>Настройки</strong>"
        "<a href='/history'>История переключений</a></nav>"
        "<form method='GET' action='/'><label>Настраиваемое реле"
        "<select name='relay' onchange='this.form.submit()'>"));

    for (uint8_t i = 0; i < kChannelCount; ++i) {
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

    client.print(F("</select></label></form>"));
    printForceOnPanel(client);
    client.print(F("<form method='POST' action='/save'><input type='hidden' name='relay' value='"));
    client.print(selectedRelayIndex + 1);
    client.print(F("'><div class='settings-grid'><fieldset class='price-chart'><legend>График цены</legend>"
        "<div class='day-switch' id='days'><button type='button' data-d='-1'>Вчера</button>"
        "<button type='button' data-d='0'>Сегодня</button>"
        "<button type='button' data-d='1'>Завтра</button></div>"
        "<div class='chart-box'><div id='chart'><noscript>Для графика нужен JavaScript."
        "</noscript></div><div id='chart-tip' class='chart-tip'></div></div>"
        "<p class='chart-note'>Синяя линия — цены по 15-минутным интервалам. Серым отмечен "
        "план автоматики выбранного реле, оранжевым — оставшееся время принудительного "
        "включения; ручной режим на графике не показывается. "
        "Наведите на график или коснитесь его, чтобы увидеть цену и причину. Вчерашний "
        "план пересчитан по текущим настройкам, фактические переключения — на странице "
        "<a href='/history'>История переключений</a>.</p>"));
    printChartData(client);
    client.print(kChartScript);

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
    client.print(F("'></label><small>N часов — это 4·N самых дешевых 15-минутных интервалов "
        "текущих суток, не обязательно подряд.</small></fieldset>"
        "<fieldset><legend>Включение перед пробуждением</legend>"
        "<label><input type='checkbox' name='wakeup_enabled' value='1'"));
    if (deviceConfig.wakeupEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Количество часов до пробуждения"
        "<input name='wakeup_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.wakeupHours);
    client.print(F("'></label><label>Время пробуждения"
        "<input name='wakeup_time' type='time' value='"));
    printTimeOfDay(client, deviceConfig.wakeupTimeMinuteOfDay);
    client.print(F("'></label><small>Реле включено ровно N часов до этого времени, "
        "без учета цены.</small></fieldset>"
        "<fieldset><legend>Включение ночью</legend><label><input type='checkbox' name='night_enabled' value='1'"));
    if (deviceConfig.nightPrepEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><div class='row'><label>Начало окна"
        "<input name='night_start' type='time' value='"));
    printTimeOfDay(client, deviceConfig.nightStartMinuteOfDay);
    client.print(F("'></label><label>Конец окна<input name='night_end' type='time' value='"));
    printTimeOfDay(client, deviceConfig.nightEndMinuteOfDay);
    client.print(F("'></label></div><label>Количество самых дешевых часов<input name='night_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.nightCheapHours);
    client.print(F("'></label><small>4·N самых дешевых 15-минутных интервалов окна. "
        "Если интервал ближе к концу окна дороже не больше чем на 1 цент/кВт·ч, "
        "выбирается он.</small></fieldset>"
        "<fieldset><legend>Принудительное включение</legend><label>Длительность, часов"
        "<input name='force_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.forceOnHours);
    client.print(F("'></label><small>На сколько часов кнопка «Включить» над настройками "
        "включает реле. Уже идущее включение не меняется.</small></fieldset>"
        "<fieldset><legend>Календарь / отпуск</legend><label><input type='checkbox' name='holiday_enabled' value='1'"));
    if (deviceConfig.holidayEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Дата и время возвращения (Europe/Riga)"
        "<input name='return_local' type='datetime-local' value='"));
    if (deviceConfig.holidayReturnEpoch != 0) {
        char returnText[17];
        rigatime::formatLocalDateTime(deviceConfig.holidayReturnEpoch, returnText, sizeof(returnText));
        client.print(returnText);
    }
    client.print(F("'></label><label>Часы работы перед возвращением"
        "<input name='holiday_hours' type='number' min='1' max='10' value='"));
    client.print(deviceConfig.holidayOnHours);
    client.print(F("'></label><small>Реле работает в N·4 самых дешевых 15-минутных "
        "интервалах последних 10 часов перед возвращением, ближе к возвращению при разнице "
        "до 1 цента/кВт·ч. До этого окна реле выключено. "
        "Приоритет: ручной режим и принудительное включение, аварийный предел, отпуск, "
        "порог, умный порог, пробуждение, включение ночью.</small></fieldset>"
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

// Ответ кусками по мере формирования: без Content-Length, конец тела — закрытие
// соединения. Для истории: 512 строк не помещаются в String целиком.
template <typename Client>
class BufferedClientWriter : public Print {
public:
    explicit BufferedClientWriter(Client& client) : client_(client) {}

    size_t write(uint8_t value) override {
        return write(&value, 1);
    }

    size_t write(const uint8_t* data, size_t length) override {
        size_t done = 0;
        while (done < length) {
            if (used_ == sizeof(buffer_) && !sendBuffer()) return done;
            size_t chunk = length - done;
            if (chunk > sizeof(buffer_) - used_) chunk = sizeof(buffer_) - used_;
            memcpy(buffer_ + used_, data + done, chunk);
            used_ += chunk;
            done += chunk;
        }
        return length;
    }

    bool finish() {
        return sendBuffer();
    }

private:
    bool sendBuffer() {
        if (failed_) return false;
        if (used_ > 0) {
            failed_ = !writeClientData(client_, buffer_, used_);
            used_ = 0;
        }
        return !failed_;
    }

    Client& client_;
    uint8_t buffer_[1024];
    size_t used_ = 0;
    bool failed_ = false;
};

template <typename Client>
void sendHistoryPage(Client& networkClient) {
    BufferedClientWriter<Client> client(networkClient);
    client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
        "<!doctype html><html lang='ru'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Opta History</title><link rel='icon' href='data:,'>"
        "<style>body{font:16px sans-serif;max-width:1100px;margin:2em auto;padding:0 1em;color:#202124}"
        ".nav{display:flex;gap:16px;margin:0 0 16px}.nav a{color:#1769aa}"
        ".table-wrap{overflow-x:auto}table{border-collapse:collapse;width:100%}"
        "th,td{text-align:left;padding:6px 10px;border-bottom:1px solid #d9dee5;white-space:nowrap}"
        "td.cause{white-space:normal}.on{color:#137333;font-weight:bold}.off{color:#5f6368}"
        "tr.device td{background:#f1f3f4;font-weight:bold}</style></head><body>"
        "<h1>История переключений</h1>"
        "<nav class='nav'><a href='/'>Настройки</a><strong>История переключений</strong></nav>"
        "<p>История хранится в оперативной памяти до перезапуска или пропадания питания, "
        "последние "));
    client.print(static_cast<unsigned>(history::SwitchHistory::kCapacity));
    client.print(F(" событий. Записано: "));
    client.print(static_cast<unsigned>(switchHistory.size()));
    if (switchHistory.dropped() > 0) {
        client.print(F(", самые старые вытеснены: "));
        client.print(switchHistory.dropped());
    }
    client.print(F(". Новые сверху.</p><div class='table-wrap'><table>"
        "<tr><th>Время (Рига)</th><th>Реле</th><th>Состояние</th><th>Причина</th></tr>"));

    char timeText[32];
    for (size_t i = 0; i < switchHistory.size(); ++i) {
        const history::SwitchEvent& event = switchHistory.newest(i);
        history::formatEventTime(event, timeText, sizeof(timeText));
        if (event.channel == history::kDeviceEvent) {
            client.print(F("<tr class='device'><td>"));
            client.print(timeText);
            client.print(F("</td><td colspan='3'>"));
            client.print(history::causeName(event.cause));
            client.print(F("</td></tr>"));
            continue;
        }
        client.print(F("<tr><td>"));
        client.print(timeText);
        client.print(F("</td><td>Реле "));
        client.print(event.channel + 1);
        client.print(event.on ? F("</td><td class='on'>ВКЛ") : F("</td><td class='off'>ВЫКЛ"));
        client.print(F("</td><td class='cause'>"));
        client.print(history::causeName(event.cause));
        client.print(F("</td></tr>"));
    }
    client.print(F("</table></div></body></html>"));
    if (!client.finish()) {
        Serial.println("[WEB] Страница истории передана не полностью.");
    }
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
    if (!wifiCredentialStore.load(credentials)) {
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
    uint8_t packet[dnspacket::kMaxPacket];
    int receivedSize = udp.read(packet, sizeof(packet));
    const uint8_t ip[4] = {address[0], address[1], address[2], address[3]};
    uint8_t answerPacket[dnspacket::kMaxPacket];
    dnspacket::MdnsReply reply = dnspacket::buildMdnsAnswer(packet, receivedSize,
        querySourcePort, DEVICE_HOSTNAME, ip, answerPacket, sizeof(answerPacket));
    if (reply.status == dnspacket::MdnsStatus::NotOurName) {
        Serial.println("[mDNS] Запрос не содержит поддерживаемое имя opta-settings.local.");
        return;
    }
    if (reply.status != dnspacket::MdnsStatus::Answer) {
        return;
    }

    IPAddress responseAddress = reply.unicast ? querySource : MDNS_MULTICAST_IP;
    uint16_t responsePort = reply.unicast ? querySourcePort : 5353;
    if (udp.beginPacket(responseAddress, responsePort)) {
        udp.write(answerPacket, reply.length);
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
    uint8_t packet[dnspacket::kMaxPacket];
    int receivedSize = dnsUdp.read(packet, sizeof(packet));
    const uint8_t portalIp[4] = {CONFIG_AP_IP[0], CONFIG_AP_IP[1], CONFIG_AP_IP[2], CONFIG_AP_IP[3]};
    size_t answerLength = dnspacket::buildCaptiveDnsAnswer(packet, receivedSize,
        sizeof(packet), portalIp);
    if (answerLength == 0) {
        return;
    }
    if (dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort())) {
        dnsUdp.write(packet, answerLength);
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

// Тело POST длиной contentLength (1…kMaxFormBody). false — ответ об ошибке уже отправлен.
template <typename Client>
bool readHttpBody(Client& client, int contentLength, String& body) {
    if (contentLength <= 0 || contentLength > static_cast<int>(httpparse::kMaxFormBody)) {
        client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid form"));
        return false;
    }
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
        return false;
    }
    return true;
}

template <typename Client>
void sendRedirectHome(Client& client) {
    client.println(F("HTTP/1.1 303 See Other"));
    client.println(F("Location: /"));
    client.println(F("Cache-Control: no-store"));
    client.println(F("Connection: close"));
    client.println();
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

    if (method == "GET") {
        uint8_t channelIndex;
        if (httpparse::queryChannel(path.c_str(), channelIndex)) {
            selectedRelayIndex = channelIndex;
        }
        int8_t chartDay;
        if (httpparse::queryChartDay(path.c_str(), chartDay)) {
            selectedChartDay = chartDay;
        }
    }
    if (method == "GET" && path.startsWith("/history")) {
        sendHistoryPage(client);
        client.stop();
        return;
    }

    if (method == "POST" && path == "/force") {
        String body;
        if (!readHttpBody(client, contentLength, body)) {
            client.stop();
            return;
        }
        httpparse::ForceForm form;
        if (!httpparse::parseForceForm(body.c_str(), body.length(), form)) {
            client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid force-on request"));
            client.stop();
            return;
        }
        selectedRelayIndex = form.channelIndex;
        if (form.start) {
            channelController.startForceOn(form.channelIndex);
        } else {
            channelController.stopForceOn(form.channelIndex);
        }
        sendRedirectHome(client);
        client.stop();
        return;
    }

    if (method == "POST" && path == "/save") {
        String body;
        if (!readHttpBody(client, contentLength, body)) {
            client.stop();
            return;
        }

        httpparse::SettingsForm form;
        httpparse::FormError formError = httpparse::parseSettingsForm(
            body.c_str(), body.length(), channelController.configs(),
            systemClock.synced() ? systemClock.nowUtc() : 0,
            wifiCredentialsVisible, form);
        // Выбор реле запоминается, даже если остальные поля формы неверны
        if (formError != httpparse::FormError::InvalidChannel) {
            selectedRelayIndex = form.channelIndex;
        }
        if (formError != httpparse::FormError::Ok) {
            client.print(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"));
            client.println(httpparse::formErrorText(formError));
            client.stop();
            return;
        }

        if (!channelController.saveConfig(selectedRelayIndex, form.config)) {
            client.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nUnable to save device settings"));
            client.stop();
            Serial.println("[ERROR] Не удалось сохранить настройки устройства в KVStore.");
            return;
        }

        bool credentialsChanged = form.hasWifi;
        bool restartRequested = form.restartRequested;
        if (credentialsChanged) {
            WifiCredentials credentials = {};
            memcpy(credentials.ssid, form.ssid, sizeof(credentials.ssid));
            memcpy(credentials.password, form.password, sizeof(credentials.password));
            if (!wifiCredentialStore.save(credentials)) {
                client.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nUnable to save Wi-Fi credentials"));
                client.stop();
                Serial.println("[ERROR] Не удалось сохранить Wi-Fi данные в KVStore.");
                return;
            }
        }
        if (!credentialsChanged && WiFi.status() == WL_CONNECTED) {
            channelController.applyConfiguredControl(selectedRelayIndex);
        }
        if (credentialsChanged || restartRequested) {
            sendRedirectHome(client);
            client.flush();
            client.stop();
            delay(500);
            NVIC_SystemReset();
            return;
        }
        sendRedirectHome(client);
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

// Запрос цен Elering за период path → out. Прогноз контроллера не трогается.
bool fetchPrices(const char* path, prices::PriceSeries& out) {
    if (!client.connect(serverHost, serverPort)) {
        Serial.println("[ERROR] Не удалось установить TLS-соединение для прогноза.");
        client.stop();
        return false;
    }
    client.print("GET ");
    client.print(path);
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

    const char* jsonError = "";
    switch (elering::parseForecast(response.c_str(), response.length(), out, jsonError)) {
        case elering::Error::Ok:
            break;
        case elering::Error::Json:
            Serial.print("[ERROR] Ошибка разбора прогноза Elering: ");
            Serial.println(jsonError);
            return false;
        case elering::Error::Empty:
            Serial.println("[ERROR] Elering вернул пустой прогноз цен LV.");
            return false;
        default:
            Serial.println("[ERROR] В ответе Elering отсутствует массив цен LV.");
            return false;
    }
    Serial.print("[FORECAST] Получено квартальных значений: ");
    Serial.println(out.size());
    return true;
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
    size_t jsonStart = 0;
    size_t jsonEnd = 0;
    if (elering::jsonObjectBounds(jsonBody.c_str(), jsonBody.length(), jsonStart, jsonEnd)) {
        jsonBody = jsonBody.substring(jsonStart, jsonEnd);
    }

    Serial.print("[HTTP] Ответ сервера: ");
    Serial.println(jsonBody);

    elering::CurrentPrice price = {};
    const char* jsonError = "";
    uint32_t nowEpoch = systemClock.synced() ? systemClock.nowUtc() : 0;
    switch (elering::parseCurrent(jsonBody.c_str(), jsonBody.length(), nowEpoch, price, jsonError)) {
        case elering::Error::Ok:
            break;
        case elering::Error::Json:
            Serial.print("[ERROR] Ошибка парсинга JSON: ");
            Serial.println(jsonError);
            return false;
        case elering::Error::NoPrice:
            Serial.println("[ERROR] В ответе API нет числовой цены или timestamp!");
            return false;
        case elering::Error::OutOfRange:
            Serial.print("[ERROR] Цена вне допустимого диапазона: ");
            Serial.println(price.eurMwh);
            return false;
        case elering::Error::BadTimestamp:
            Serial.print("[ERROR] Некорректный timestamp цены: ");
            Serial.println(price.timestamp);
            return false;
        case elering::Error::Stale:
            Serial.print("[ERROR] Цена не относится к текущему времени (timestamp ");
            Serial.print(price.timestamp);
            Serial.print(", сейчас ");
            Serial.print(nowEpoch);
            Serial.println(").");
            return false;
        default:
            Serial.println("[ERROR] Некорректный формат данных в ответе API!");
            return false;
    }

    currentPriceEurKwh = price.eurKwh;
    priceTimestamp = price.timestamp;

    Serial.print("[NORDPOOL-LV] Цена из API: ");
    Serial.print(currentPriceEurKwh, 5);
    Serial.print(" EUR/kWh, timestamp: ");
    Serial.println(priceTimestamp);

    return true;
}

// =============================================================================
// ПОДРОБНЫЙ ОТЧЕТ О СТАТУСЕ В СЕРИЙНЫЙ МОНИТОР
// =============================================================================
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
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        Serial.print("    Реле ");
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(channelController.outputOn(i) ? "ВКЛЮЧЕНО" : "ВЫКЛЮЧЕНО");
        if (channelController.forceOnActive(i)) {
            char remaining[24];
            forceon::formatRemaining(channelController.forceOnRemainingMs(i), remaining,
                sizeof(remaining));
            Serial.print(" (принудительно, осталось ");
            Serial.print(remaining);
            Serial.print(")");
        } else if (channelController.manual(i)) {
            Serial.print(" (ручной режим)");
        }
        Serial.println();
    }

    Serial.print("  [Биржа] Цена LV:   ");
    if (channelController.hasLatestPrice()) {
        Serial.print(channelController.latestPriceEurKwh(), 5);
        Serial.println(" EUR/kWh");
    } else if (channelController.hadAnyPrice()) {
        Serial.println("нет актуальной цены больше часа; автоматические реле выключены");
    } else {
        Serial.println("Ожидание первого запроса...");
    }

    const ChannelConfig& selectedConfig = channelController.config(selectedRelayIndex);
    Serial.print("  [Детали реле ");
    Serial.print(selectedRelayIndex + 1);
    Serial.print("] Режим: ");
    Serial.println(channelController.modeName(selectedRelayIndex));
    Serial.print("  [Детали реле ");
    Serial.print(selectedRelayIndex + 1);
    Serial.print("] Порог включения: ");
    Serial.print(selectedConfig.alwaysOnBelowPriceEurKwh, 5);
    Serial.println(" EUR/kWh");
    Serial.print("  [Детали реле ");
    Serial.print(selectedRelayIndex + 1);
    Serial.print("] Аварийный предел: ");
    Serial.print(selectedConfig.safetyCeilingEurKwh, 5);
    Serial.println(" EUR/kWh");

    uint32_t retrySeconds = 0;
    if (channelController.priceRetryPending(retrySeconds)) {
        Serial.print("  [Таймер]:          Повтор цены через ");
        Serial.print(retrySeconds);
        Serial.println(" сек");
    } else if (systemClock.synced()) {
        uint32_t secondsToNextInterval =
            controller::kPriceIntervalSeconds -
            (systemClock.nowUtc() % controller::kPriceIntervalSeconds);
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
    Serial.println("  Команды: 'status [1-4]' (или '?'), 'update', 'on [1-4]', 'off [1-4]', 'auto [1-4]',");
    Serial.println("           'force [1-4]', 'force off [1-4]', 'history [1-4]'");
    Serial.println("==================================================\n");
}

// Последние переключения, новые сверху. channel == kDeviceEvent — все реле.
// События устройства (запуск) выводятся всегда.
void printSwitchHistory(uint8_t channel) {
    const size_t kMaxLines = 50;
    Serial.println("\n[HISTORY] Переключения (новые сверху), хранятся до перезапуска:");
    size_t printed = 0;
    char timeText[32];
    for (size_t i = 0; i < switchHistory.size() && printed < kMaxLines; ++i) {
        kickWatchdog();
        const history::SwitchEvent& event = switchHistory.newest(i);
        if (channel != history::kDeviceEvent && event.channel != channel &&
            event.channel != history::kDeviceEvent) {
            continue;
        }
        history::formatEventTime(event, timeText, sizeof(timeText));
        Serial.print("  ");
        Serial.print(timeText);
        if (event.channel == history::kDeviceEvent) {
            Serial.print("  ---------  ");
        } else {
            Serial.print("  Реле ");
            Serial.print(event.channel + 1);
            Serial.print(event.on ? " ВКЛ   " : " ВЫКЛ  ");
        }
        Serial.println(history::causeName(event.cause));
        ++printed;
    }
    if (printed == 0) {
        Serial.println("  Событий нет.");
    }
    Serial.print("[HISTORY] Всего событий: ");
    Serial.print(switchHistory.size());
    Serial.print(", показано: ");
    Serial.println(printed);
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
        bool relayArgumentGiven = false;
        bool forceOffRequested = false;
        String relayArgument;
        int argumentStart = cmd.indexOf(' ');
        if (argumentStart >= 0) {
            relayArgument = cmd.substring(argumentStart + 1);
            relayArgument.trim();
            // force off [N]: номер реле — после слова off
            if (cmd.startsWith("force ") &&
                (relayArgument == "off" || relayArgument.startsWith("off "))) {
                forceOffRequested = true;
                relayArgument = relayArgument.substring(3);
                relayArgument.trim();
            }
            int relayNumber = relayArgument.toInt();
            if (relayNumber >= 1 && relayNumber <= kChannelCount) {
                commandRelay = static_cast<uint8_t>(relayNumber - 1);
                selectedRelayIndex = commandRelay;
                relayArgumentGiven = true;
            }
            cmd = cmd.substring(0, argumentStart);
            cmd.trim();
        }

        if (cmd == "status" || cmd == "?" || cmd == "info" || cmd == "") {
            printStatusReport();
        } else if (cmd == "update" || cmd == "price") {
            Serial.println("\n[CMD] Внеочередной запрос актуальных котировок...");
            channelController.updatePrice();
            printStatusReport();
        } else if (cmd == "on") {
            channelController.setManual(commandRelay, true);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" ВКЛЮЧЕНО вручную (ручной режим до границы 15 минут).");
        } else if (cmd == "off") {
            channelController.setManual(commandRelay, false);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" ВЫКЛЮЧЕНО вручную (ручной режим до границы 15 минут).");
        } else if (cmd == "auto") {
            channelController.clearManual(commandRelay);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" возвращено в автоматический режим.");
            if (channelController.forceOnActive(commandRelay)) {
                channelController.stopForceOn(commandRelay);
            }
            channelController.updatePrice();
        } else if (cmd == "force") {
            if (relayArgument.length() > 0 && !relayArgumentGiven) {
                Serial.println("\n[CMD] Формат: force [1-4] или force off [1-4].");
            } else if (forceOffRequested) {
                channelController.stopForceOn(commandRelay);
            } else {
                channelController.startForceOn(commandRelay);
            }
        } else if (cmd == "history") {
            printSwitchHistory(relayArgumentGiven ? commandRelay : history::kDeviceEvent);
        } else {
            Serial.print("\n[CMD] Неизвестная команда: \"");
            Serial.print(cmd);
            Serial.println("\". Доступные команды: status [1-4], update, on [1-4], off [1-4], auto [1-4], "
                "force [1-4], force off [1-4], history [1-4]");
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
    ChannelConfig loadedConfigs[kChannelCount] = {};
    if (!loadDeviceConfigs(loadedConfigs)) {
        Serial.println("[WARN] KVStore недоступен; используются настройки по умолчанию из RAM.");
        for (uint8_t i = 0; i < kChannelCount; ++i) {
            channelconfig::setDefaults(loadedConfigs[i]);
        }
    }

    // Инициализация пинов реле, кнопки и светодиодов. Реле уже выключены в RAM,
    // поэтому в историю попадает одно событие запуска, а не четыре выключения.
    switchHistory.record(0, millis() / 1000UL, history::kDeviceEvent, false,
        history::Cause::Startup);
    relayOutputs.begin();
    channelController.begin(loadedConfigs);
    statusLed.begin();
    userButton.begin();

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
        bool credentialsValid = wifiCredentialStore.load(credentials);
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
            channelController.updatePrice();
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
        channelController.updatePrice();
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
    channelController.serviceFailsafe();
    channelController.serviceForceOn();
    unsigned long currentMillis = millis();

    // -------------------------------------------------------------------------
    // 1. Обработка команд из Serial Monitor ('status', 'update', 'on', 'off')
    // -------------------------------------------------------------------------
    handleSerialCommands();

    // -------------------------------------------------------------------------
    // 2. Опрос кнопки на корпусе Opta (BTN_USER) для ручного управления
    // -------------------------------------------------------------------------
    const userbutton::Event buttonEvent = userButton.service(currentMillis);
#ifdef USE_WIFI
    if (buttonEvent == userbutton::Event::ShortPress) {
        bool newState = !channelController.outputOn(0);
        channelController.setManual(0, newState,
            "\n[BUTTON] Короткое нажатие USER: ручное переключение Реле 1.");
    }
#else
    if (buttonEvent == userbutton::Event::Pressed) {
        bool newState = !channelController.outputOn(0);
        channelController.setManual(0, newState, newState
            ? "\n[BUTTON] ==========================================\r\n"
              "[BUTTON] Нажата кнопка USER! Ручное переключение Реле 1: ВКЛЮЧЕНО"
            : "\n[BUTTON] ==========================================\r\n"
              "[BUTTON] Нажата кнопка USER! Ручное переключение Реле 1: ВЫКЛЮЧЕНО");
        Serial.println("[BUTTON] (Реле переведено в РУЧНОЙ режим до следующего цикла)");
        Serial.println("[BUTTON] ==========================================\n");
    }
#endif

#ifdef USE_WIFI
    if (buttonEvent == userbutton::Event::LongPress) {
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

    statusLed.service(currentMillis, isConnected);

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
            if (wifiCredentialStore.load(credentials)) {
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
        for (uint8_t i = 0; i < kChannelCount; ++i) {
            if (i > 0) Serial.print(", ");
            Serial.print(i + 1);
            Serial.print(" ");
            Serial.print(channelController.outputOn(i) ? "ВКЛ" : "ВЫКЛ");
            if (channelController.forceOnActive(i)) Serial.print(" принуд.");
        }
        Serial.print(" | Цена: ");
        if (channelController.hasLatestPrice()) {
            Serial.print(channelController.latestPriceEurKwh(), 5);
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
