// Страница настроек GET /: выбор реле, панель принудительного включения, форма
// настроек канала и данные графика (рисует браузер, D-020).
// Страница собирается в String целиком (~24 КБ), чтобы отправить Content-Length;
// не хватило памяти — ответ 500. Построение графика обновляет фиксации
// планировщика канала (Controller::chartPlan), поэтому контроллер не const.
#pragma once

#include <Arduino.h>
#include <api/Client.h>

#include <Controller.h>
#include <ControllerPorts.h>

// channel — индекс канала, chartDay — сутки графика (-1 вчера, 0 сегодня, 1 завтра),
// wifiFields — показывать поля SSID и пароля, message — текст внизу страницы.
void sendSettingsPage(arduino::Client& networkClient, controller::Controller& channels,
    const controller::Clock& clock, uint8_t channel, int8_t chartDay, bool wifiFields,
    const String& message);
