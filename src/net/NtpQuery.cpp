#include "NtpQuery.h"

bool queryNtpTime(arduino::UDP& udp, bool& udpStarted, Print& log,
    uint32_t& epoch, uint32_t& atMs) {
    if (!udpStarted) {
        if (!udp.begin(2390)) {
            log.println("[NTP] Не удалось открыть UDP-порт для синхронизации времени.");
            return false;
        }
        udpStarted = true;
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

    while (udp.parsePacket() > 0) {
        uint8_t discard[48];
        while (udp.available() > 0) {
            udp.read(discard, sizeof(discard));
        }
    }

    if (!udp.beginPacket("pool.ntp.org", 123) ||
        udp.write(packet, sizeof(packet)) != sizeof(packet) ||
        !udp.endPacket()) {
        log.println("[NTP] Не удалось отправить запрос серверу времени.");
        return false;
    }

    unsigned long startedAt = millis();
    while (millis() - startedAt < 1500UL) {
        int packetSize = udp.parsePacket();
        if (packetSize >= static_cast<int>(sizeof(packet))) {
            int bytesRead = udp.read(packet, sizeof(packet));
            if (bytesRead < static_cast<int>(sizeof(packet)) ||
                (packet[0] & 0x07) != 4 ||
                packet[1] == 0) {
                log.println("[NTP] Получен некорректный ответ сервера времени.");
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
                log.println("[NTP] В ответе указано некорректное время.");
                return false;
            }

            epoch = epochSeconds;
            atMs = millis();
            log.print("[NTP] Время синхронизировано (UTC epoch ");
            log.print(epochSeconds);
            log.println(").");
            return true;
        }
        delay(10);
    }

    log.println("[NTP] Сервер времени не ответил за 1.5 секунды.");
    return false;
}
