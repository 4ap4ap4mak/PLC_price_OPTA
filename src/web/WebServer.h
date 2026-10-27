// HTTP-обработчик страницы настроек: читает запрос из принятого соединения
// (Wi-Fi или Ethernet), разбирает маршрут и отвечает. Маршруты: GET / (настройки),
// GET /history, POST /save, POST /force, POST /manual, POST /firmware, POST /mqtt
// (docs/INTERFACES.md). Состояния между запросами нет: реле и сутки страницы — из
// query (KI-010).
// Сокеты и прием соединений — NetworkManager.
//
// Блокирует до 3 с на строку запроса и заголовки и до 5 с на тело формы, сбрасывая
// watchdog. Обслуживает одно соединение за раз (KI-016). POST /save с новым Wi-Fi
// подключается к нему без перезагрузки (D-031), «Сохранить и перезапустить»
// перезагружает устройство.
// Пока подключен Home Assistant (RemoteControl), страница только для просмотра,
// POST отклоняется ответом 409 (D-042). Без RemoteControl (прошивка без MQTT) панели
// «Home Assistant (MQTT)» и маршрута POST /mqtt нет.
#pragma once

#include <Arduino.h>

#include <Controller.h>
#include <ControllerPorts.h>
#include <MqttSettingsStore.h>
#include <SwitchHistory.h>
#include <WifiCredentialStore.h>

#include "../integrations/RemoteControl.h"
#include "../net/NetworkManager.h"
#include "../update/FirmwareUpdater.h"

class WebServer : public HttpClientHandler {
public:
    WebServer(controller::Controller& channels, const controller::Clock& clock,
        NetworkManager& network, configstore::WifiCredentialStore& wifiCredentials,
        const history::SwitchHistory& switchHistory, FirmwareUpdater& firmware,
        configstore::MqttSettingsStore& mqttSettings, RemoteControl* remoteControl = nullptr)
        : channels_(channels), clock_(clock), network_(network),
          wifiCredentials_(wifiCredentials), switchHistory_(switchHistory),
          firmware_(firmware), mqttSettings_(mqttSettings), remoteControl_(remoteControl) {}

    // Обработать запрос и закрыть соединение.
    void handleClient(arduino::Client& client) override;

private:
    void handleForce(arduino::Client& client, int contentLength);
    void handleManual(arduino::Client& client, int contentLength);
    void handleSave(arduino::Client& client, int contentLength);
    void handleFirmware(arduino::Client& client, int contentLength);
    void handleMqtt(arduino::Client& client, int contentLength);

    controller::Controller& channels_;
    const controller::Clock& clock_;
    NetworkManager& network_;
    configstore::WifiCredentialStore& wifiCredentials_;
    const history::SwitchHistory& switchHistory_;
    FirmwareUpdater& firmware_;
    configstore::MqttSettingsStore& mqttSettings_;
    RemoteControl* remoteControl_;  // nullptr — пульта нет
};
