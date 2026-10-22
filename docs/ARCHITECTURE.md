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
   графике берутся из одного и того же `ChannelPlan` (`planner::buildPlan`, с шага 2b;
   до этого были две функции, и они расходились — KI-005).
6. **Перенос кода и изменение поведения — в разных ветках** (см. раздел 6).
7. **Устройство — контроллер нагрузок для умного дома, а не терморегулятор** (D-010).
   Логическая единица — **канал** (`Channel*`), физическое реле — **выход** (`Outputs`).
   Сейчас канал = реле Opta, а нагрузка — бойлер. Слово «нагрев» в именах не используется.
   Что закладывается для расширения — раздел 8.

## 2. Слои и состав модулей

```
L3  src/main.cpp                    сборка объектов, загрузка настроек, повтор NTP, setup()/loop()
L2  lib/controller Controller        расписание цены, failsafe, ручной режим, принудительное
                                    включение, решения автоматики (с 2e, D-022)
    src/web/   WebServer (запрос + маршруты), SettingsPage, HistoryPage, ClientWriter (с 2f)
    src/console/ SerialConsole: команды, отчет, статус раз в 10 с (с 2f)
L1  src/platform/ Log (+SerialLogSink), SystemClock (NTP), KvStorage, RelayOutputBank,
                  UserButton, StatusLed, Watchdog (с 2f)
    src/net/   NetworkManager, ListeningSockets, MdnsResponder, CaptiveDns,
               EleringPriceSource (с 2f); FetchWorker (поток запросов), NtpQuery (с 2g);
               WifiConnector (поток подключения Wi-Fi, D-029)
L0  lib/rigatime     даты, DST Europe/Riga, разбор/формат локального времени
    lib/prices       PriceSeries (15-мин слоты), агрегация в часы
    lib/channelconfig  ChannelConfig: значения по умолчанию, валидация, кодек/миграция блоба
    lib/planner      сценарии → ChannelPlan, решение для текущего момента
    lib/elering      разбор JSON Elering (текущая цена и прогноз) + валидация
    lib/httpparse    строка запроса, query, urlencoded-формы, форма → ChannelConfig
    lib/dnspacket    сборка ответов captive DNS и mDNS
    lib/history      история переключений: кольцевой буфер в RAM (шаг 2b, D-019)
    lib/forceon      таймер принудительного включения по millis() (шаг 2c, D-006/D-007)
    lib/configstore  ConfigStore, WifiCredentialStore поверх KvBackend (шаг 2d)
    lib/userbutton   антидребезг, короткое/долгое нажатие кнопки USER (шаг 2d)
```

Шаг 2d перенес логику хранения настроек и кнопки в `lib/` (а не в `src/platform/`,
как планировалось): так загрузку настроек из flash — все ветки миграции и отказов —
проверяют тесты с фейковым KVStore. В `src/platform/` остались тонкие обертки над
железом: `KvStorage` (`kv_*`), `UserButton` (пин `BTN_USER`).

Шаг 2e по той же причине положил `Controller` в `lib/controller`, а не в `src/app/`
(D-022): тесты `env:native` проверяют failsafe, расписание и таймер с фейковыми портами.

Шаг 2f разложил остаток монолита по `src/net`, `src/web`, `src/console` (D-024).
HTTP-сокеты порта 80 и mDNS принадлежат `NetworkManager`, а не `WebServer`: их запуск и
остановка привязаны к переходам сети. `WebServer` получает уже принятое соединение.

Откуда что берется (имена функций монолита до рефакторинга; всё перенесено, ✅ —
на шагах 2d–2f, L0 — на шаге 2a):

