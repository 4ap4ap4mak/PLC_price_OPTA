#include "SerialConsole.h"

#include <WiFi.h>

#include <ForceOnTimer.h>

#include "../FirmwareVersion.h"
#include "../platform/OtaStorage.h"
#include "../platform/Watchdog.h"

using channelconfig::ChannelConfig;
using channelconfig::kChannelCount;

// Интервал краткого вывода статуса в монитор (раз в 10 секунд)
static const unsigned long HEARTBEAT_INTERVAL_MS = 10000UL;

// =============================================================================
// ПОДРОБНЫЙ ОТЧЕТ О СТАТУСЕ В СЕРИЙНЫЙ МОНИТОР
// =============================================================================
void SerialConsole::printStatusReport() {
    Serial.println("\n==================================================");
    Serial.println("       СТАТУС ARDUINO OPTA PLC: NORDPOOL          ");
    Serial.println("==================================================");

    // WiFi.SSID() и RSSI() — только когда поток подключения не занимает Wi-Fi (D-029)
    if (network_.wifiOnline()) {
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
    const IPAddress wired = network_.wiredAddress();
    Serial.print("  [Сеть] Ethernet:   ");
    if (wired[0] != 0) {
        Serial.println(wired);
    } else {
        Serial.println("нет адреса");
    }
    Serial.print("  [Сеть] Интернет:   ");
    Serial.println(uplinkName(network_.uplink()));

    Serial.println("  [Реле] Состояния:");
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        Serial.print("    Реле ");
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(channels_.outputOn(i) ? "ВКЛЮЧЕНО" : "ВЫКЛЮЧЕНО");
        if (channels_.forceOnActive(i)) {
            char remaining[24];
            forceon::formatRemaining(channels_.forceOnRemainingMs(i), remaining,
                sizeof(remaining));
            Serial.print(" (принудительно, осталось ");
            Serial.print(remaining);
            Serial.print(")");
        } else if (channels_.manual(i)) {
            Serial.print(" (ручной режим)");
        }
        Serial.println();
    }

    Serial.print("  [Биржа] Цена LV:   ");
    if (channels_.hasLatestPrice()) {
        Serial.print(channels_.latestPriceEurKwh(), 5);
        Serial.println(" EUR/kWh");
    } else if (channels_.hadAnyPrice()) {
        Serial.println("нет актуальной цены больше часа; автоматические реле выключены");
    } else {
        Serial.println("Ожидание первого запроса...");
    }

    const ChannelConfig& selectedConfig = channels_.config(selectedChannel_);
    Serial.print("  [Детали реле ");
    Serial.print(selectedChannel_ + 1);
    Serial.print("] Режим: ");
    Serial.println(channels_.modeName(selectedChannel_));
    Serial.print("  [Детали реле ");
    Serial.print(selectedChannel_ + 1);
    Serial.print("] Порог включения: ");
    Serial.print(selectedConfig.alwaysOnBelowPriceEurKwh, 5);
    Serial.println(" EUR/kWh");
    Serial.print("  [Детали реле ");
    Serial.print(selectedChannel_ + 1);
    Serial.print("] Аварийный предел: ");
    Serial.print(selectedConfig.safetyCeilingEurKwh, 5);
    Serial.println(" EUR/kWh");

    uint32_t retrySeconds = 0;
    if (channels_.priceFetchInProgress()) {
        Serial.println("  [Таймер]:          Идет запрос цены (в фоне)");
    } else if (channels_.priceRetryPending(retrySeconds)) {
        Serial.print("  [Таймер]:          Повтор цены через ");
        Serial.print(retrySeconds);
        Serial.println(" сек");
    } else if (clock_.synced()) {
        uint32_t secondsToNextInterval =
            controller::kPriceIntervalSeconds -
            (clock_.nowUtc() % controller::kPriceIntervalSeconds);
        Serial.print("  [Таймер]:          Следующая плановая проверка через ");
        Serial.print(secondsToNextInterval / 60);
        Serial.print(" мин ");
        Serial.print(secondsToNextInterval % 60);
        Serial.println(" сек");
    } else {
        Serial.println("  [Таймер]:          Ожидание синхронизации времени NTP");
    }

    printFirmwareLine();

    Serial.println("--------------------------------------------------");
    Serial.print("  Дополнительные детали: выбрано реле ");
    Serial.println(selectedChannel_ + 1);
    Serial.println("  Команды: 'status [1-4]' (или '?'), 'update', 'on [1-4]', 'off [1-4]', 'auto [1-4]',");
    Serial.println("           'force [1-4]', 'force off [1-4]', 'history [1-4]', 'sockets', 'mqtt', 'config reset',");
    Serial.println("           'firmware', 'firmware check', 'firmware install'");
    Serial.println("==================================================\n");
}

