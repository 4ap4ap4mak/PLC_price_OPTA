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
#include <MqttSettingsStore.h>

#include "../update/FirmwareUpdater.h"

// channel — индекс канала, chartDay — сутки графика (-1 вчера, 0 сегодня, 1 завтра),
// wifiFields — показывать поля SSID и пароля, message — текст под навигацией,
// firmware — панель «Прошивка» (D-038), remoteControl — подключен Home Assistant:
// плашка «Смотрите Home Assistant», поля и кнопки форм отключены и серые (D-042);
// mqtt — сохраненный брокер для панели «Home Assistant (MQTT)», nullptr — без панели.
void sendSettingsPage(arduino::Client& networkClient, controller::Controller& channels,
    const controller::Clock& clock, FirmwareUpdater& firmware, uint8_t channel,
    int8_t chartDay, bool wifiFields, const String& message, bool remoteControl,
    const configstore::MqttSettings* mqtt);

// Ответ 409 на POST, пока подключен Home Assistant (D-042).
void sendRemoteControlRefusal(arduino::Client& networkClient);

// Ответ на POST /save с новой сетью Wi-Fi (D-031): устройство подключается к ней без
// перезагрузки, точка настройки сейчас закроется. ssid — новая сеть, channel — реле
// для ссылки «Вернуться к настройкам» (работает по Ethernet и после подключения).
void sendWifiSavedPage(arduino::Client& networkClient, const char* ssid, uint8_t channel);