| Модуль | Текущий код в `src/main.cpp` |
|---|---|
| `rigatime` | `daysFromCivil`, `civilFromDays`, `lastSundayOfMonth`, `rigaDaylightSaving`, `parseRigaDateTime`, `formatRigaDateTime`, `parseTimeOfDay`, `formatTimeOfDay`, `localHourAndDay` |
| `prices` | `ForecastSlot`, `PriceHour`, `buildPriceHours` |
| `channelconfig` | `DeviceConfig`, `Legacy*`, `setDefaultDeviceConfig`, `importLegacyDeviceConfig`, `isValidDeviceConfig`, разбор блоба из `loadDeviceConfigs` |
| `planner` | `nightWindowKey`, `choose*`, `getNextWakeupEpoch`, `isWakeupHeatingTime`, `getAutomaticRelayDecision`, `buildRelayWorkSlotsForChart`, `relayModeName` |
| `elering` | разбор JSON из `fetchNordPoolPrice` и `fetchPriceForecast`, расчет диапазона дат прогноза |
| `httpparse` | `decodeFormValue`, `formField`, `parseFormFloat`, `parseFormInteger`, разбор формы `/save` и query |
| `dnspacket` | сборка пакетов из `serviceMDNSOn` и `handleDnsRequests` |
| `SystemClock` ✅ | `syncNetworkTime`, `getCurrentEpochSeconds` (повтор NTP — пока `servicePriceSchedule` в `main.cpp`); с 2g запрос NTP — `NtpQuery` в потоке `FetchWorker` |
| `ConfigStore` / `WifiCredentialStore` ✅ | `kv_get`/`kv_set` из `load/saveDeviceConfigs`, `load/saveWifiCredentials` |
| `RelayOutputBank` ✅ | пины, светодиоды, лог `[RELAY]` (историю пишет `Controller`) |
| `UserButton`, `StatusLed` ✅ | антидребезг, короткое/долгое нажатие, мигание |
| `NetworkManager` ✅ | `startWifiManager`, `stopWifiManager`, `servicePortalReconnect`, `serviceWiredNetwork`, `startSettingsWebServer`, `startWiredWebServer`, `service*WebServer` (прием соединений), `configureEthernetHostname`, переподключение и долгое нажатие USER из `loop`, Wi-Fi часть `setup` |
| `ListeningSockets` ✅ | `CheckedWiFiServer`, `CheckedEthernetServer` |
| `MdnsResponder`, `CaptiveDns` ✅ | `serviceMDNSOn`, `serviceMDNS`, `handleDnsRequests` |
| `EleringPriceSource` ✅ | `fetchNordPoolPrice`, `fetchPrices`, `readHttpResponseBody`, TLS-клиент; с 2g — синхронный исполнитель внутри потока `FetchWorker` (отдельного `HttpsFetcher` нет, D-026) |
| `WebServer` ✅ | `readHttpLine`, `readHttpBody`, `sendRedirectHome`, `handlePortalClient` (строка запроса и `Content-Length` — `httpparse`) |
| `SettingsPage`, `HistoryPage`, `ClientWriter` ✅ | `sendPortalPage`, `printChartData`, `kChartScript`, `printForceOnPanel`, `htmlEscape`, `sendHistoryPage`, `StringPageWriter`, `BufferedClientWriter`, `writeClientData` |
| `Watchdog` ✅ | `kickWatchdog`, `startHardwareWatchdog` |
| `Controller` ✅ | `servicePriceSchedule` (часть после NTP), `updatePriceAndRelay`, `fetchPriceForecast`, `getAutomaticRelayDecision`, `serviceScheduledControl`, `servicePriceFailsafe`, `applyCurrentPriceToConfiguredControl`, `setRelay`, принудительное включение, `relayModeName`, `manualOverrides`, `latestPrice*`, `forecast`, `plannerStates` |
| `SerialConsole` ✅ | `handleSerialCommands`, `printStatusReport`, `printSwitchHistory`, heartbeat из `loop` |

## 3. Интерфейсы L0 (чистое ядро, покрывается тестами)

### 3.0 Состояние после шагов 2a и 2b

Слой L0 перенесен целиком (D-011), на шаге 2b переписан планировщик (D-012…D-019),
на шаге 2c из него удален «непрерывный блок» и добавлен `forceon` (D-006, D-007, D-021).
Источник правды по сигнатурам — заголовки `lib/<модуль>/*.h`. Ниже в 3.1–3.8 —
**целевые** интерфейсы; часть из них появится на следующих шагах:

| Модуль | Реализовано | Появится позже |
|---|---|---|
| `rigatime` | `daysFromCivil`, `civilFromDays`, `lastSundayOfMonth`, `isDaylightSaving`, `toLocal`, `localHourAndDay`, `parse/formatTimeOfDay`, `parse/formatLocalDateTime` (текст + длина); с 2b — `localDay`, `localToUtc(day, minute)`, `localDayStartUtc` | `utcOffsetSeconds`, `nextLocalTime` — по необходимости |
| `prices` | `PriceSeries` на 300 интервалов (вчера–завтра): `append` (емкость + строгий порядок), `indexAt`, `lowerBound`, `coverageEnd`, `replaceKeepingOlder`, `prependOlder`, `buildHours` (часы по средней цене; с шага 2c планировщиком не используется) | — |
| `channelconfig` | `ChannelConfig` (v5; поле `holidayHeatHours` → `holidayOnHours`), `setDefaults`, `isValid`, `importLegacy`, `blobKind`, `decode` (+ маска сброшенных каналов) | `validate()` с кодом ошибки, `encode`, формат v6 (§8.3) |
| `planner` | с 2b: `buildPlan` → `ChannelPlan` (маска сценариев на интервал + `PlanTrace`), `decideNow` → `Decision{on, Reason}`, `PlannerState`/`SlotSelection` (фиксации), `slotOn`/`slotReason`/`reasonName`, окна (`activeNightWindow`, `localDayWindow`, `holidayWindow`), `selectCheapest` (с D-027 — с исключенными интервалами: умный порог и ночь не делят интервалы); без изменений — `ceilingExceeded`, `holidayExpired`, `modeName` (с 2c — `ModeInputs::forceOn`); с 2c удалены `BlockLock`, `chooseConsecutiveHours`, `Reason::Block`, `slot::kBlock` | таблица сценариев `ScenarioFn` (§8.2) |
| `elering` | `parseCurrent` (`now == 0` — без проверки актуальности), `parseForecast` (с 2b разбирает только `data.lv`), `forecastPath`, `dayPricesPath` (2b), `jsonObjectBounds`; с 2g `priceIsCurrent` (проверка актуальности отдельно, для `Controller`) | — |
| `httpparse` | `formField`, `parseFormFloat`, `parseFormInteger`, `queryChannel`, `queryChartDay` (с 2b: −1…1), `parseSettingsForm` → `FormError` (с 2c поле `force_hours`), `parseForceForm` (2c); с 2f `parseRequestLine` (границы метода и пути в строке, как в монолите) и `parseContentLength`; с KI-010 `pageSelection`, `formatHomeLocation` | — |
| `dnspacket` | `buildCaptiveDnsAnswer` (ответ в том же буфере), `buildMdnsAnswer` → `MdnsReply` со статусом для лога | — |
| `history` | с 2b: `SwitchHistory` (512 событий), `causeName`, `formatEventTime`; с 2c причины `ForceOn`, `NoPrice` | — |
| `forceon` | с 2c: `ForceOnTimer` (`start`, `stop`, `expire`, `remainingMs`, безопасно к переполнению `millis()`), `formatRemaining`; с 2e таймеры — внутри `Controller` | — |
| `configstore` | с 2d: `KvBackend` (интерфейс KVStore), `ConfigStore` (`load` → `LoadReport` с исходом для лога, `save`), `loadSucceeded`, `WifiCredentialStore` (`load` с проверкой размера и строк, `save`); с 2e `Controller` пишет через `ConfigStore` напрямую, без `ConfigRepository` (D-022) | — |
| `userbutton` | с 2d: `PressDetector::update(nowMs, pressed)` → `Event` `None / Pressed / ShortPress / LongPress` (антидребезг > 50 мс, долгое ≥ 5 с) | — |

Как `Controller` (до шага 2e — `main.cpp`) вызывает планировщик (порядок важен для логов
и записи во flash):

1. ручной режим или принудительное включение (`channelOverridden`) → решения нет;
2. `planner::ceilingExceeded` → выключить, лог `[DECISION]`; план все равно строится,
   чтобы фиксации следили за окнами и настройками;
3. `planner::buildPlan` → контроллер печатает строки `[WARN]`/`[DECISION]` по
   `ChannelPlan::trace` (при пересчете раз в минуту — только если трасса изменилась, D-023);
   `planner::decideNow` → решение и причина; `Controller::setOutput` пишет смену состояния
   в историю.

Истечение отпуска (`planner::holidayExpired` → снять флажок, записать flash, лог
`[HOLIDAY]`) — отдельный шаг `Controller::expireHolidays` в начале
`servicePriceSchedule`, а не часть решения (KI-013 исправлен). Решение от него не зависит:
планировщик сам не учитывает отпуск с прошедшей датой возвращения.

График строит тот же план (`buildPlan`) и закрашивает интервалы, для которых
`slotOn(mask)` истинно. Рабочий `ChannelPlan` (~330 байт) один на все каналы:
решение и график строятся в основном потоке по очереди.

Целевые интерфейсы:

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
constexpr uint32_t kSlotSeconds = 900;
constexpr size_t kMaxSlots = 300;      // вчера, сегодня, завтра по 15 мин + запас на DST
constexpr size_t kMaxHours = kMaxSlots / 4 + 2;

struct PriceSlot { uint32_t start; float eurKwh; };
struct PriceHour { uint32_t start; float avgEurKwh; uint8_t quarters; };  // планировщиком не используется с 2c

class PriceSeries {
public:
    void   clear();
    bool   append(uint32_t start, float eurKwh);   // false: переполнение/не по порядку
    size_t size() const;
    const PriceSlot& operator[](size_t i) const;
    int    indexAt(uint32_t utc) const;             // слот, содержащий момент, или -1
    size_t lowerBound(uint32_t utc) const;          // первый слот не раньше момента
    uint32_t coverageEnd() const;                   // конец последнего слота
    void   replaceKeepingOlder(uint32_t keepFrom, const PriceSeries& newer);  // свежий прогноз + вчера
    void   prependOlder(const PriceSeries& older);  // догруженные вчерашние цены
    size_t buildHours(PriceHour* out, size_t cap) const;
};
```

### 3.3 `lib/channelconfig/ChannelConfig.h`

```cpp
constexpr uint8_t  kChannelCount = 4;
constexpr uint16_t kConfigVersion = 5;

struct ChannelConfig { /* поля формата v5, см. lib/channelconfig/ChannelConfig.h */ };
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

36 байт подтверждены `static_assert` в сборке под Opta и под ПК, а смещение каждого
поля — тестом `test_blob_layout_is_frozen`. Новые поля
(например, минимальное время работы реле) — только через `kConfigVersion = 6` и ветку
миграции в `decode`; тест миграции фиксирует старые блобы байт в байт.

