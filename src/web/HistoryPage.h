// Страница GET /history: история переключений всех реле, новые сверху (D-019).
// Передается потоком без Content-Length (BufferedClientWriter). channelIndex — реле,
// на страницу которого ведет ссылка «Настройки» (/history?relay=N, D-030).
#pragma once

#include <Arduino.h>
#include <api/Client.h>

#include <SwitchHistory.h>

void sendHistoryPage(arduino::Client& networkClient, const history::SwitchHistory& switchHistory,
    uint8_t channelIndex);
