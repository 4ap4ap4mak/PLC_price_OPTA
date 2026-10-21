#include "SystemClock.h"

bool SystemClock::sync() {
    lastSyncAttemptMs_ = millis();
    hasAttemptedSync_ = true;

    if (!udpStarted_) {
        if (!udp_.begin(2390)) {
            Serial.println("[NTP] Не удалось открыть UDP-порт для синхронизации времени.");
            return false;
        }
        udpStarted_ = true;
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

    while (udp_.parsePacket() > 0) {
        uint8_t discard[48];
        while (udp_.available() > 0) {
            udp_.read(discard, sizeof(discard));
        }
    }

    if (!udp_.beginPacket("pool.ntp.org", 123) ||
        udp_.write(packet, sizeof(packet)) != sizeof(packet) ||
        !udp_.endPacket()) {
        Serial.println("[NTP] Не удалось отправить запрос серверу времени.");
        return false;
    }

    unsigned long startedAt = millis();
    while (millis() - startedAt < 1500UL) {
        int packetSize = udp_.parsePacket();
        if (packetSize >= static_cast<int>(sizeof(packet))) {
            int bytesRead = udp_.read(packet, sizeof(packet));
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

            syncedEpoch_ = epochSeconds;
            syncedAtMs_ = millis();
            if (!synced_ && onFirstSync_ != nullptr) {
                onFirstSync_(epochSeconds, millis() / 1000UL);
            }
            synced_ = true;
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

uint32_t SystemClock::nowUtc() const {
    return syncedEpoch_ + (millis() - syncedAtMs_) / 1000UL;
}
