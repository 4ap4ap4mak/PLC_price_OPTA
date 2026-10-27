#include "MqttLink.h"

#include <string.h>

using channelconfig::kChannelCount;

namespace {

const char kHaStatusTopic[] = "homeassistant/status";

// FNV-1a: изменился ли буфер графика с прошлой сборки
uint32_t textHash(const char* text) {
    uint32_t hash = 2166136261UL;
    for (; *text != 0; ++text) {
        hash ^= static_cast<uint8_t>(*text);
        hash *= 16777619UL;
    }
    return hash;
}

}  // namespace

MqttLink::MqttLink()
    : thread_(osPriorityNormal, kStackSize, stack_, "mqtt"),
      mqtt_(wifiClient_) {}

bool MqttLink::begin(const char* deviceId, const char* firmwareVersion) {
    strncpy(deviceId_, deviceId, sizeof(deviceId_) - 1);
    firmwareVersion_ = firmwareVersion;
    return thread_.start(mbed::callback(this, &MqttLink::run)) == osOK;
}

void MqttLink::applyBrokerSettings(const configstore::MqttSettings& settings) {
    mutex_.lock();
    settings_ = settings;
    ++generation_;
    mutex_.unlock();
}

void MqttLink::setUplink(Uplink uplink) {
    mutex_.lock();
    uplink_ = uplink;
    mutex_.unlock();
}

void MqttLink::setState(const State& state) {
    mutex_.lock();
    state_ = state;
    mutex_.unlock();
}

bool MqttLink::takeCommand(hamqtt::Command& command) {
    mutex_.lock();
    const bool available = queueCount_ > 0;
    if (available) {
        command = queue_[queueHead_];
        queueHead_ = static_cast<uint8_t>((queueHead_ + 1) % kQueueSize);
        --queueCount_;
    }
    mutex_.unlock();
    return available;
}

bool MqttLink::connected() {
    mutex_.lock();
    const bool result = connected_;
    mutex_.unlock();
    return result;
}

bool MqttLink::lockChart() {
    return chartMutex_.trylock();
}

void MqttLink::unlockChart() {
    const char* buffers[1 + kChannelCount];
    buffers[0] = forecastJson_;
    for (uint8_t i = 0; i < kChannelCount; ++i) buffers[1 + i] = planJson_[i];
    for (uint8_t i = 0; i < 1 + kChannelCount; ++i) {
        const uint32_t hash = textHash(buffers[i]);
        if (hash != chartHash_[i]) {
            chartHash_[i] = hash;
            ++chartVersion_[i];
        }
    }
    chartMutex_.unlock();
}

bool MqttLink::sameState(const State& a, const State& b) {
    if (a.hasPrice != b.hasPrice || a.priceEurKwh != b.priceEurKwh) return false;
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (a.channels[i].output != b.channels[i].output ||
            a.channels[i].manual != b.channels[i].manual ||
            strcmp(a.channels[i].reason, b.channels[i].reason) != 0 ||
            memcmp(&a.channels[i].config, &b.channels[i].config,
                sizeof(a.channels[i].config)) != 0) {
            return false;
        }
    }
    return true;
}

// =============================================================================
// ПОТОК
// =============================================================================
void MqttLink::run() {
    for (;;) {
        step();
        delay(100);
    }
}

void MqttLink::step() {
    mutex_.lock();
    const bool configured = settings_.host[0] != '\0';
    const uint32_t generation = generation_;
    const Uplink uplink = uplink_;
    mutex_.unlock();

    if (activeUplink_ != Uplink::None) {
        if (!configured || generation != configGeneration_) {
            closeConnection("настройки брокера изменены");
        } else if (uplink != activeUplink_) {
            closeConnection("канал связи сменился или пропал");
        } else if (!mqtt_.connected()) {
            closeConnection("соединение потеряно");
        }
    }
    if (!configured || uplink == Uplink::None) return;

    if (activeUplink_ == Uplink::None) {
        const uint32_t nowMs = millis();
        // Новые настройки — попытка сразу, без паузы
        if (hasLastAttempt_ && generation == configGeneration_ &&
            nowMs - lastAttemptMs_ < kRetryMs) {
            return;
        }
        hasLastAttempt_ = true;
        lastAttemptMs_ = nowMs;
        configGeneration_ = generation;
        if (!connectBroker(uplink)) return;
        publishDiscovery();
        publishedValid_ = false;
        publishChart(true);
    }

    receive();

    mutex_.lock();
    const State state = state_;
    mutex_.unlock();
    const uint32_t nowMs = millis();
    if (!publishedValid_ || !sameState(state, published_) ||
        nowMs - lastPublishMs_ >= kRepublishMs) {
        publishState(state);
        published_ = state;
        publishedValid_ = true;
        lastPublishMs_ = nowMs;
    }
    publishChart(false);
}

