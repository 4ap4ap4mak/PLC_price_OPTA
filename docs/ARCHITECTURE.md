# Архитектура прошивки Opta: модули и интерфейсы

Целевое разбиение монолитного `src/main.cpp` (≈3260 строк). Документ описывает,
куда переезжает каждый кусок кода, какие у модулей публичные интерфейсы и в каком
порядке выполнять перенос (этап 2 плана рефакторинга).

## 1. Принципы

1. **Слои, зависимости только вниз.** Чистая логика не знает про Arduino, сеть и flash.
2. **Чистое ядро в `lib/`, железо в `src/`.** Всё из `lib/` собирается и под Opta, и под
   `env:native` для unit-тестов на ПК. В `lib/` запрещены `Arduino.h`, `String`, `Serial`,
   `millis()`, глобальные переменные. Время передается параметром (`uint32_t` epoch UTC).
3. **Нет глобального состояния.** Все объекты создаются статически в `main.cpp` и
   получают зависимости по ссылке в конструкторе. Динамической памяти не добавляем.
4. **Чистые модули не логируют.** Они возвращают код результата/причину, а логирует
   вызывающий слой.
5. **Один источник правды для плана реле.** Решение «включить сейчас» и серые полосы на
   графике берутся из одного и того же `ChannelPlan`. Сейчас это две разные функции
   (`getAutomaticRelayDecision` и `buildRelayWorkSlotsForChart`), и они расходятся.
6. **Перенос кода и изменение поведения — в разных ветках** (см. раздел 6).
7. **Устройство — контроллер нагрузок для умного дома, а не терморегулятор** (D-010).
   Логическая единица — **канал** (`Channel*`), физическое реле — **выход** (`Outputs`).
   Сейчас канал = реле Opta, а нагрузка — бойлер. Слово «нагрев» в именах не используется.
   Что закладывается для расширения — раздел 8.

## 2. Слои и состав модулей

```
L3  src/main.cpp                    сборка объектов, setup()/loop()
L2  src/app/   Controller           расписание цены, failsafe, ручной режим, применение плана
    src/web/   WebServer, Router, SettingsPage, ChartSvg
    src/console/ SerialConsole, StatusReport
L1  src/platform/ Log, SystemClock(NTP), KvStorage, ConfigStore, WifiCredentialStore,
                  RelayOutputBank, UserButton, StatusLed, Watchdog
    src/net/   NetworkManager, HttpsFetcher(+FetchWorker), MdnsResponder, CaptiveDns
L0  lib/rigatime     даты, DST Europe/Riga, разбор/формат локального времени
    lib/prices       PriceSeries (15-мин слоты), агрегация в часы
    lib/channelconfig  ChannelConfig: значения по умолчанию, валидация, кодек/миграция блоба
    lib/planner      сценарии → ChannelPlan, решение для текущего момента
    lib/elering      разбор JSON Elering (текущая цена и прогноз) + валидация
    lib/httpparse    строка запроса, query, urlencoded-формы, форма → ChannelConfig
    lib/dnspacket    сборка ответов captive DNS и mDNS
```

Откуда что берется в текущем `main.cpp`:

