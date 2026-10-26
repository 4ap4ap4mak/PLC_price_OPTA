// Прошивка Arduino Opta: управление 4 реле по цене Nord Pool LV (API Elering).
// Перед изменениями прочитайте CLAUDE.md и docs/: логика реле — docs/SCENARIOS.md,
// известные баги — docs/KNOWN_ISSUES.md, модули — docs/ARCHITECTURE.md.
// Чистая логика и состояние каналов — lib/ (тесты: pio test -e native), сеть —
// src/net, веб — src/web, консоль — src/console, железо — src/platform.
// Здесь только сборка объектов, загрузка настроек, повтор NTP, setup() и loop().
// Сетевые запросы (цены Elering, NTP) идут в рабочем потоке FetchWorker (шаг 2g),
// подключение Wi-Fi — в потоке WifiConnector внутри NetworkManager (D-029).
#include <Arduino.h>

#include <ChannelConfig.h>
#include <ConfigStore.h>
#include <Controller.h>
#include <SwitchHistory.h>
#include <WifiCredentialStore.h>

#include "FirmwareVersion.h"
#include "console/SerialConsole.h"
#include "net/FetchWorker.h"
#include "net/NetworkManager.h"
#include "platform/BoardInfoRecovery.h"
#include "platform/KvStorage.h"
#include "platform/Log.h"
#include "platform/OtaStorage.h"
#include "platform/RelayOutputBank.h"
#include "platform/StatusLed.h"
#include "platform/SystemClock.h"
#include "platform/UserButton.h"
#include "platform/Watchdog.h"
#include "update/FirmwareUpdater.h"
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

// Обновление прошивки с GitHub Releases: проверка и загрузка — в FetchWorker (D-038)
FirmwareUpdater firmwareUpdater(fetchWorker, channelController, systemClock);

// Сеть, веб-страница и консоль — адаптеры над Controller
NetworkManager network(wifiCredentialStore);
WebServer webServer(channelController, systemClock, network, wifiCredentialStore,
    switchHistory, firmwareUpdater);
SerialConsole serialConsole(channelController, systemClock, switchHistory, network,
    firmwareUpdater);

// Запрос NTP в рабочем потоке. Пока предыдущий не завершен, новый не ставится и
// момент попытки не сдвигается.
bool timeSyncRequested = false;
unsigned long lastTimeSyncRequestMillis = 0;
// Первое подключение после запуска (Wi-Fi или Ethernet) уже было: время, цена и отчет
// запрошены
bool startupRequestsSent = false;

void requestTimeSync() {
    if (fetchWorker.startTimeSync()) {
        timeSyncRequested = true;
        lastTimeSyncRequestMillis = millis();
    }
}

// Повтор NTP раз в минуту до первой синхронизации, затем раз в 6 часов — только при
// связи. Расписание реле идет при синхронизированных часах и без сети: отпуск, граница
// 15 минут и план по загруженному прогнозу (D-035); запросы цены — только при связи.
void serviceSchedule(bool online) {
    unsigned long nowMillis = millis();
    if (!systemClock.synced()) {
        if (online && (!timeSyncRequested ||
            nowMillis - lastTimeSyncRequestMillis >= TIME_SYNC_RETRY_INTERVAL_MS)) {
            requestTimeSync();
        }
        return;
    }

    if (online && nowMillis - lastTimeSyncRequestMillis >= TIME_RESYNC_INTERVAL_MS) {
        requestTimeSync();
    }
    channelController.serviceSchedule();
    if (online) {
        channelController.servicePriceRequests();
    }
}

// Результаты рабочего потока: его строки лога, ответ NTP, этапы запроса цены.
// Работает в любом режиме сети: запрос, начатый до потери Wi-Fi, завершается и
// в режиме точки настройки.
void serviceFetchResults() {
    uint32_t epoch = 0;
    uint32_t atMillis = 0;
    if (fetchWorker.pollTimeSync(epoch, atMillis) == controller::FetchStatus::Done) {
        systemClock.apply(epoch, atMillis);
    }
    channelController.servicePriceFetch();
    fetchWorker.flushLog();
}