// Прогноз и планы: изменившиеся с прошлой публикации или все (новое соединение,
// перезапуск Home Assistant). Основной поток в это время буферы не пишет: его
// lockChart() не ждет и пробует позже.
void MqttLink::publishChart(bool all) {
    chartMutex_.lock();
    for (uint8_t i = 0; i < 1 + kChannelCount; ++i) {
        if (chartVersion_[i] == 0) continue;  // еще не собран
        if (!all && publishedChart_[i] == chartVersion_[i]) continue;
        const char* json = i == 0 ? forecastJson_ : planJson_[i - 1];
        if (json[0] == 0) continue;  // не поместился в буфер
        if (i == 0) {
            hamqtt::forecastTopic(deviceId_, topic_, sizeof(topic_));
        } else {
            hamqtt::channelTopic(deviceId_, i - 1, "plan", topic_, sizeof(topic_));
        }
        publish(topic_, json, true);
        publishedChart_[i] = chartVersion_[i];
    }
    chartMutex_.unlock();
}

bool MqttLink::connectBroker(Uplink uplink) {
    mutex_.lock();
    const configstore::MqttSettings settings = settings_;
    mutex_.unlock();

    if (uplink == Uplink::Ethernet) {
        mqtt_.setClient(ethernetClient_);
    } else {
        mqtt_.setClient(wifiClient_);
    }
    mqtt_.setId(deviceId_);
    mqtt_.setUsernamePassword(settings.user, settings.password);
    mqtt_.setKeepAliveInterval(60UL * 1000UL);
    mqtt_.setConnectionTimeout(10UL * 1000UL);
    // Завещание: брокер сам опубликует offline, если плата пропадет
    hamqtt::availabilityTopic(deviceId_, topic_, sizeof(topic_));
    mqtt_.beginWill(topic_, true, 1);
    mqtt_.print("offline");
    mqtt_.endWill();

    log_.print("[MQTT] Подключение к ");
    log_.print(settings.host);
    log_.print(":");
    log_.print(settings.port);
    log_.print(" через ");
    log_.print(uplinkName(uplink));
    log_.println("...");
    const uint32_t startMs = millis();
    const bool ok = mqtt_.connect(settings.host, settings.port) != 0;
    const uint32_t durationMs = millis() - startMs;
    if (!ok) {
        log_.print("[MQTT] Не подключено за ");
        log_.print(durationMs);
        log_.print(" мс, код ");
        log_.print(mqtt_.connectError());
        log_.println(" (-2 — отказ соединения, -1 — таймаут, 4 — логин или пароль, 5 — нет прав).");
        mqtt_.stop();
        return false;
    }

    activeUplink_ = uplink;
    mutex_.lock();
    connected_ = true;
    mutex_.unlock();
    log_.print("[MQTT] Подключено за ");
    log_.print(durationMs);
    log_.print(" мс, устройство ");
    log_.print(deviceId_);
    log_.println(".");
    hamqtt::commandFilter(deviceId_, topic_, sizeof(topic_));
    mqtt_.subscribe(topic_);
    mqtt_.subscribe(kHaStatusTopic);
    hamqtt::availabilityTopic(deviceId_, topic_, sizeof(topic_));
    publish(topic_, "online", true);
    return true;
}

