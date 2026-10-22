// Запрос времени у NTP-сервера pool.ntp.org. Блокирует до 1,5 с ожидания ответа
// плюс разрешение имени, поэтому с шага 2g вызывается только из рабочего потока
// FetchWorker. Лог [NTP] — в log (буфер строк FetchWorker).
#pragma once

#include <Arduino.h>
#include <Udp.h>

// true — epoch (UTC) получен в момент atMs (millis()). UDP-порт открывается на время
// запроса и закрывается в конце: сокетов в ядре mbed 4 на все устройство (KI-017, D-032).
bool queryNtpTime(arduino::UDP& udp, Print& log, uint32_t& epoch, uint32_t& atMs);
