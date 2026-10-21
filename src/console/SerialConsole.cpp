#include "SerialConsole.h"

#include <WiFi.h>

#include <ForceOnTimer.h>

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

    const ChannelConfig& selectedConfig = channels_.config(selection_.channel);
    Serial.print("  [Детали реле ");
    Serial.print(selection_.channel + 1);
    Serial.print("] Режим: ");
    Serial.println(channels_.modeName(selection_.channel));
    Serial.print("  [Детали реле ");
    Serial.print(selection_.channel + 1);
    Serial.print("] Порог включения: ");
    Serial.print(selectedConfig.alwaysOnBelowPriceEurKwh, 5);
    Serial.println(" EUR/kWh");
    Serial.print("  [Детали реле ");
    Serial.print(selection_.channel + 1);
    Serial.print("] Аварийный предел: ");
    Serial.print(selectedConfig.safetyCeilingEurKwh, 5);
    Serial.println(" EUR/kWh");

    uint32_t retrySeconds = 0;
    if (channels_.priceRetryPending(retrySeconds)) {
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

    Serial.println("--------------------------------------------------");
    Serial.print("  Дополнительные детали: выбрано реле ");
    Serial.println(selection_.channel + 1);
    Serial.println("  Команды: 'status [1-4]' (или '?'), 'update', 'on [1-4]', 'off [1-4]', 'auto [1-4]',");
    Serial.println("           'force [1-4]', 'force off [1-4]', 'history [1-4]'");
    Serial.println("==================================================\n");
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
        cmd.toLowerCase();
        uint8_t commandRelay = selection_.channel;
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
                selection_.channel = commandRelay;
                relayArgumentGiven = true;
            }
            cmd = cmd.substring(0, argumentStart);
            cmd.trim();
        }

        if (cmd == "status" || cmd == "?" || cmd == "info" || cmd == "") {
            printStatusReport();
        } else if (cmd == "update" || cmd == "price") {
            Serial.println("\n[CMD] Внеочередной запрос актуальных котировок...");
            channels_.updatePrice();
            printStatusReport();
        } else if (cmd == "on") {
            channels_.setManual(commandRelay, true);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" ВКЛЮЧЕНО вручную (ручной режим до границы 15 минут).");
        } else if (cmd == "off") {
            channels_.setManual(commandRelay, false);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" ВЫКЛЮЧЕНО вручную (ручной режим до границы 15 минут).");
        } else if (cmd == "auto") {
            channels_.clearManual(commandRelay);
            Serial.print("\n[CMD] Реле ");
            Serial.print(commandRelay + 1);
            Serial.println(" возвращено в автоматический режим.");
            if (channels_.forceOnActive(commandRelay)) {
                channels_.stopForceOn(commandRelay);
            }
            channels_.updatePrice();
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
        } else {
            Serial.print("\n[CMD] Неизвестная команда: \"");
            Serial.print(cmd);
            Serial.println("\". Доступные команды: status [1-4], update, on [1-4], off [1-4], auto [1-4], "
                "force [1-4], force off [1-4], history [1-4]");
        }
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
        if (WiFi.status() == WL_CONNECTED) {
            Serial.print("есть (");
            Serial.print(WiFi.RSSI());
            Serial.print(" dBm)");
        } else {
            Serial.print("нет");
        }
        Serial.println();
        Serial.println("Напишите \"STATUS\" для дополнительной информации.");
    }
}
