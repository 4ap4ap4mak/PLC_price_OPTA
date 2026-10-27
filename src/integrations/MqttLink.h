// Соединение с брокером MQTT для Home Assistant (D-042, D-043) в отдельном потоке,
// как FetchWorker и WifiConnector: подключение к брокеру блокирует, loop() его не ждет.
//
// Поток публикует описания сущностей (MQTT Discovery, lib/hamqtt), доступность
// (online; offline — завещанием брокеру), цену, выходы, режимы и причины каналов —
// при изменении и не реже раза в минуту. Команды Home Assistant (режим, принудительное
// включение) поток только разбирает и кладет в очередь; выполняет их основной поток
// через Controller (takeCommand), поэтому реле и Controller поток не трогает.
//
// Пока uplink None (Wi-Fi подключается или связи нет), поток сеть не трогает
// и соединение закрывает. Зависание подключения перезагрузкой не лечится: реле
// от MQTT не зависят, а занятый сокет не мешает запросу цены (D-043).
#pragma once

#include <Arduino.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <mbed.h>

#include <ArduinoMqttClient.h>
#include <ChannelConfig.h>
#include <HaMqtt.h>
#include <MqttSettingsStore.h>

#include "../net/FetchWorker.h"
#include "../net/Uplink.h"
#include "RemoteControl.h"

class MqttLink : public RemoteControl {
public:
    // Пауза между попытками подключения к брокеру.
    static constexpr uint32_t kRetryMs = 10UL * 1000UL;
    // Состояние публикуется при изменении и не реже этого.
    static constexpr uint32_t kRepublishMs = 60UL * 1000UL;

    struct ChannelState {
        bool output;
        bool manual;
        char reason[128];  // режим и причина, как на странице
    };
    struct State {
        bool hasPrice;
        float priceEurKwh;
        ChannelState channels[channelconfig::kChannelCount];
    };

    MqttLink();
    // setup(): идентификатор устройства (hamqtt::formatDeviceId) и запуск потока.
    bool begin(const char* deviceId, const char* firmwareVersion);

    // RemoteControl — основной поток
    bool remoteControlActive() override { return connected(); }
    void applyBrokerSettings(const configstore::MqttSettings& settings) override;

    // Основной поток, каждый loop(): канал связи и состояние для публикации.
    void setUplink(Uplink uplink);
    void setState(const State& state);
    // Основной поток: следующая команда Home Assistant. false — очередь пуста.
    bool takeCommand(hamqtt::Command& command);
    // Основной поток: строки лога потока.
    void flushLog() { log_.flushTo(Serial); }
    // Соединение с брокером открыто (занимает один сокет).
    bool connected();
    const char* deviceId() const { return deviceId_; }

private:
    static constexpr uint32_t kStackSize = 8 * 1024;
    static constexpr uint8_t kQueueSize = 8;

    void run();
    void step();
    bool connectBroker(Uplink uplink);
    void receive();
    void publishDiscovery();
    void publishState(const State& state);
    void publish(const char* topic, const char* payload, bool retain);
    void closeConnection(const char* reason);
    static bool sameState(const State& a, const State& b);

    MBED_ALIGN(8) unsigned char stack_[kStackSize];
    rtos::Thread thread_;
    rtos::Mutex mutex_;
    LineLog log_;
    char deviceId_[hamqtt::kDeviceIdSize] = {};
    const char* firmwareVersion_ = "";

    // Только поток
    arduino::WiFiClient wifiClient_;
    arduino::EthernetClient ethernetClient_;
    MqttClient mqtt_;
    Uplink activeUplink_ = Uplink::None;
    bool hasLastAttempt_ = false;
    uint32_t lastAttemptMs_ = 0;
    uint32_t lastPublishMs_ = 0;
    State published_ = {};
    bool publishedValid_ = false;
    uint32_t configGeneration_ = 0;
    char topic_[hamqtt::kTopicSize] = {};
    char payload_[hamqtt::kDiscoveryPayloadSize] = {};

    // Общие, под mutex_
    configstore::MqttSettings settings_ = {};
    uint32_t generation_ = 0;     // растет при каждом applyBrokerSettings()
    Uplink uplink_ = Uplink::None;
    State state_ = {};
    bool connected_ = false;
    hamqtt::Command queue_[kQueueSize] = {};
    uint8_t queueHead_ = 0;
    uint8_t queueCount_ = 0;
};