// Версия прошивки и состояние обновления (D-038).
void SerialConsole::printFirmwareLine() {
    Serial.print("  [Прошивка]:        ");
    Serial.print(kFirmwareVersion);
    Serial.print(", загрузчик ");
    Serial.print(OtaStorage::bootloaderVersion());
    Serial.print(OtaStorage::bootloaderSupportsOta() ? "" : " (без обновления по сети)");
    Serial.print("; ");
    Serial.print(FirmwareUpdater::stateName(firmware_.state()));
    const fwupdate::Manifest* latest = firmware_.latest();
    if (latest != nullptr) {
        Serial.print(", последний релиз ");
        Serial.print(latest->versionText);
    }
    if (firmware_.state() == FirmwareUpdater::State::Downloading && latest != nullptr) {
        Serial.print(", загружено ");
        Serial.print(firmware_.downloadedBytes() / 1024UL);
        Serial.print(" из ");
        Serial.print(latest->size / 1024UL);
        Serial.print(" КБ");
    }
    if (firmware_.state() == FirmwareUpdater::State::CheckFailed ||
        firmware_.state() == FirmwareUpdater::State::DownloadFailed) {
        Serial.print(" (");
        Serial.print(fwupdate::failureText(firmware_.failure()));
        Serial.print(")");
    }
    Serial.println();
}

// Последние переключения, новые сверху. channel == kDeviceEvent — все реле.
// События устройства (запуск) выводятся всегда.
void SerialConsole::printSwitchHistory(uint8_t channel) {
    const size_t kMaxLines = 50;
    Serial.println("\n[HISTORY] Переключения (новые сверху), хранятся до перезапуска:");
    size_t printed = 0;
    char timeText[32];
    for (size_t i = 0; i < switchHistory_.size() && printed < kMaxLines; ++i) {
        kickWatchdog();
        const history::SwitchEvent& event = switchHistory_.newest(i);
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
    Serial.print(switchHistory_.size());
    Serial.print(", показано: ");
    Serial.println(printed);
}

// =============================================================================
// ОБРАБОТКА КОМАНД ИЗ СЕРИЙНОГО МОНИТОРА
// =============================================================================
void SerialConsole::service() {
    if (Serial.available() > 0) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        const String raw = cmd;
        cmd.toLowerCase();
        uint8_t commandRelay = selectedChannel_;
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
                selectedChannel_ = commandRelay;
                relayArgumentGiven = true;
            }
            cmd = cmd.substring(0, argumentStart);
            cmd.trim();
        }

        if (cmd == "status" || cmd == "?" || cmd == "info" || cmd == "") {
            printStatusReport();
        } else if (cmd == "update" || cmd == "price") {
            // Без связи запрос не ставится: поток запросов не должен трогать Wi-Fi,
            // пока его подключает WifiConnector (D-029)
            if (network_.uplink() == Uplink::None) {
                Serial.println("\n[CMD] Нет связи с интернетом; цена будет запрошена после подключения.");
            } else {
                Serial.println("\n[CMD] Внеочередной запрос актуальных котировок...");
                channels_.updatePrice();
            }
            printStatusReport();
        } else if (cmd == "on") {
            channels_.setManual(commandRelay, true);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" ВКЛЮЧЕНО вручную (до команды auto или кнопки «Вернуть автоматике»).");
        } else if (cmd == "off") {
            channels_.setManual(commandRelay, false);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" ВЫКЛЮЧЕНО вручную (до команды auto или кнопки «Вернуть автоматике»).");
        } else if (cmd == "auto") {
            // Решение сразу по последней цене и прогнозу, без запроса в сеть (D-034)
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(": возврат в автоматический режим.");
            if (channels_.forceOnActive(commandRelay)) {
                channels_.stopForceOn(commandRelay);
            } else {
                channels_.releaseManual(commandRelay);
            }
        } else if (cmd == "force") {
            if (relayArgument.length() > 0 && !relayArgumentGiven) {
                Serial.println("\n[CMD] Формат: force [1-4] или force off [1-4].");
            } else if (forceOffRequested) {
                channels_.stopForceOn(commandRelay);
            } else {
                channels_.startForceOn(commandRelay);
            }
        } else if (cmd == "history") {
            printSwitchHistory(relayArgumentGiven ? commandRelay : history::kDeviceEvent);
        } else if (cmd == "config") {
            // config reset (D-037): записать текущие настройки из RAM поверх нечитаемых
            if (relayArgument != "reset") {
                Serial.println("\n[CMD] Формат: config reset.");
            } else if (!channels_.configWriteLocked()) {
                Serial.println("\n[CMD] Настройки прочитаны при запуске; сброс не нужен.");
            } else if (channels_.overwriteStoredConfig()) {
                Serial.println("\n[CONFIG] Текущие настройки записаны во flash; сохранение снова доступно.");
            } else {
                Serial.println("\n[ERROR] Не удалось записать настройки во flash.");
            }
        } else if (cmd == "sockets") {
            if (mqtt_.connecting()) {
                Serial.println("[NET] Идет подключение MQTT; проверка сокетов не выполнена, повторите через 10 секунд.");
            } else {
                network_.printSocketReport(channels_.priceFetchInProgress(), mqtt_.connected());
            }
        } else if (cmd == "mqtt") {
            handleMqtt(raw);
        } else if (cmd == "firmware") {
            // firmware [check|install] (D-038); результат — строками [FIRMWARE] из loop()
            const bool online = network_.uplink() != Uplink::None;
            if (relayArgument == "check" || relayArgument == "install") {
                const FirmwareUpdater::Request result = relayArgument == "check"
                    ? firmware_.requestCheck(online)
                    : firmware_.requestInstall(online);
                if (result != FirmwareUpdater::Request::Started) {
                    Serial.print("\n[CMD] Не выполнено: ");
                    Serial.print(FirmwareUpdater::requestText(result));
                    Serial.println(".");
                }
            } else if (relayArgument.length() > 0) {
                Serial.println("\n[CMD] Формат: firmware, firmware check или firmware install.");
            } else {
                Serial.println();
                printFirmwareLine();
            }
        } else {
            Serial.print("\n[CMD] Неизвестная команда: \"");
            Serial.print(cmd);
            Serial.println("\". Доступные команды: status [1-4], update, on [1-4], off [1-4], auto [1-4], "
                "force [1-4], force off [1-4], history [1-4], sockets, mqtt, config reset, "
                "firmware [check|install]");
        }
    }
}

