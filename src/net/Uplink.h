// Через какой интерфейс идут запросы в интернет — цены Elering и NTP (D-036, KI-008).
// Выбирает NetworkManager: Wi-Fi, если подключен к сохраненной сети, иначе Ethernet
// с адресом; FetchWorker запоминает выбор в момент постановки задания.
#pragma once

#include <stdint.h>

enum class Uplink : uint8_t {
    None,      // связи с интернетом нет: запросы не ставятся
    WiFi,
    Ethernet,
};

// "Wi-Fi", "Ethernet" или "нет" — для лога.
const char* uplinkName(Uplink uplink);

// Сделать интерфейс uplink маршрутом по умолчанию стека (общего у Wi-Fi и Ethernet):
// соединения с адресами вне локальных сетей идут через него. Без этого после
// неудачной попытки Wi-Fi маршрутом оставался бы упавший Wi-Fi, и запрос по Ethernet
// не уходил бы. false — интерфейс недоступен.
bool routeThrough(Uplink uplink);