### 3.4 `lib/planner/Planner.h` — главный модуль

Реализовано на шаге 2b (подробные комментарии — в `lib/planner/Planner.h`):

```cpp
namespace planner {

constexpr float    kShiftToleranceEurKwh = 0.01f;       // сдвиг к концу окна, D-016
constexpr uint32_t kHolidayWindowSeconds = 10 * 3600;   // окно отпуска, D-015

enum class Reason : uint8_t {
    None, Threshold, Smart, Wakeup, Night, Block, Holiday,   // почему включено / ничего
    HolidayOff, Ceiling                                      // почему выключено принудительно
};
// «Принудительное включение» в планировщике отсутствует: это команда пользователя
// с таймером, ее ведет Controller (см. 5.1 и docs/DECISIONS.md, D-006).

// План одного канала на весь прогноз: маска сценариев на каждый 15-минутный интервал.
struct ChannelPlan {
    size_t    slotCount;
    uint8_t   mask[kMaxSlots];   // slot::kThreshold | kSmart | … | kHolidayPeriod | kCeiling
    PlanTrace trace;             // статусы сценариев и нехватка интервалов для лога
};

// Зафиксированный выбор интервалов текущего окна, чтобы план не «плыл».
struct SlotSelection {
    bool tracked, enabled, built; uint8_t hours; uint32_t param;   // параметры при выборе
    uint32_t windowStart, windowEnd, earliest;                      // окно и первый кандидат
    uint16_t requested, available;
    uint32_t bits[4];            // бит i → интервал firstSlot + i·15 мин выбран (до 128)
};
struct PlannerState { SlotSelection smart, night, holiday; };

void buildPlan(const ChannelConfig& cfg, const PriceSeries& prices, uint32_t now,
               PlannerState& state, BlockLock& lock, ChannelPlan& out);

struct Decision { bool on; Reason reason; };
// Текущая цена из /current авторитетнее прогноза для аварийного предела и порога.
Decision decideNow(const ChannelConfig& cfg, const ChannelPlan& plan, const PriceSeries& prices,
                   uint32_t now, float currentEurKwh);

bool slotOn(uint8_t mask);          // для графика
const char* reasonName(Reason r);   // «Включение ночью», «Аварийный предел цены», ...
}
```

Приоритет внутри планировщика: аварийный предел → отпуск →
(порог ИЛИ умный порог ИЛИ пробуждение ИЛИ ночь ИЛИ блок). Над планировщиком в `Controller`
стоят команды пользователя: принудительное включение и ручной режим (см. docs/SCENARIOS.md).

Побочный эффект «выключить отпуск и записать во flash» выполняет
`Controller::expireHolidays` по `planner::holidayExpired()`, отдельно от решения
(ветка `fix/controller-side-effects`; до нее — внутри функции решения, KI-013).

`PlannerState` живет в RAM, массив на все каналы внутри `Controller`. Выбор считается по **всему окну**, включая прошедшие интервалы, и делается,
когда прогноз покрывает окно целиком (D-012). Прогноз хранит и вчерашние цены, поэтому
после перезагрузки выбор восстанавливается тем же, в том числе для ночи, начавшейся
вчера. Сохранять состояние во flash не нужно. Смена параметров сценария посреди окна
переносит уже отработанные интервалы и добирает остаток (D-013).

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
// Одни местные сутки — для догрузки вчерашних цен после запуска.
bool dayPricesPath(int64_t localDay, char* out, size_t cap);
constexpr const char* kHost = "dashboard.elering.ee";
constexpr const char* kCurrentPath = "/api/nps/price/lv/current";
}
```

ArduinoJson header-only и собирается под `native`, поэтому разбор тестируется на
сохраненных реальных ответах API (`test/fixtures/EleringFixtures.h`).

### 3.6 `lib/httpparse/HttpParse.h`

```cpp
// Реализовано на шаге 2f: границы в исходной строке, без копирования и ограничения длины
struct RequestLine { size_t methodLength; bool hasPath; size_t pathStart, pathLength; };
void parseRequestLine(const char* line, RequestLine& out);
bool parseContentLength(const char* headerLine, long& value);   // имя без учета регистра, значение как atol

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

### 3.8 `lib/history/SwitchHistory.h`

```cpp
namespace history {
enum class Cause : uint8_t { Startup, Threshold, Smart, Wakeup, Night, Block, Holiday,
                             HolidayOff, NoScenario, Ceiling, Failsafe, Manual };
struct SwitchEvent { uint32_t epoch; uint32_t uptimeSeconds; uint8_t channel; bool on; Cause cause; };
class SwitchHistory {                 // кольцевой буфер, 512 событий, только RAM (D-019)
public:
    void record(uint32_t epoch, uint32_t uptimeSeconds, uint8_t channel, bool on, Cause);
    void fillMissingEpochs(uint32_t nowEpoch, uint32_t nowUptimeSeconds);  // после первой NTP
    size_t size() const; uint32_t dropped() const;
    const SwitchEvent& newest(size_t index) const;   // 0 — самое новое
};
const char* causeName(Cause);
void formatEventTime(const SwitchEvent&, char* out, size_t cap);   // "20.10.2026 03:15:07"
}
```