void SerialConsole::handleMqtt(const String& raw) {
    // Слова после «mqtt»: адрес, пользователь, пароль, порт — регистр сохраняется
    String words[4];
    uint8_t count = 0;
    int position = raw.indexOf(' ');
    while (position >= 0 && count < 4) {
        while (position < static_cast<int>(raw.length()) && raw[position] == ' ') ++position;
        if (position >= static_cast<int>(raw.length())) break;
        int end = raw.indexOf(' ', position);
        words[count++] = end < 0 ? raw.substring(position) : raw.substring(position, end);
        position = end;
    }
    if (count == 0) {
        mqtt_.printStatus();
    } else if (count == 1 && words[0].equalsIgnoreCase("off")) {
        mqtt_.configure("", 1883, "", "");
        Serial.println("[MQTT] Выключено.");
    } else if (count >= 3) {
        const long port = count == 4 ? words[3].toInt() : 1883;
        if (port <= 0 || port > 65535) {
            Serial.println("[MQTT] Неверный порт.");
            return;
        }
        mqtt_.configure(words[0].c_str(), static_cast<uint16_t>(port), words[1].c_str(),
            words[2].c_str());
        Serial.print("[MQTT] Брокер ");
        Serial.print(words[0]);
        Serial.print(":");
        Serial.print(port);
        Serial.println(" задан до перезапуска; подключение — в фоне, итог строкой [MQTT].");
    } else {
        Serial.println("[MQTT] Формат: mqtt <IP> <пользователь> <пароль> [порт], mqtt off или mqtt.");
    }
}

void SerialConsole::serviceHeartbeat(uint32_t nowMs) {
    if (nowMs - lastHeartbeatMillis_ >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMillis_ = nowMs;
        Serial.print("[STATUS] Реле: ");
        for (uint8_t i = 0; i < kChannelCount; ++i) {
            if (i > 0) Serial.print(", ");
            Serial.print(i + 1);
            Serial.print(" ");
            Serial.print(channels_.outputOn(i) ? "ВКЛ" : "ВЫКЛ");
            if (channels_.forceOnActive(i)) Serial.print(" принуд.");
        }
        Serial.print(" | Цена: ");
        if (channels_.hasLatestPrice()) {
            Serial.print(channels_.latestPriceEurKwh(), 5);
            Serial.print(" EUR/kWh");
        } else {
            Serial.print("нет данных");
        }
        Serial.print(" | Wi-Fi: ");
        if (network_.wifiOnline()) {
            Serial.print("есть (");
            Serial.print(WiFi.RSSI());
            Serial.print(" dBm)");
        } else {
            Serial.print("нет");
        }
        Serial.print(" | Интернет: ");
        Serial.print(uplinkName(network_.uplink()));
        Serial.println();
        Serial.println("Напишите \"STATUS\" для дополнительной информации.");
    }
}
