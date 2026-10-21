// Запрос времени у NTP-сервера pool.ntp.org. Блокирует до 1,5 с ожидания ответа
// плюс разрешение имени, поэтому с шага 2g вызывается только из рабочего потока
// FetchWorker. Лог [NTP] — в log (буфер строк FetchWorker).
#pragma once

#include <Arduino.h>
#include <Udp.h>

// true — epoch (UTC) получен в момент atMs (millis()). udpStarted — открыт ли уже
// UDP-порт: открывается при первом запросе.
bool queryNtpTime(arduino::UDP& udp, bool& udpStarted, Print& log,
    uint32_t& epoch, uint32_t& atMs);
