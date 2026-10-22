#include "DnsPacket.h"

#include <ctype.h>
#include <string.h>

namespace dnspacket {

size_t buildCaptiveDnsAnswer(uint8_t* packet, int receivedSize, size_t cap, const uint8_t ip[4]) {
    if (receivedSize < 17 || (packet[2] & 0x80) != 0) {
        return 0;
    }

    size_t questionEnd = 12;
    while (questionEnd < static_cast<size_t>(receivedSize) && packet[questionEnd] != 0) {
        uint8_t labelLength = packet[questionEnd++];
        if ((labelLength & 0xC0) != 0 || questionEnd + labelLength > static_cast<size_t>(receivedSize)) {
            return 0;
        }
        questionEnd += labelLength;
    }
    if (questionEnd >= static_cast<size_t>(receivedSize) ||
        questionEnd + 5 > static_cast<size_t>(receivedSize)) {
        return 0;
    }
    // Адрес IPv4 — только на запрос A или ANY. На AAAA и прочие — ответ без записей
    // (NOERROR): иначе телефон получил бы A-запись в ответ на AAAA
    const uint16_t queryType = (static_cast<uint16_t>(packet[questionEnd + 1]) << 8) |
        packet[questionEnd + 2];
    const bool answerAddress = queryType == 1 || queryType == 255;
    questionEnd += 5; // Пропуск нулевого терминатора и QTYPE/QCLASS
    packet[2] = 0x81;
    packet[3] = 0x80;
    packet[6] = 0;
    packet[7] = answerAddress ? 1 : 0;
    packet[8] = 0;
    packet[9] = 0;
    packet[10] = 0;
    packet[11] = 0;
    if (!answerAddress) {
        return questionEnd;
    }

    const uint8_t answer[] = {
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
        ip[0], ip[1], ip[2], ip[3]
    };
    if (questionEnd + sizeof(answer) > cap) {
        return 0;
    }
    memcpy(packet + questionEnd, answer, sizeof(answer));
    return questionEnd + sizeof(answer);
}

MdnsReply buildMdnsAnswer(uint8_t* packet, int receivedSize, uint16_t sourcePort,
    const char* hostLabel, const uint8_t ip[4], uint8_t* out, size_t outCap) {
    MdnsReply reply = {MdnsStatus::Ignore, 0, false};
    if (receivedSize < 17 || (packet[2] & 0x80) != 0 ||
        (packet[4] == 0 && packet[5] == 0) || (packet[2] & 0x78) != 0) {
        return reply;
    }

    bool requestUnicastResponse = sourcePort != 5353;
    size_t questionEnd = 12;
    size_t matchingNameOffset = 0;
    bool isDeviceQuery = false;
    uint16_t questionCount = (static_cast<uint16_t>(packet[4]) << 8) | packet[5];
    for (uint16_t question = 0; question < questionCount; ++question) {
        if (questionEnd >= static_cast<size_t>(receivedSize)) {
            return reply;
        }
        size_t nameOffset = questionEnd;
        uint8_t labelIndex = 0;
        bool matches = true;
        while (questionEnd < static_cast<size_t>(receivedSize) && packet[questionEnd] != 0) {
            uint8_t labelLength = packet[questionEnd++];
            if ((labelLength & 0xC0) != 0 ||
                questionEnd + labelLength > static_cast<size_t>(receivedSize)) {
                return reply;
            }
            const char* expected = labelIndex == 0 ? hostLabel :
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
            return reply;
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
        reply.status = MdnsStatus::NotOurName;
        return reply;
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
    if (questionEnd + sizeof(answer) > outCap) {
        return reply;
    }
    memcpy(out, packet, questionEnd);
    memcpy(out + questionEnd, answer, sizeof(answer));
    out[questionEnd] = static_cast<uint8_t>(0xC0 | (matchingNameOffset >> 8));
    out[questionEnd + 1] = static_cast<uint8_t>(matchingNameOffset & 0xFF);
    out[questionEnd + sizeof(answer) - 4] = ip[0];
    out[questionEnd + sizeof(answer) - 3] = ip[1];
    out[questionEnd + sizeof(answer) - 2] = ip[2];
    out[questionEnd + sizeof(answer) - 1] = ip[3];

    reply.status = MdnsStatus::Answer;
    reply.length = questionEnd + sizeof(answer);
    reply.unicast = requestUnicastResponse;
    return reply;
}

}  // namespace dnspacket