С шага 2e историю пишет `Controller` (до этого — `setRelay` в `main.cpp`). `ChannelListener`
(§8.2 п. 6) появится вместе со вторым потребителем событий — MQTT (D-022).

## 4. Интерфейсы L1 (платформа)

Абстрактные классы вводятся только там, где `Controller` нужно подменить в тесте на ПК.
Остальное — конкретные классы.

```cpp
// src/platform/Log.h — замена сотен строк Serial.print (с 2d)
void logLine(const char* tag, const char* fmt, ...);     // "[NTP] ..." через vsnprintf
```

Имя `logLine`, а не `logf`: `logf` — функция из `<math.h>`. Строка до 256 байт с тегом.
Через `logLine` пишут модули, перенесенные с шага 2d; остальной код переходит на него
по мере переноса. Дробные числа пока печатаются `Serial.print(value, digits)`:
поддержка `%f` в printf ядра mbed на железе не проверена.

```cpp
// Порты Controller (lib/controller/ControllerPorts.h), с шага 2e
class Clock {
    virtual bool     synced() const = 0;
    virtual uint32_t nowUtc() const = 0;     // до синхронизации — секунды от запуска
    virtual uint32_t uptimeMs() const = 0;   // millis()
};
class Outputs {
    virtual void set(uint8_t channel, bool on) = 0;
    virtual bool get(uint8_t channel) const = 0;
};
// С шага 2g асинхронный (D-008, D-026): start* ставит запрос, результат забирает
// Controller::servicePriceFetch() через poll(). Реализация — FetchWorker, печатает ход
// запроса через свой буфер строк.
enum class FetchStatus : uint8_t { Idle, Busy, Done, Failed };
class PriceSource {
    virtual bool startCurrent() = 0;                                   // false — занято
    virtual bool startPrices(const char* path, PriceSeries& out) = 0;  // out — до результата у источника
    virtual FetchStatus poll(float& eurKwh, long& timestamp) = 0;     // Done/Failed — один раз
};
// Лог: перегрузки print/println как у Serial, на устройстве — SerialLogSink.
class LogSink { /* printText, printSigned, printUnsigned, printFloat */ };
```

Отдельного `ConfigRepository` нет: контроллер пишет flash через `configstore::ConfigStore`,
в тестах — поверх фейкового `KvBackend` (D-022).

Порт сделан проще первоначального плана (`requestCurrent`/`requestForecast`/`pollCurrent`/
`pollForecast`/`busy`): путь запроса прогноза формирует `Controller`, а одного `poll`
хватает, потому что запросы идут строго по одному. Актуальность цены проверяет
`Controller` по часам в момент результата (`elering::priceIsCurrent`).

Конкретные реализации:

| Класс | Ответственность |
|---|---|
| `SystemClock : Clock` | с 2g: `apply(epoch, atMs)` — принять ответ NTP от `FetchWorker`; первая синхронизация проставляет время событиям истории; повтор раз в минуту и ресинхронизация раз в 6 ч — в `main.cpp` |
| `RelayOutputBank : Outputs` | пины D0–D3 и светодиоды, лог переключений; с 2d — `begin`, `set`, `get`, с 2e — интерфейс `Outputs` |
| `KvStorage : configstore::KvBackend` | с 2d: обертка `kv_get`/`kv_set`/`kv_get_info` с кодами ошибок |
| `ConfigStore` | с 2d в `lib/configstore`: `load()` через `channelconfig::decode`, `save()` блоба как есть; `Controller` использует его напрямую (D-022) |
| `WifiCredentialStore` | с 2d в `lib/configstore`: `load(WifiCredentials&)`, `save(...)` |
| `UserButton` | с 2d: `service(nowMs)` → событие `None / Pressed / ShortPress / LongPress(5 с)`, логика — `lib/userbutton` |
| `StatusLed` | с 2d: `service(nowMs, connected)` |
| `Watchdog` | с 2f: `kickWatchdog()` (inline), `startHardwareWatchdog()` |
| `EleringPriceSource` | с 2f в `src/net/`: синхронные `fetchCurrent` и `fetchPrices` поверх своего `WiFiSSLClient`, лимиты заголовков/тела, таймауты; с 2g вызывается только из потока `FetchWorker`, пишет лог в его буфер, watchdog не сбрасывает. Отдельного `HttpsFetcher` нет (D-026) |
| `NtpQuery` | с 2g: `queryNtpTime(udp, …, epoch, atMs)` — блокирующий запрос NTP для потока `FetchWorker` |
| `ListeningSockets` | с 2f: `CheckedWiFiServer::beginOnAddress`, `CheckedEthernetServer::beginListening` — порт 80 с кодом ошибки Mbed |
| `FetchWorker : PriceSource` | с 2g: `rtos::Thread` (стек 16 КБ, приоритет `loop()`), задания цены и NTP (`startTimeSync`/`pollTimeSync`), результат под мьютексом; `LineLog` — буфер строк лога, печатает основной поток (`flushLog`); `stuck()` — задание дольше 3 мин (D-026) |
| `WifiConnector` | `rtos::Thread` (стек 8 КБ): одна попытка `WiFi.begin()` за раз, `start`/`poll` → `Idle / Busy / Connected / Failed`, длительность попытки, `stuck()` — дольше 3 мин (D-029) |
| `NetworkManager` | конечный автомат сети (ниже) |
| `MdnsResponder`, `CaptiveDns` | с 2f: UDP-сокет + `dnspacket`; `begin`, `stop`, `service` (один пакет за вызов); mDNS — по объекту на Wi-Fi и Ethernet |

