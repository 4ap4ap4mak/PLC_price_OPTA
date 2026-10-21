// Прошивка Arduino Opta: управление 4 реле по цене Nord Pool LV (API Elering).
// Перед изменениями прочитайте CLAUDE.md и docs/: логика реле — docs/SCENARIOS.md,
// известные баги — docs/KNOWN_ISSUES.md, модули — docs/ARCHITECTURE.md.
// Чистая логика и состояние каналов — lib/ (тесты: pio test -e native), сеть —
// src/net, веб — src/web, консоль — src/console, железо — src/platform.
// Здесь только сборка объектов, загрузка настроек, повтор NTP, setup() и loop().
// Сетевые запросы (цены Elering, NTP) идут в рабочем потоке FetchWorker (шаг 2g).
#include <Arduino.h>

#include <ChannelConfig.h>
#include <ConfigStore.h>
#include <Controller.h>
#include <SwitchHistory.h>
#include <WifiCredentialStore.h>

#include "console/SerialConsole.h"
#include "net/FetchWorker.h"
#include "net/NetworkManager.h"
#include "platform/KvStorage.h"
#include "platform/Log.h"
#include "platform/RelayOutputBank.h"
#include "platform/StatusLed.h"
#include "platform/SystemClock.h"
#include "platform/UserButton.h"
#include "platform/Watchdog.h"
#include "web/WebServer.h"

using channelconfig::ChannelConfig;
using channelconfig::kChannelCount;

const unsigned long TIME_SYNC_RETRY_INTERVAL_MS = 60UL * 1000UL;
const unsigned long TIME_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;

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
SystemClock systemClock(fillHistoryEpochs);

// Рабочий поток: цены Elering по TLS и NTP, loop() не ждет сеть (KI-006, D-026)
FetchWorker fetchWorker;

// Владелец состояния каналов: настройки, ручной режим, принудительное включение,
// цена, прогноз, решения автоматики (lib/controller)
controller::Controller channelController(systemClock, relayOutputs, configStore,
    fetchWorker, switchHistory, serialLog);

// Сеть, веб-страница и консоль — адаптеры над Controller
NetworkManager network(wifiCredentialStore);
WebServer webServer(channelController, systemClock, network, wifiCredentialStore,
    switchHistory);
SerialConsole serialConsole(channelController, systemClock, switchHistory);

// Запрос NTP в рабочем потоке. Пока предыдущий не завершен, новый не ставится и
// момент попытки не сдвигается.
bool timeSyncRequested = false;
unsigned long lastTimeSyncRequestMillis = 0;

void requestTimeSync() {
    if (fetchWorker.startTimeSync()) {
        timeSyncRequested = true;
        lastTimeSyncRequestMillis = millis();
    }
}

// Повтор NTP раз в минуту до первой синхронизации, затем раз в 6 часов. Расписание
// цены идет только при синхронизированных часах.
void servicePriceSchedule() {
    unsigned long nowMillis = millis();
    if (!systemClock.synced()) {
        if (!timeSyncRequested ||
            nowMillis - lastTimeSyncRequestMillis >= TIME_SYNC_RETRY_INTERVAL_MS) {
            requestTimeSync();
        }
        return;
    }

    if (nowMillis - lastTimeSyncRequestMillis >= TIME_RESYNC_INTERVAL_MS) {
        requestTimeSync();
    }
    channelController.servicePriceSchedule();
}

// Результаты рабочего потока: его строки лога, ответ NTP, этапы запроса цены.
// Работает в любом режиме сети: запрос, начатый до потери Wi-Fi, завершается и
// в режиме точки настройки.
void serviceFetchResults(unsigned long nowMillis) {
    uint32_t epoch = 0;
    uint32_t atMillis = 0;
    if (fetchWorker.pollTimeSync(epoch, atMillis) == controller::FetchStatus::Done) {
        systemClock.apply(epoch, atMillis);
    }
    channelController.servicePriceFetch();
    fetchWorker.flushLog();

    // Сетевой вызов завис: раньше loop() стоял бы в нем, и устройство перезапустил бы
    // watchdog. Теперь loop() жив, поэтому перезапуск явный (D-026). После запуска
    // реле выключены до решения автоматики.
    if (fetchWorker.stuck(nowMillis)) {
        logLine("ERROR", "Сетевой запрос не завершился за %lu с; перезагрузка устройства.",
            static_cast<unsigned long>(FetchWorker::kStuckMs / 1000UL));
        Serial.flush();
        delay(500);
        NVIC_SystemReset();
    }
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

    if (!fetchWorker.begin()) {
        Serial.println("[ERROR] Не удалось запустить поток сетевых запросов; цены и время недоступны.");
    }

    // Ethernet, Wi-Fi или точка настройки; true — Wi-Fi подключен. Время и цена
    // запрашиваются в фоне: результат придет в loop() (поток делает NTP первым).
    if (network.begin(webServer)) {
        requestTimeSync();
        channelController.updatePrice();
        serialConsole.printStatusReport();
    }
    startHardwareWatchdog();
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
    kickWatchdog();
    // До любых ранних return: должно работать и без сети, и в режиме точки настройки
    channelController.serviceFailsafe();
    channelController.serviceForceOn();
    unsigned long currentMillis = millis();
    serviceFetchResults(currentMillis);

    // -------------------------------------------------------------------------
    // 1. Обработка команд из Serial Monitor ('status', 'update', 'on', 'off')
    // -------------------------------------------------------------------------
    serialConsole.service();

    // -------------------------------------------------------------------------
    // 2. Кнопка на корпусе Opta (BTN_USER): короткое нажатие — ручное
    //    переключение реле 1, удержание 5 с — настройка Wi-Fi
    // -------------------------------------------------------------------------
    const userbutton::Event buttonEvent = userButton.service(currentMillis);
    if (buttonEvent == userbutton::Event::ShortPress) {
        bool newState = !channelController.outputOn(0);
        channelController.setManual(0, newState,
            "\n[BUTTON] Короткое нажатие USER: ручное переключение Реле 1.");
    }
    if (buttonEvent == userbutton::Event::LongPress) {
        network.requestPortal();
    }

    // -------------------------------------------------------------------------
    // 3. Индикация Wi-Fi светодиодом LED_USER:
    //    - Медленное мигание (1.5 сек): Wi-Fi подключен
    //    - Быстрое мигание (200 мс): нет подключения
    // -------------------------------------------------------------------------
    statusLed.service(currentMillis, network.wifiConnected());

    // Точка настройки или потеря Wi-Fi: статус и расписание цены пропускаются,
    // failsafe и принудительное включение уже обслужены выше.
    if (!network.service(currentMillis)) {
        return;
    }

    // -------------------------------------------------------------------------
    // 4. Периодический краткий статус в Serial Monitor (каждые 10 секунд)
    // -------------------------------------------------------------------------
    serialConsole.serviceHeartbeat(currentMillis);

    // -------------------------------------------------------------------------
    // 5. Опрос по реальному времени: на каждой четверти часа, затем каждые 30 секунд
    //    до появления новой цены в API
    // -------------------------------------------------------------------------
    servicePriceSchedule();
}
