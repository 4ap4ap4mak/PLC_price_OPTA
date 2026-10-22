#include "MdnsResponder.h"

#include <DnsPacket.h>

static const IPAddress MDNS_MULTICAST_IP(224, 0, 0, 251);

bool MdnsResponder::begin(const char* interfaceName) {
    interfaceName_ = interfaceName;
    started_ = udp_.beginMulticast(MDNS_MULTICAST_IP, 5353);
    return started_;
}

void MdnsResponder::stop() {
    udp_.stop();
    started_ = false;
}

void MdnsResponder::service(const IPAddress& address) {
    int packetSize = udp_.parsePacket();
    if (packetSize < 17 || packetSize > 512) {
        return;
    }
    // Лог — только об ответах на свое имя: в домашней сети mDNS-пакеты от телефонов
    // и медиаустройств идут постоянно, строка на каждый засоряла бы Serial
    IPAddress querySource = udp_.remoteIP();
    uint16_t querySourcePort = udp_.remotePort();
    uint8_t packet[dnspacket::kMaxPacket];
    int receivedSize = udp_.read(packet, sizeof(packet));
    const uint8_t ip[4] = {address[0], address[1], address[2], address[3]};
    uint8_t answerPacket[dnspacket::kMaxPacket];
    dnspacket::MdnsReply reply = dnspacket::buildMdnsAnswer(packet, receivedSize,
        querySourcePort, hostLabel_, ip, answerPacket, sizeof(answerPacket));
    if (reply.status != dnspacket::MdnsStatus::Answer) {
        return;
    }

    IPAddress responseAddress = reply.unicast ? querySource : MDNS_MULTICAST_IP;
    uint16_t responsePort = reply.unicast ? querySourcePort : 5353;
    if (udp_.beginPacket(responseAddress, responsePort)) {
        udp_.write(answerPacket, reply.length);
        int result = udp_.endPacket();
        if (result <= 0) {
            Serial.println("[mDNS] Ошибка отправки ответа opta-settings.local.");
        } else {
            Serial.print("[mDNS] Ответ opta-settings.local отправлен через ");
            Serial.print(interfaceName_);
            Serial.print(" на ");
            Serial.print(responseAddress);
            Serial.print(":");
            Serial.println(responsePort);
        }
    } else {
        Serial.println("[mDNS] Не удалось подготовить UDP-ответ.");
    }
}