| Модуль | Текущий код в `src/main.cpp` |
|---|---|
| `rigatime` | `daysFromCivil`, `civilFromDays`, `lastSundayOfMonth`, `rigaDaylightSaving`, `parseRigaDateTime`, `formatRigaDateTime`, `parseTimeOfDay`, `formatTimeOfDay`, `localHourAndDay` |
| `prices` | `ForecastSlot`, `PriceHour`, `buildPriceHours` |
| `channelconfig` | `DeviceConfig`, `Legacy*`, `setDefaultDeviceConfig`, `importLegacyDeviceConfig`, `isValidDeviceConfig`, разбор блоба из `loadDeviceConfigs` |
| `planner` | `nightWindowKey`, `choose*`, `getNextWakeupEpoch`, `isWakeupHeatingTime`, `getAutomaticRelayDecision`, `buildRelayWorkSlotsForChart`, `relayModeName` |
| `elering` | разбор JSON из `fetchNordPoolPrice` и `fetchPriceForecast`, расчет диапазона дат прогноза |
| `httpparse` | `decodeFormValue`, `formField`, `parseFormFloat`, `parseFormInteger`, разбор формы `/save` и query |
| `dnspacket` | сборка пакетов из `serviceMDNSOn` и `handleDnsRequests` |
| `SystemClock` | `syncNetworkTime`, `getCurrentEpochSeconds` |
| `ConfigStore` / `WifiCredentialStore` | `kv_get`/`kv_set` из `load/saveDeviceConfigs`, `load/saveWifiCredentials` |
| `RelayOutputBank` | `setRelay`, пины, светодиоды |
| `UserButton`, `StatusLed` | антидребезг, короткое/долгое нажатие, мигание |
| `NetworkManager` | `startWifiManager`, `stopWifiManager`, `servicePortalReconnect`, `serviceWiredNetwork`, переподключение из `loop`, Wi-Fi часть `setup` |
| `HttpsFetcher` | TLS-клиент, `readHttpResponseBody` |
| `WebServer`/`Router` | `CheckedWiFiServer`, `CheckedEthernetServer`, `readHttpLine`, `handlePortalClient`, `service*WebServer` |
| `SettingsPage`, `ChartSvg` | `sendPortalPage`, `streamPriceChartSvg`, `sendPriceChart`, `StringPageWriter`, `htmlEscape` |
| `Controller` | `servicePriceSchedule`, `updatePriceAndRelay`, `serviceScheduledControl`, `servicePriceFailsafe`, `applyCurrentPriceToConfiguredControl`, `manualOverrides`, `latestPrice*` |
| `SerialConsole` | `handleSerialCommands`, `printStatusReport`, heartbeat из `loop` |

## 3. Интерфейсы L0 (чистое ядро, покрывается тестами)

Сигнатуры ориентировочные: имена и состав полей можно уточнять при переносе,
а разделение ответственности должно остаться таким.

### 3.1 `lib/rigatime/RigaTime.h`

```cpp
namespace rigatime {
constexpr uint32_t kSecondsPerDay = 86400;

bool     isDaylightSaving(uint32_t utc);          // последнее вс марта/октября, 01:00 UTC
int32_t  utcOffsetSeconds(uint32_t utc);           // 7200 или 10800
uint32_t toLocal(uint32_t utc);                    // utc + offset
uint32_t localDay(uint32_t utc);                   // номер локальных суток
uint8_t  localHour(uint32_t utc);
uint16_t localMinuteOfDay(uint32_t utc);

// Локальная дата/время → UTC; false для несуществующего времени (переход на летнее).
bool localToUtc(int year, unsigned month, unsigned day,
                unsigned hour, unsigned minute, uint32_t& utc);
// Ближайший момент после `after`, когда локальное время равно minuteOfDay.
bool nextLocalTime(uint32_t after, uint16_t minuteOfDay, uint32_t& utc);
// Начало локальных суток `day` в UTC (учитывает DST).
uint32_t localDayStartUtc(uint32_t day);

// Текстовые форматы веб-формы. Буферы выделяет вызывающий.
bool parseTimeOfDay(const char* hhmm, uint16_t& minuteOfDay);               // "07:30"
void formatTimeOfDay(uint16_t minuteOfDay, char out[6]);
bool parseLocalDateTime(const char* text, uint32_t& utc);                   // "2026-10-20T07:30"
void formatLocalDateTime(uint32_t utc, char out[17]);
void formatUtcIso(uint32_t utc, char out[25]);                              // для запроса к Elering
}
```

### 3.2 `lib/prices/PriceSeries.h`

```cpp
constexpr size_t kMaxSlots = 200;      // 2 суток по 15 мин + запас на DST
constexpr size_t kMaxHours = 52;

struct PriceSlot { uint32_t start; float eurKwh; };
struct PriceHour { uint32_t start; float avgEurKwh; uint8_t quarters; };  // quarters==4 → полный час

class PriceSeries {
public:
    void   clear();
    bool   append(uint32_t start, float eurKwh);   // false: переполнение/не по порядку
    size_t size() const;
    const PriceSlot& operator[](size_t i) const;
    int    indexAt(uint32_t utc) const;             // слот, содержащий момент, или -1
    size_t buildHours(PriceHour* out, size_t cap) const;
    uint32_t coverageEnd() const;                   // конец последнего слота
};
```