### 4.1 `NetworkManager`

Выделен на шаге 2f переносом без изменения поведения, автомат Wi-Fi — D-029
(ветка `fix/network-startup`):

```cpp
class HttpClientHandler { virtual void handleClient(arduino::Client&) = 0; };  // WebServer

class NetworkManager {
public:
    explicit NetworkManager(WifiCredentialStore&);
    void begin(HttpClientHandler&);   // setup(): поток WifiConnector, DHCP без ожидания,
                                      // первая попытка Wi-Fi через 3 с; не ждет сеть
    bool service(uint32_t nowMs);     // loop(): шаг автомата; true — Wi-Fi подключен,
                                      // иначе остаток loop() пропускается
    void requestPortal();             // долгое нажатие USER
    bool wifiConnected() const;
    bool wifiFieldsVisible() const;
    bool stuck(uint32_t nowMs);       // попытка Wi-Fi дольше 3 мин → main.cpp перезагружает
};
```

Состояния Wi-Fi и серии попыток:

| Состояние | Что происходит | Выход |
|---|---|---|
| `Waiting` | пауза перед попыткой (после запуска — до 3 с или до адреса Ethernet) | конец паузы → `Connecting` |
| `Connecting` | `WiFi.begin()` в потоке `WifiConnector`, основной поток WiFi не трогает | успех → `Online`; неудача → по серии |
| `Online` | подключен; mDNS и HTTP-сервер Wi-Fi | статус не `WL_CONNECTED` → серия `Reconnect` |
| `Portal` | точка настройки, captive DNS | раз в 5 мин без активности → серия `PortalReturn` |
| `Off` | модуль не найден или смена режима | — |

| Серия | Неудачная попытка |
|---|---|
| `Boot` | паузы 5, 15, 30, 60 с; после 5-й — точка настройки или поля Wi-Fi по Ethernet |
| `Reconnect` | пауза 10 с; после 3-й — точка настройки, если нет страницы по Ethernet |
| `PortalReturn` | снова точка настройки |

Сокеты (HTTP-порт 80, mDNS Wi-Fi и Ethernet, captive DNS) — члены `NetworkManager`.
Запуск и остановка точки настройки (`WiFi.beginAP`) — в основном потоке и только когда
попытка не идет; долгое нажатие во время попытки откладывается до ее итога.

Что еще можно добавить позже:

```cpp
    void onCredentialsSaved();            // вместо немедленного NVIC_SystemReset (позже)
    bool internetUp() const;              // есть маршрут к Elering
    // Интерфейсы, на которых должны слушать HTTP/mDNS; меняются при смене IP.
    bool wifiAddress(IPAddress&) const;
    bool ethernetAddress(IPAddress&) const;
    uint32_t addressGeneration() const;   // счетчик изменений → WebServer перезапускает сокеты
```

Здесь же позже появится выбор аплинка для доступа в интернет через Ethernet (KI-008).

## 5. Интерфейсы L2 (приложение)

### 5.1 `Controller` — единственный владелец состояния реле

Реализован на шаге 2e в `lib/controller/Controller.h` как перенос без изменения
поведения. Отличия от целевого интерфейса ниже: конструктор принимает еще
`SwitchHistory&` и `LogSink&`; вместо одного `service()` — `serviceFailsafe()`,
`serviceForceOn()` и `servicePriceSchedule()` на прежних местах `loop()`; команды
`setManual(channel, on, announce)`, `clearManual`, `startForceOn`, `stopForceOn`,
`updatePrice`, `saveConfig` + `applyConfiguredControl` (вместо `updateConfig`; решение без
запроса прогноза, KI-011); с 2g `servicePriceFetch()` — этапы фонового запроса цены и
`priceFetchInProgress()`;
для страницы и консоли — `config`, `outputOn`, `manual`, `forceOnActive`,
`forceOnRemainingMs`, `modeName`, `hasLatestPrice`, `latestPriceEurKwh`, `forecast`,
`priceRetryPending`, `chartPlan(channel, now)`. Тесты — `test/test_controller`.

