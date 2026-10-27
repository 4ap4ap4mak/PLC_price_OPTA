// Сборка ответов DNS для точки настройки (captive portal) и mDNS (opta-settings.local).
// Чистый модуль L0: без сокетов и Arduino, не логирует. Прием и отправку UDP
// делает вызывающий слой; здесь — только разбор запроса и байты ответа.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace dnspacket {

// Наибольший обрабатываемый пакет (как размер буфера в монолите).
constexpr size_t kMaxPacket = 512;

// Captive DNS: на любой A-запрос отвечает адресом ip (TTL 60 с).
// packet — принятый запрос (receivedSize — результат read(), может быть < 0);
// ответ собирается в том же буфере емкостью cap. Возвращает длину ответа
// или 0, если запрос надо проигнорировать.
size_t buildCaptiveDnsAnswer(uint8_t* packet, int receivedSize, size_t cap, const uint8_t ip[4]);

enum class MdnsStatus : uint8_t {
    Ignore,      // не запрос, поврежденный пакет или ответ не помещается — молча пропустить
    NotOurName,  // корректный запрос, но не про hostLabel.local
    Answer       // ответ собран в out
};
struct MdnsReply {
    MdnsStatus status;
    size_t length;  // длина ответа при Answer
    bool unicast;   // отвечать отправителю, а не в группу 224.0.0.251:5353
};

// mDNS: ответ на A/ANY-запрос имени "<hostLabel>.local" (регистр не важен, TTL 120 с).
// packet изменяется (заголовок готовится к ответу). sourcePort != 5353 или бит QU
// в классе вопроса → unicast-ответ. sourcePort != 5353 (legacy unicast, обычный
// DNS-резолвер) → класс записи IN без бита cache-flush и TTL 10 с (RFC 6762 §6.7, §10.2).
MdnsReply buildMdnsAnswer(uint8_t* packet, int receivedSize, uint16_t sourcePort,
    const char* hostLabel, const uint8_t ip[4], uint8_t* out, size_t outCap);

}  // namespace dnspacket