// Сетевой вызов в потоке завис (запрос FetchWorker или подключение Wi-Fi): раньше
// loop() стоял бы в нем, и устройство перезапустил бы watchdog. Теперь loop() жив,
// поэтому перезапуск явный (D-026, D-029). После запуска реле выключены до решения
// автоматики.
// Пока идет принудительное включение, перезагрузка откладывается: она бы его
// оборвала (D-007, п. 3), а реле под контролем — loop() работает, таймер идет.
void serviceStuckNetwork(unsigned long nowMillis) {
    static bool rebootDeferredLogged = false;
    const char* what = nullptr;
    unsigned long stuckSeconds = 0;
    if (fetchWorker.stuck(nowMillis)) {
        what = "Сетевой запрос не завершился";
        stuckSeconds = FetchWorker::kStuckMs / 1000UL;
    } else if (network.stuck(nowMillis)) {
        what = "Подключение к Wi-Fi не завершилось";
        stuckSeconds = WifiConnector::kStuckMs / 1000UL;
    }
    if (what == nullptr) {
        rebootDeferredLogged = false;
        return;
    }
    if (channelController.anyForceOnActive()) {
        if (!rebootDeferredLogged) {
            logLine("WARN", "%s за %lu с; перезагрузка отложена до конца принудительного включения.",
                what, stuckSeconds);
            rebootDeferredLogged = true;
        }
        return;
    }
    logLine("ERROR", "%s за %lu с; перезагрузка устройства.", what, stuckSeconds);
    Serial.flush();
    delay(500);
    NVIC_SystemReset();
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
        case LoadOutcome::ReadFailed:
            logLine("ERROR", "Не удалось прочитать настройки реле (KVStore: %d, попыток: %u); запись настроек заблокирована до перезапуска, чтобы не затереть сохраненные. Команда config reset записывает настройки по умолчанию.",
                report.kvResult, static_cast<unsigned>(configstore::ConfigStore::kReadAttempts));
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
            // MigratedSaveFailed: перенесенные настройки работают из RAM (D-037)
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
    // Первым делом: фатальная ошибка при запуске — перезагрузка, а не остановка (D-041).
    // Таймаут ~33 с: шаги setup() ниже короче, после настроек — kickWatchdog().
    startHardwareWatchdog();
    // До всего, что использует QSPI (KVStore, Wi-Fi, TLS): данные платы из OTP (KI-032).
    // Если ядро их не прочитало — перезагрузка, реле еще не настроены.
    const BoardInfoRecovery boardInfoRecovery = recoverBoardInfo();
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
    kickWatchdog();

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
    logLine("FIRMWARE", "Версия %s, загрузчик %u%s.", kFirmwareVersion,
        static_cast<unsigned>(OtaStorage::bootloaderVersion()),
        OtaStorage::bootloaderSupportsOta() ? "" : " (обновление по сети не поддерживает)");
    switch (boardInfoRecovery) {
        case BoardInfoRecovery::NotNeeded:
            break;
        case BoardInfoRecovery::AfterReboot:
            logLine("BOOT", "Данные платы из QSPI не прочитались при прошлом запуске (микросхема была занята); после перезагрузки прочитаны.");
            break;
        case BoardInfoRecovery::Failed:
            logLine("ERROR", "Данные платы из QSPI не прочитаны и после перезагрузки: USB VID 0xFFFF, запись QSPI и TLS могут не работать. Поможет отключение питания или прошивка по USB.");
            break;
    }

    if (!fetchWorker.begin()) {
        Serial.println("[ERROR] Не удалось запустить поток сетевых запросов; цены и время недоступны.");
    }

    // Ethernet и первая попытка Wi-Fi через 3 с; подключение идет в фоне, время и цена
    // запрашиваются в loop() при первом подключении (D-029)
    network.begin(webServer);
    reportHardwareWatchdog();
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
    serviceFetchResults();
    serviceStuckNetwork(currentMillis);

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

    // Связь с интернетом — Wi-Fi или Ethernet (D-036). Без нее статус и запросы в сеть
    // пропускаются, расписание реле по загруженному прогнозу идет (D-035).
    const bool online = network.service(currentMillis);
    fetchWorker.setUplink(network.uplink());

    // -------------------------------------------------------------------------
    // 3. Индикация связи светодиодом LED_USER:
    //    - Медленное мигание (1.5 сек): есть связь (Wi-Fi или Ethernet)
    //    - Быстрое мигание (200 мс): нет связи
    // -------------------------------------------------------------------------
    statusLed.service(currentMillis, online);

    // Первое подключение после запуска: время и цена запрашиваются в фоне, результат
    // придет в serviceFetchResults() (поток делает NTP первым)
    if (online && !startupRequestsSent) {
        startupRequestsSent = true;
        requestTimeSync();
        channelController.updatePrice();
        serialConsole.printStatusReport();
    }

    // -------------------------------------------------------------------------
    // 4. Периодический краткий статус в Serial Monitor (каждые 10 секунд)
    // -------------------------------------------------------------------------
    if (online) {
        serialConsole.serviceHeartbeat(currentMillis);
    }

    // -------------------------------------------------------------------------
    // 5. Расписание по реальному времени: план реле раз в минуту, на каждой четверти
    //    часа — запрос цены, затем каждые 30 секунд до появления новой цены в API
    // -------------------------------------------------------------------------
    serviceSchedule(online);

    // -------------------------------------------------------------------------
    // 6. Обновление прошивки: проверка раз в сутки, результаты проверки и загрузки,
    //    установка с перезагрузкой (D-038)
    // -------------------------------------------------------------------------
    firmwareUpdater.service(currentMillis, online);
}
