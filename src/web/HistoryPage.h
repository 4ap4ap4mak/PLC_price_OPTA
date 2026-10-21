// Страница GET /history: история переключений, новые сверху (D-019).
// Передается потоком без Content-Length (BufferedClientWriter).
#pragma once

#include <Arduino.h>
#include <api/Client.h>

#include <SwitchHistory.h>

void sendHistoryPage(arduino::Client& networkClient, const history::SwitchHistory& switchHistory);