### 3.3 `lib/channelconfig/ChannelConfig.h`

```cpp
constexpr uint8_t  kChannelCount = 4;
constexpr uint16_t kConfigVersion = 5;

struct ChannelConfig { /* поля как в текущем DeviceConfig */ };
static_assert(sizeof(ChannelConfig) == 36, "формат блоба во flash изменился — нужна миграция");

void defaults(ChannelConfig& c);
// Код первой нарушенной проверки (для понятной ошибки в вебе), Ok если всё верно.
enum class ConfigError : uint8_t { Ok, CeilingBelowThreshold, NightWindow, NightHours, ... };
ConfigError validate(const ChannelConfig& c);

// Блоб из KVStore → 4 конфигурации. Распознает v2/v3 (одно реле), v4 (4 реле), v5.
enum class DecodeResult : uint8_t { Ok, Migrated, Defaulted, Corrupt };
DecodeResult decode(const uint8_t* blob, size_t size, ChannelConfig out[kChannelCount]);
size_t       encode(const ChannelConfig in[kChannelCount], uint8_t* blob, size_t cap);
```

36 байт — расчетный размер текущего `DeviceConfig` с выравниванием ARM; при переносе
сверить с `sizeof` в реальной сборке. Новые поля
(например, минимальное время работы реле) — только через `kConfigVersion = 6` и ветку
миграции в `decode`; тест миграции фиксирует старые блобы байт в байт.

### 3.4 `lib/planner/Planner.h` — главный модуль

```cpp
namespace planner {

enum class Reason : uint8_t {
    Off, Threshold, Smart, Wakeup, Night, Holiday,   // почему слот включен
    Ceiling, NoForecast                              // почему выключен принудительно
};
// «Принудительное включение» в планировщике отсутствует: это команда пользователя
// с таймером, ее ведет Controller (см. 5.1 и docs/DECISIONS.md, D-006).

// План одного реле на горизонт прогноза, по 15-минутным слотам PriceSeries.
struct ChannelPlan {
    size_t  slotCount;
    bool    on[kMaxSlots];
    Reason  reason[kMaxSlots];
    bool    holidayExpired;      // Controller должен выключить отпуск и сохранить конфиг
    uint8_t warnings;            // битовая маска: прогноз не покрывает окно и т.п.
};

// Зафиксированный выбор часов, чтобы план не «плыл» по мере хода времени.
struct HourSelection {
    uint32_t key;        // окно: локальные сутки (умный порог, ночь), момент возвращения (отпуск)
    uint32_t baseHour;   // UTC начала первого часа маски
    uint64_t mask;       // бит i → час baseHour + i*3600 выбран (покрывает 64 часа)
};
struct PlannerState {
    uint32_t      configRevision;   // смена конфига сбрасывает все фиксации
    HourSelection smart, night, holiday;
};

void buildPlan(const ChannelConfig& cfg, const PriceSeries& prices, uint32_t now,
               PlannerState& state, ChannelPlan& out);

struct Decision { bool on; Reason reason; };
// Текущая цена из /current авторитетнее прогноза для аварийного предела и порога.
Decision decideNow(const ChannelConfig& cfg, const ChannelPlan& plan, const PriceSeries& prices,
                   uint32_t now, float currentEurKwh);

const char* reasonName(Reason r);   // «Отпуск», «Умный порог включения», ...
}
```

Приоритет внутри планировщика: аварийный предел → отпуск →
(порог ИЛИ умный порог ИЛИ пробуждение ИЛИ ночь). Над планировщиком в `Controller`
стоят команды пользователя: принудительное включение и ручной режим (см. docs/SCENARIOS.md).

Побочный эффект «выключить отпуск и записать во flash» сейчас сидит внутри
`getAutomaticRelayDecision`. Он переезжает в `Controller` через флаг `holidayExpired`.

`PlannerState` живет в RAM внутри `Controller`. Выбор считается по **всему окну**
(включая прошедшие часы), а не по оставшейся части. Поэтому после перезагрузки план
восстанавливается тем же, если прогноз покрывает окно. Сохранять состояние во flash
не нужно.