Целевой интерфейс:

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

Состояние, которое до шага 2e лежало в глобалах `main.cpp` (`relayConfigs`,
`manualOverrides`, `forceOnTimers`, `latestPrice*`, `forecast`, `plannerStates`,
`awaitingNewPriceTimestamp`, `lastUpdateMillis` …), теперь — приватные поля `Controller`.
Поведение этапа 1 (failsafe, сброс ручного режима на границе 15 минут, проверка «новый
timestamp больше старого») перенесено без изменений и покрыто тестами на ПК с фейковыми
`Clock`/`PriceSource`.

Таймер принудительного включения считается по `Clock::uptimeMs()`, а не по epoch. Поэтому
он работает без NTP и без интернета. Принудительное включение не сбрасывается на границе 15 минут
и не отключается failsafe. Последняя команда пользователя побеждает: ручное вкл/выкл
отменяет принудительное включение, и наоборот. Правила зафиксированы в
docs/DECISIONS.md (D-006, D-007).

### 5.2 Веб

Шаг 2f (`src/web/`): `WebServer : HttpClientHandler` читает строку запроса, заголовки
и тело формы и выбирает маршрут; страницы — свободные функции
`sendSettingsPage(client, controller, clock, channel, chartDay, wifiFields, message)` и
`sendHistoryPage(client, history)`. Реле и сутки страницы берутся из query запроса
(`httpparse::pageSelection`), после POST — редирект на `/?relay=N`
(`httpparse::formatHomeLocation`); состояния между запросами нет (KI-010, D-025).
Отдельного `Router` нет: маршрутов четыре. Целевой вариант:

```cpp
struct HttpRequest {               // заполняет WebServer
    RequestLine line;
    char body[2048]; size_t bodyLen;
};
class Router {
public:
    Router(Controller&, NetworkManager&, WifiCredentialStore&);
    // GET /, GET /history, POST /save, POST /force (график — данные внутри страницы, D-020)
    void handle(const HttpRequest&, arduino::Client& out);
};
class WebServer {
public:
    explicit WebServer(Router&);
    void service(const NetworkManager&);   // accept на Wi-Fi и Ethernet, перезапуск при смене IP
};
void renderSettingsPage(Print& out, const Controller&, uint8_t channel, int8_t day,
                        bool wifiFields, const char* message);
void renderChartData(Print& out, const Controller&, uint8_t channel, uint32_t now);  // var CHART
void renderHistoryPage(Print& out, const history::SwitchHistory&);   // потоком, без Content-Length
```

- `WiFiClient` и `EthernetClient` в ядре mbed оба наследуют `arduino::Client`,
  поэтому шаблоны `template <typename Client>` заменяются на `arduino::Client&`.
- Выбранные реле и день передаются в рендер **параметрами из query**, а не через
  глобалы `selectedRelayIndex`/`selectedChartDay` (сделано, ветка `fix/web-selection`).
- `POST /save` больше не тянет прогноз синхронно (`applyCurrentPriceToConfiguredControl`):
  `Controller::updateConfig` пересчитывает план по уже загруженному прогнозу.
- Точка расширения под авторизацию/CSRF — проверка в начале `Router::handle`.

### 5.3 Консоль

С шага 2f — `src/console/SerialConsole`: `SerialConsole(Controller&, Clock&,
SwitchHistory&)`, методы `service()` (команды `status/update/on/off/auto/
force/history`), `serviceHeartbeat(nowMs)` (статус раз в 10 с), `printStatusReport()`.
Состояние Wi-Fi консоль читает у `WiFi` напрямую. Свой выбор реле для команд хранится
внутри консоли и не связан с вебом (KI-010, D-025).

### 5.4 `main.cpp` после рефакторинга