// Входящие: команды каналов — в очередь основного потока; перезапуск Home Assistant —
// описания заново.
void MqttLink::receive() {
    const int size = mqtt_.parseMessage();
    if (size <= 0) return;
    const String topic = mqtt_.messageTopic();
    const bool retained = mqtt_.messageRetain() != 0;
    char payload[hamqtt::kCommandValueSize + 8] = {};
    size_t length = 0;
    while (mqtt_.available() && length < sizeof(payload) - 1) {
        payload[length++] = static_cast<char>(mqtt_.read());
    }
    while (mqtt_.available()) mqtt_.read();

    if (topic == kHaStatusTopic) {
        if (strcmp(payload, "online") == 0) {
            log_.println("[MQTT] Home Assistant перезапущен: описания публикуются заново.");
            publishDiscovery();
            publishedValid_ = false;
            publishChart(true);
        }
        return;
    }

    hamqtt::Command command = {};
    if (!hamqtt::parseCommand(deviceId_, topic.c_str(), payload, length, command)) {
        log_.print("[MQTT] Неизвестная команда: ");
        log_.print(topic);
        log_.print(" = ");
        log_.println(payload);
        return;
    }
    // Сохраненная брокером команда пришла бы заново после каждого переподключения
    if (retained) {
        log_.print("[MQTT] Сохраненная брокером команда пропущена: ");
        log_.println(topic);
        return;
    }
    mutex_.lock();
    const bool queued = queueCount_ < kQueueSize;
    if (queued) {
        queue_[(queueHead_ + queueCount_) % kQueueSize] = command;
        ++queueCount_;
    }
    mutex_.unlock();
    if (!queued) {
        log_.println("[MQTT] Очередь команд переполнена; команда пропущена.");
    }
}

void MqttLink::closeConnection(const char* reason) {
    mqtt_.stop();
    activeUplink_ = Uplink::None;
    mutex_.lock();
    connected_ = false;
    mutex_.unlock();
    log_.print("[MQTT] Соединение закрыто: ");
    log_.print(reason);
    log_.println(".");
}

void MqttLink::publish(const char* topic, const char* payload, bool retain) {
    const size_t length = strlen(payload);
    if (!mqtt_.beginMessage(topic, length, retain)) return;
    mqtt_.write(reinterpret_cast<const uint8_t*>(payload), length);
    mqtt_.endMessage();
}

void MqttLink::publishDiscovery() {
    uint8_t published = 0;
    for (uint8_t i = 0; i < hamqtt::kDiscoveryCount; ++i) {
        if (!hamqtt::discoveryMessage(i, deviceId_, firmwareVersion_, topic_, sizeof(topic_),
                payload_, sizeof(payload_))) {
            log_.print("[MQTT] Описание ");
            log_.print(i);
            log_.println(" не поместилось в буфер.");
            continue;
        }
        publish(topic_, payload_, true);
        ++published;
    }
    log_.print("[MQTT] Описания для Home Assistant опубликованы: ");
    log_.print(published);
    log_.println(".");
}

void MqttLink::publishState(const State& state) {
    char value[24];
    hamqtt::priceTopic(deviceId_, topic_, sizeof(topic_));
    if (state.hasPrice) {
        hamqtt::formatPrice(state.priceEurKwh, value, sizeof(value));
    } else {
        strcpy(value, "unknown");
    }
    publish(topic_, value, true);
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        const ChannelState& channel = state.channels[i];
        hamqtt::channelTopic(deviceId_, i, "state", topic_, sizeof(topic_));
        publish(topic_, channel.output ? "ON" : "OFF", true);
        hamqtt::channelTopic(deviceId_, i, "mode", topic_, sizeof(topic_));
        publish(topic_, hamqtt::modeOption(hamqtt::channelMode(channel.manual, channel.output)), true);
        hamqtt::channelTopic(deviceId_, i, "reason", topic_, sizeof(topic_));
        publish(topic_, channel.reason, true);
        hamqtt::channelTopic(deviceId_, i, "settings", topic_, sizeof(topic_));
        if (hamqtt::formatSettingsJson(channel.config, payload_, sizeof(payload_))) {
            publish(topic_, payload_, true);
        }
    }
}