### 3.5 `lib/elering/Elering.h`

```cpp
namespace elering {
enum class Error : uint8_t { Ok, Json, Format, NoPrice, OutOfRange, Stale, Future, Empty };

struct CurrentPrice { float eurKwh; uint32_t timestamp; };
// now == 0 → время не синхронизировано, проверка возраста пропускается.
Error parseCurrent(const char* json, size_t len, uint32_t now, CurrentPrice& out);
Error parseForecast(const char* json, size_t len, PriceSeries& out);

// Путь запроса прогноза: локальные сегодня 00:00 … завтра 23:45 (Europe/Riga).
void forecastPath(uint32_t now, char* out, size_t cap);
constexpr const char* kHost = "dashboard.elering.ee";
constexpr const char* kCurrentPath = "/api/nps/price/lv/current";
}
```

ArduinoJson header-only и собирается под `native`, поэтому разбор тестируется на
сохраненных реальных ответах API (`test/fixtures/*.json`).

### 3.6 `lib/httpparse/HttpParse.h`

```cpp
struct RequestLine { char method[8]; char path[64]; char query[128]; };
bool parseRequestLine(const char* line, RequestLine& out);
bool parseContentLength(const char* headerLine, size_t& value);   // регистронезависимо

// Ищет поле в query или urlencoded-теле, декодирует %XX и '+'. false если поля нет.
bool formField(const char* body, const char* name, char* out, size_t cap);
bool formInt(const char* body, const char* name, int lo, int hi, int& out);
bool formFloat(const char* body, const char* name, float lo, float hi, float& out);

// Форма /save → новая конфигурация реле. errorText — человекочитаемая причина отказа.
struct SettingsForm {
    uint8_t channelIndex; ChannelConfig config;
    bool hasWifi; char ssid[33]; char password[64];
    bool restartRequested;
};
bool parseSettingsForm(const char* body, const ChannelConfig current[kChannelCount],
                       uint32_t now, bool wifiFieldsVisible,
                       SettingsForm& out, const char*& errorText);
```

### 3.7 `lib/dnspacket/DnsPacket.h`

```cpp
// Возвращают длину ответа в out или 0, если запрос надо проигнорировать.
size_t buildCaptiveDnsAnswer(const uint8_t* query, size_t len, const uint8_t ip[4],
                             uint8_t* out, size_t cap);
struct MdnsReply { size_t length; bool unicast; };
MdnsReply buildMdnsAnswer(const uint8_t* query, size_t len, uint16_t srcPort,
                          const char* hostLabel, const uint8_t ip[4],
                          uint8_t* out, size_t cap);
```

## 4. Интерфейсы L1 (платформа)

Абстрактные классы вводятся только там, где `Controller` нужно подменить в тесте на ПК.
Остальное — конкретные классы.

```cpp
// src/platform/Log.h — замена сотен строк Serial.print
void logf(const char* tag, const char* fmt, ...);        // "[NTP] ..." через vsnprintf
```

```cpp
// Интерфейсы для Controller (src/app/Ports.h)
struct Clock {
    virtual bool     synced() const = 0;
    virtual uint32_t nowUtc() const = 0;
    virtual uint32_t uptimeMs() const = 0;
};
struct Outputs {
    virtual void set(uint8_t channel, bool on) = 0;
    virtual bool get(uint8_t channel) const = 0;
};
struct ConfigRepository {
    virtual bool save(const ChannelConfig cfg[kChannelCount]) = 0;
};
// Асинхронный источник цен: запрос не блокирует loop().
struct PriceSource {
    virtual void requestCurrent() = 0;
    virtual void requestForecast(uint32_t now) = 0;
    virtual bool pollCurrent(elering::Error& err, elering::CurrentPrice& out) = 0;  // true — результат готов
    virtual bool pollForecast(elering::Error& err, PriceSeries& out) = 0;
    virtual bool busy() const = 0;
};
```

Конкретные реализации:

| Класс | Ответственность |
|---|---|
| `SystemClock : Clock` | NTP-синхронизация (неблокирующая: отправил пакет → проверяет ответ в `service()`), ресинхронизация раз в 6 ч |
| `RelayOutputBank : Outputs` | пины D0–D3 и светодиоды, лог переключений |
| `KvStorage` | обертка `kv_get`/`kv_set`/`kv_get_info` с кодами ошибок |
| `ConfigStore : ConfigRepository` | `load()` через `channelconfig::decode`, `save()` через `encode` |
| `WifiCredentialStore` | `load(WifiCredentials&)`, `save(...)` |
| `UserButton` | `service(nowMs)` → событие `None / ShortPress / LongPress(5 с)` |
| `StatusLed` | `service(nowMs, connected)` |
| `Watchdog` | `start()`, `kick()` |
| `HttpsFetcher` | синхронный `get(host, path, buf, cap, len)`: TLS, лимиты заголовков/тела, таймауты |
| `FetchWorker : PriceSource` | отдельный `rtos::Thread`, который выполняет `HttpsFetcher` и `elering::parse*` и отдает результат под мьютексом |
| `NetworkManager` | конечный автомат сети (ниже) |
| `MdnsResponder`, `CaptiveDns` | UDP-сокеты + `dnspacket` |

### 4.1 `NetworkManager`

```cpp
enum class NetMode : uint8_t { Starting, StaConnecting, StaOnline, Portal, EthernetOnly };

class NetworkManager {
public:
    NetworkManager(WifiCredentialStore&);
    void begin();                         // Ethernet DHCP + попытка STA с ретраями
    void service(uint32_t nowMs);         // переподключение, выход из портала, смена IP
    void requestPortal();                 // долгое нажатие USER
    void onCredentialsSaved();            // вместо немедленного NVIC_SystemReset (позже)
    NetMode mode() const;
    bool internetUp() const;              // есть маршрут к Elering
    bool wifiFieldsVisible() const;       // показывать поля SSID/пароль на странице
    // Интерфейсы, на которых должны слушать HTTP/mDNS; меняются при смене IP.
    bool wifiAddress(IPAddress&) const;
    bool ethernetAddress(IPAddress&) const;
    uint32_t addressGeneration() const;   // счетчик изменений → WebServer перезапускает сокеты
};
```