С шага 2f `main.cpp` (~240 строк) — сборка объектов, `loadDeviceConfigs` (лог исхода
загрузки), повтор NTP и `setup()`/`loop()`. С 2g здесь же `serviceFetchResults()`: до ранних
`return` забирает ответ NTP, ведет этапы запроса цены, печатает лог потока и
перезагружает устройство, если запрос завис (D-026). В `loop()` остался один ранний `return`:
`if (!network.service(now)) return;` — без Wi-Fi статус и расписание цены пропускаются,
как в монолите. Целевой вид:

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
| 2a ✅ | `refactor/core-l0` | `env:native` + Unity; **весь слой L0** (D-011): `lib/rigatime`, `lib/prices`, `lib/channelconfig` (+ тесты миграции v2–v5), `lib/planner` (как есть, с текущими багами, обе функции: решение и график), `lib/elering` (+ реальные фикстуры), `lib/httpparse`, `lib/dnspacket`; 80 характеризационных тестов | без изменений |
| 2b ✅ | `fix/planner` | `ChannelPlan` как единый источник для решения и графика; выбор 15-минутных интервалов по всему окну с фиксацией (`PlannerState`), смена настроек посреди окна, сдвиг к концу окна в пределах 1 цента, окно отпуска 10 ч, пробуждение до минуты (D-012…D-017); прогноз вчера–завтра и сохранение прежнего при сбое, вкладка «Вчера» (D-018); история переключений `lib/history`, страница `/history`, команда `history` (D-019); исправление KI-002, KI-003, KI-005 | **меняется** |
| 2c ✅ | `feature/force-on` | старый «непрерывный блок» удаляется из планировщика (KI-004); новое «принудительное включение» по кнопке: `POST /force`, кнопки на странице, команды `force`; нейтральные тексты UI и логов (KI-014); таймер по `millis()` в `lib/forceon` (SCENARIOS §6, D-006, D-007, D-021); команды пока в коде монолита рядом с ручным режимом | **меняется** |
| 2d ✅ | `refactor/config-price-relay` | `lib/configstore`: `ConfigStore` (поверх `channelconfig::decode`) и `WifiCredentialStore` + тесты с фейковым KVStore; `lib/userbutton` + тесты; `src/platform/`: `Log` (`logLine`), `KvStorage`, `RelayOutputBank`, `UserButton`, `StatusLed` | без изменений |
| 2e ✅ | `refactor/controller` | глобалы → `lib/controller` (`Controller` с ручным и принудительным режимами, порты `Clock`/`Outputs`/`PriceSource`/`LogSink`), `src/platform/SystemClock` (NTP), `SerialLogSink`; тесты failsafe, расписания и таймера принудительного включения с фейками (D-022) | без изменений |
| 2e-fix ✅ | `fix/controller-side-effects` | KI-011 (`/save` без синхронного прогноза), KI-013 (истечение отпуска вне функции решения, лог плана при пересчете раз в минуту только при изменении), D-023 | **меняется** |
| 2f ✅ | `refactor/net-web` | разбор строки запроса и заголовков в `lib/httpparse` (+ тесты), `MdnsResponder`/`CaptiveDns` поверх `lib/dnspacket`, `NetworkManager` (с сокетами), `WebServer` и страницы, `SerialConsole`, `EleringPriceSource`, `Watchdog` в `src/`; место для исправления KI-001 — `NetworkManager::begin()` (D-024) | без изменений |
| 2f-fix ✅ | `fix/web-selection` | выбор реле и дня через query, редирект на `/?relay=N`, свой выбор у консоли (KI-010, D-025) | **меняется** (выбор в вебе независимый) |
| 2g ✅ | `feature/async-fetch` | `FetchWorker` на `rtos::Thread`, NTP в том же потоке, асинхронный `PriceSource` и этапы запроса в `Controller`, цена принимается вместе с прогнозом (с D-030 — сразу), перезагрузка при зависшем запросе (KI-006 частично, D-026) | **меняется** (loop не блокируется до 20 с) |
| 2g-fix | `fix/price-future-lead`, `fix/network-startup` | KI-015 (цена до 5 мин впереди часов); KI-012 (интервалы `millis()` в Ethernet); KI-001 и остаток KI-006: поток `WifiConnector`, автомат Wi-Fi, 5 попыток после запуска, DHCP без ожидания (D-029) | **меняется** |
| 2h+ | `feature/*` | авторизация/CSRF (KI-007), интернет через Ethernet (KI-008), конфиг v6 с именами, типом нагрузки и минимальным временем работы (§8.3); интеграции умного дома (§8.4) | **меняется** |

Конфигурация PlatformIO для тестов:

```ini
[platformio]
default_envs = opta          ; pio run без -e собирает только прошивку

[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17 -Wall -Wextra
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
3. ~~Баг первой загрузки~~ (KI-001) — решено повторами после запуска (D-029),
   проверено на железе 2026-10-22: подключается вторая попытка.
4. **Потоки** (сделано на шаге 2g, D-026). `FetchWorker` в отдельном `rtos::Thread`
   проще конечного автомата, потому что TLS-handshake в `WiFiSSLClient::connect`
   блокирующий. Сетевой стек mbed потокобезопасен, но веб-сервер в основном потоке и
   запрос в рабочем делят сокеты и память TLS. Проверить на железе: запас heap при
   открытой странице во время запроса; переход в точку настройки (долгое нажатие USER)
   во время запроса; DNS основного потока не нужен (NTP тоже в рабочем).

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

Текущая сборка (шаг 2f): RAM 15,5 % (81 224 байт из 512 КБ; к шагу 2a +7,7 КБ, из них
6 КБ — история переключений), flash 65,7 % (516 600 байт из 768 КБ; шаг 2f сэкономил
2,6 КБ: обработчик запроса и страницы больше не собираются дважды — для `WiFiClient` и
`EthernetClient`, а работают через `arduino::Client&`).
Основной резерв — flash: HTML-страница и тексты занимают заметную часть. Перед
крупными функциями (MQTT, JSON API) стоит сжать страницу (gzip в flash) и проверить
запас heap при TLS (D-008).
