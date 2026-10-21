#include "CaptiveDns.h"

#include <DnsPacket.h>

bool CaptiveDns::begin() {
    if (!started_) {
        if (!udp_.begin(53)) {
            return false;
        }
        started_ = true;
    }
    return true;
}

void CaptiveDns::stop() {
    if (started_) {
        udp_.stop();
        started_ = false;
    }
}

void CaptiveDns::service(const IPAddress& portalIpAddress) {
    int packetSize = udp_.parsePacket();
    if (packetSize < 17 || packetSize > 512) {
        return;
    }
    uint8_t packet[dnspacket::kMaxPacket];
    int receivedSize = udp_.read(packet, sizeof(packet));
    const uint8_t portalIp[4] = {portalIpAddress[0], portalIpAddress[1], portalIpAddress[2], portalIpAddress[3]};
    size_t answerLength = dnspacket::buildCaptiveDnsAnswer(packet, receivedSize,
        sizeof(packet), portalIp);
    if (answerLength == 0) {
        return;
    }
    if (udp_.beginPacket(udp_.remoteIP(), udp_.remotePort())) {
        udp_.write(packet, answerLength);
        udp_.endPacket();
    }
}
