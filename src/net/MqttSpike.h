// ПРОТОТИП (ветка spike/mqtt-socket, в main не сливается): проверка, что MQTT-соединение
// с брокером Home Assistant уживается с остальной сетью на лимите 4 сокета (D-031).
//
// Отдельный поток, как WifiConnector и FetchWorker: подключение к брокеру блокирует,
// loop() его не ждет. Поток публикует через MQTT Discovery датчик цены и состояния
// реле, а также доступность (online, offline — завещанием брокеру). Команд не
// принимает, реле не трогает.
//
// Настройки брокера — из flash (MqttSettingsStore): страница, команда mqtt, запуск.
// Пока uplink None (Wi-Fi подключается или связи нет), поток сеть не трогает и
// соединение закрывает.
#pragma once

#include <Arduino.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <mbed.h>

#include <ArduinoMqttClient.h>
#include <ChannelConfig.h>

#include "../integrations/RemoteControl.h"
#include "FetchWorker.h"
#include "Uplink.h"

class MqttSpike : public RemoteControl {
public:
    // Пауза между попытками подключения к брокеру.
    static constexpr uint32_t kRetryMs = 10UL * 1000UL;
    // Состояние публикуется при изменении и не реже этого.
    static constexpr uint32_t kRepublishMs = 60UL * 1000UL;

    struct Snapshot {
        bool hasPrice;
        float priceEurKwh;
        bool outputs[channelconfig::kChannelCount];
    };

    MqttSpike();
    // setup(): запуск потока.
    bool begin();

    // Основной поток. Брокер и учетные данные; пустой host — выключить.
    void configure(const char* host, uint16_t port, const char* user, const char* password);
    // Основной поток, каждый loop(): канал связи и текущее состояние для публикации.
    void setUplink(Uplink uplink);
    void setSnapshot(const Snapshot& snapshot);
    // Основной поток: строки лога потока.
    void flushLog() { log_.flushTo(Serial); }
    // Соединение с брокером открыто (занимает один сокет).
    bool connected();
    // Страница только для просмотра, пока соединение открыто (D-042).
    bool remoteControlActive() override { return connected(); }
    // Брокер со страницы, из консоли или из flash при запуске (D-042).
    void applyBrokerSettings(const configstore::MqttSettings& settings) override {
        configure(settings.host, settings.port, settings.user, settings.password);
    }
    // Идет подключение: команда sockets его не тревожит.
    bool connecting();
    // Основной поток, команда mqtt без аргументов.
    void printStatus();

private:
    static constexpr uint32_t kStackSize = 8 * 1024;

    void run();
    void step();
    bool connectBroker(Uplink uplink);
    void publishDiscovery();
    void publishState(const Snapshot& snapshot);
    void publish(const char* topic, const char* payload, bool retain);
    void closeConnection(const char* reason);

    MBED_ALIGN(8) unsigned char stack_[kStackSize];
    rtos::Thread thread_;
    rtos::Mutex mutex_;
    LineLog log_;

    // Только поток
    arduino::WiFiClient wifiClient_;
    arduino::EthernetClient ethernetClient_;
    MqttClient mqtt_;
    Uplink activeUplink_ = Uplink::None;
    bool hasLastAttempt_ = false;
    uint32_t lastAttemptMs_ = 0;
    uint32_t lastPublishMs_ = 0;
    Snapshot published_ = {};
    bool publishedValid_ = false;
    uint32_t configGeneration_ = 0;

    // Общие, под mutex_
    char host_[64] = {};
    uint16_t port_ = 1883;
    char user_[32] = {};
    char password_[64] = {};
    uint32_t generation_ = 0;     // растет при каждом configure()
    Uplink uplink_ = Uplink::None;
    Snapshot snapshot_ = {};
    bool connected_ = false;
    bool connecting_ = false;
    uint32_t connectCount_ = 0;
    uint32_t publishCount_ = 0;
    int lastError_ = 0;
};