Явный автомат состояний — подходящее место для исправления
[бага первой загрузки](#7-открытые-вопросы): несколько попыток STA перед порталом
и, при необходимости, одна самоперезагрузка. Здесь же позже появится выбор аплинка
для доступа в интернет через Ethernet.

## 5. Интерфейсы L2 (приложение)

### 5.1 `Controller` — единственный владелец состояния реле

```cpp
struct PriceSnapshot { bool valid; float eurKwh; uint32_t timestamp; uint32_t receivedMs; };
enum class Override : uint8_t { None, ManualOn, ManualOff, ForceOn };
struct ChannelStatus  {
    bool on;
    Override override;          // команда пользователя поверх автоматики
    uint32_t forceRemainingS;   // > 0 только при Override::ForceOn
    planner::Reason reason;     // причина автоматики (когда override == None)
};

class Controller {
public:
    Controller(Clock&, Outputs&, ConfigRepository&, PriceSource&);
    void begin(const ChannelConfig loaded[kChannelCount]);
    void service();       // каждые loop(): граница 15 мин, ретрай 30 с, failsafe 1 ч,
                          // пересчет раз в минуту, прием результатов PriceSource

    // Команды (веб, консоль, кнопка)
    void setManual(uint8_t channel, bool on);
    void toggleManual(uint8_t channel);
    void clearManual(uint8_t channel);
    // Принудительное включение на config.forceOnHours, не глядя на цену.
    void startForceOn(uint8_t channel);
    void cancelForceOn(uint8_t channel);      // возврат к автоматике
    void requestPriceUpdate();
    ConfigError updateConfig(uint8_t channel, const ChannelConfig&);  // валидация+сохранение+сброс фиксаций

    // Только чтение
    const ChannelConfig&      config(uint8_t channel) const;
    ChannelStatus             status(uint8_t channel) const;
    const planner::ChannelPlan& plan(uint8_t channel) const;
    const PriceSeries&      forecast() const;
    PriceSnapshot           latestPrice() const;
    uint32_t                secondsToNextCheck() const;
};
```

Состояние, которое сейчас лежит в глобалах (`relayConfigs`, `relayStates`,
`manualOverrides`, `latestPrice*`, `forecastSlots`, `locked*`, `awaitingNewPriceTimestamp`,
`lastUpdateMillis` …), становится приватными полями `Controller`. Поведение этапа 1
(failsafe, сброс ручного режима на границе 15 минут, проверка «новый timestamp больше
старого») переезжает без изменений и получает тесты на ПК с фейковыми `Clock`/`PriceSource`.

Таймер принудительного включения считается по `Clock::uptimeMs()`, а не по epoch. Поэтому
он работает без NTP и без интернета. Нагрев не сбрасывается на границе 15 минут
и не отключается failsafe. Последняя команда пользователя побеждает: ручное вкл/выкл
отменяет принудительное включение, и наоборот. Правила зафиксированы в
docs/DECISIONS.md (D-006, D-007).

### 5.2 Веб

```cpp
struct HttpRequest {               // заполняет WebServer
    RequestLine line;
    char body[2048]; size_t bodyLen;
};
class Router {
public:
    Router(Controller&, NetworkManager&, WifiCredentialStore&);
    void handle(const HttpRequest&, arduino::Client& out);   // GET /, GET /chart.svg, POST /save, POST /force
};
class WebServer {
public:
    explicit WebServer(Router&);
    void service(const NetworkManager&);   // accept на Wi-Fi и Ethernet, перезапуск при смене IP
};
void renderSettingsPage(Print& out, const Controller&, uint8_t channel, uint8_t day,
                        bool wifiFields, const char* message);
void renderChartSvg(Print& out, const Controller&, uint8_t channel, uint8_t day, uint32_t now);
```

- `WiFiClient` и `EthernetClient` в ядре mbed оба наследуют `arduino::Client`,
  поэтому шаблоны `template <typename Client>` заменяются на `arduino::Client&`.
- Выбранные реле и день передаются в рендер **параметрами из query**, а не через
  глобалы `selectedRelayIndex`/`selectedChartDay`. Сейчас два браузера мешают друг другу,
  а консоль меняет выбор в вебе.
- `POST /save` больше не тянет прогноз синхронно (`applyCurrentPriceToConfiguredControl`):
  `Controller::updateConfig` пересчитывает план по уже загруженному прогнозу.
- Точка расширения под авторизацию/CSRF — проверка в начале `Router::handle`.

### 5.3 Консоль

`SerialConsole(Controller&, NetworkManager&)`: команды `status/update/on/off/auto`,
полный отчет и heartbeat раз в 10 с. Свой выбор реле для команд хранится внутри
консоли и не связан с вебом.

### 5.4 `main.cpp` после рефакторинга

```cpp
static SystemClock clock; static RelayOutputBank relays; static KvStorage kv;
static ConfigStore configStore(kv); static WifiCredentialStore wifiStore(kv);
static FetchWorker fetcher(clock);
static Controller controller(clock, relays, configStore, fetcher);
static NetworkManager network(wifiStore);
static Router router(controller, network, wifiStore); static WebServer web(router);
static SerialConsole console(controller, network);
static UserButton button; static StatusLed led; static Watchdog watchdog;

void setup() { /* load config → relays off → network.begin → controller.begin → watchdog.start */ }
void loop() {
    watchdog.kick();
    controller.service();            // failsafe работает в любом режиме сети
    console.service();
    switch (button.service(millis())) { case ShortPress: controller.toggleManual(0); break;
                                        case LongPress:  network.requestPortal();   break; }
    led.service(millis(), network.internetUp());
    network.service(millis());
    web.service(network);
    clock.service(network.internetUp());
}
```

`loop()` не делает ранних `return`: каждый модуль сам решает, что ему делать
в текущем режиме сети.

## 6. Порядок переноса (этап 2)

Каждый шаг — отдельная ветка и PR. Перенос кода не смешивается с изменением поведения.
После каждого шага: `pio run -e opta` и `pio test -e native`, затем проверка на железе
пользователем.

| Шаг | Ветка | Содержание | Поведение |
|---|---|---|---|
| 2a | `refactor/core-time-planner` | `env:native` + Unity; `lib/rigatime`, `lib/prices`, `lib/planner` (как есть, с текущими багами, обе функции: решение и график); характеризационные тесты текущего поведения | без изменений |
| 2b | `fix/planner` | `ChannelPlan` как единый источник для решения и графика; выбор по всему окну + `HourSelection`; исправление KI-002, KI-003, KI-005; тесты на баги | **меняется** |
| 2c | `feature/force-on` | старый «непрерывный блок» удаляется из планировщика (KI-004); новое «принудительное включение» по кнопке: `POST /force`, кнопки на странице, команды `force`; нейтральные тексты UI и логов (KI-014); таймер по `millis()` (SCENARIOS §6, D-006, D-007); пока в коде монолита рядом с ручным режимом | **меняется** |
| 2d | `refactor/config-price-relay` | `lib/channelconfig` (+ тесты миграции v2–v5), `lib/elering` (+ фикстуры), `ConfigStore`, `RelayOutputBank`, `UserButton`, `StatusLed`, `Log` | без изменений |
| 2e | `refactor/controller` | глобалы → `Controller` (вместе с ручным и принудительным режимами); тесты failsafe, расписания и таймера принудительного включения с фейками; KI-011, KI-013 | без изменений |
| 2f | `refactor/net-web` | `lib/httpparse`, `lib/dnspacket`, `NetworkManager`, `WebServer`/`Router`/страницы; выбор реле/дня через query (KI-010); место для исправления KI-001 | почти без изменений (выбор в вебе становится независимым) |
| 2g | `feature/async-fetch` | `FetchWorker` на `rtos::Thread`, неблокирующий NTP (KI-006) | **меняется** (loop не блокируется до 20 с) |
| 2h+ | `feature/*` | авторизация/CSRF (KI-007), интернет через Ethernet (KI-008), конфиг v6 с именами, типом нагрузки и минимальным временем работы (§8.3); интеграции умного дома (§8.4) | **меняется** |

Конфигурация PlatformIO для тестов:

```ini
[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17 -Wall
lib_deps = bblanchon/ArduinoJson @ ^7.0.0
```

Для `env:native` на Windows нужен компилятор хоста: MinGW-w64 (WinLibs, GCC 16.2)
установлен через winget, см. [DEVELOPMENT.md](DEVELOPMENT.md).

## 7. Открытые вопросы

1. ~~Семантика «непрерывного блока»~~ — решено, D-006/D-007: принудительное
   включение по кнопке на заданное время.
2. **Сохранение Wi-Fi без перезагрузки.** Сейчас после `/save` с новыми учетными данными
   вызывается `NVIC_SystemReset()`. С `NetworkManager` можно переподключаться без
   рестарта, но это изменение поведения — отложено до 2h+.
3. **Баг первой загрузки** (KI-001): нужен лог Serial первой загрузки. Исправление
   ложится в `NetworkManager::begin()`.
4. **Потоки.** `FetchWorker` в отдельном `rtos::Thread` проще конечного автомата,
   потому что TLS-handshake в `WiFiSSLClient::connect` блокирующий. Сетевой стек mbed
   потокобезопасен, но веб-сервер в основном потоке и fetch в рабочем будут делить
   сокеты/память TLS. Проверить на железе запас heap при одновременной работе.

## 8. Расширяемость: умный дом

Сейчас устройство управляет одним бойлером, но дальше планируется умный дом (D-010).
Ниже — что закладывается **сразу**, чтобы потом не переписывать, и что откладывается.

### 8.1 Термины

| Термин | Значение | В коде |
|---|---|---|
| Канал | логическая управляемая единица: настройки, состояние, план, команды пользователя | `ChannelConfig`, `ChannelStatus`, `ChannelPlan`, индекс `channel` |
| Выход | физический ключ: реле Opta D0–D3, позже — модули расширения | интерфейс `Outputs`, реализация `RelayOutputBank` |
| Нагрузка | что подключено к выходу: бойлер, свет, насос, вентиляция | `ChannelConfig::loadType` (с конфига v6) |
| Сценарий | правило, которое добавляет время работы канала | функция в `lib/planner` |
| Команда | действие пользователя поверх автоматики: ручной режим, принудительное включение | `Controller::setManual`, `startForceOn` |

### 8.2 Закладываем сейчас (входит в шаги 2a–2g без отдельной работы)

1. **Нейтральные имена** во всех новых модулях: канал, выход, работа, включение.
2. **Число каналов — одна константа** `kChannelCount`. Нигде в коде нет литерала `4`,
   все массивы, циклы, проверки диапазона и веб-страница строятся по ней.
3. **Выходы за интерфейсом `Outputs`.** `Controller` не знает про пины. Каналы на
   модулях расширения Opta (AUX) или внешних реле добавляются новой реализацией
   `Outputs` без изменения логики.
4. **Сценарий — независимая функция с единой сигнатурой:**
   ```cpp
   using ScenarioFn = void (*)(const ChannelConfig&, const PlanContext&, PlannerState&, ChannelPlan&);
   ```
   `buildPlan` проходит по таблице сценариев. Новый сценарий (расписание по
   времени без цены, по датчику) — одна функция, поле в конфиге и строка в таблице.
5. **`Controller` — единственная точка управления.** Веб, консоль и кнопка — адаптеры
   над его публичным API. Будущие MQTT, Home Assistant и REST — такие же адаптеры,
   без доступа к внутреннему состоянию.
6. **Уведомления об изменениях.**
   ```cpp
   struct ChannelListener { virtual void onChannelChanged(uint8_t channel, const ChannelStatus&) = 0; };
   void Controller::setListener(ChannelListener*);
   ```
   Сейчас слушатель — лог `[RELAY]`. Позже — публикация в MQTT, журнал событий, push.
7. **Решение не обязано зависеть от цены.** `planner::Reason` и `ChannelStatus`
   описывают причину общим образом. Канал без ценовых сценариев (например, свет по
   расписанию) — допустимая конфигурация, failsafe по цене к нему не применяется
   (уточняется при появлении таких каналов).

### 8.3 Конфиг v6 — одной миграцией

Чтобы не мигрировать flash несколько раз, в v6 сразу добавляются все известные поля:

```cpp
struct ConfigHeader { uint16_t version; uint8_t channelCount; uint8_t reserved; };
struct ChannelConfig {      // v6
    // ... все поля v5 ...
    char     name[24];         // «Бойлер», «Свет на крыльце»
    uint8_t  loadType;         // Generic, WaterHeater, SpaceHeating, Lighting, Pump, Ventilation
    bool     priceControl;     // false → цена не учитывается вовсе
    uint16_t minOnMinutes;     // минимальное время работы (защита нагрузки)
    uint16_t minOffMinutes;    // минимальная пауза
    uint8_t  forceOnHours;     // бывший consecutiveBlockHours
};
```

С заголовком `channelCount` формат перестает зависеть от числа реле. Миграция
v5 → v6 — в `channelconfig::decode`, с тестом на реальный блоб v5.

### 8.4 Откладываем (не делать без запроса пользователя)

| Возможность | Куда ляжет |
|---|---|
| MQTT и автообнаружение Home Assistant | новый адаптер в `src/integrations/` + `ChannelListener` |
| JSON API (`GET /api/state`, `POST /api/channel/N`) для приложений | `Router`, поверх API `Controller` |
| Входы Opta I1–I8 (выключатели, датчики 0–10 В) | модуль L1 `Inputs` + сценарии-условия в планировщике |
| Датчики температуры и условия «включить, если ниже T» | `Inputs` + `ScenarioFn` |
| Группы и сцены (несколько каналов одной командой) | `Controller` |
| Учетные записи и права | `Router` (вместе с KI-007) |

### 8.5 Ресурсы

Текущая сборка: RAM 14 % (73 КБ из 512 КБ), flash 64 % (505 КБ из 768 КБ).
Основной резерв — flash: HTML-страница и тексты занимают заметную часть. Перед
крупными функциями (MQTT, JSON API) стоит сжать страницу (gzip в flash) и проверить
запас heap при TLS (D-008).
