#include "MqttSpike.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

using channelconfig::kChannelCount;

namespace {

const char kClientId[] = "opta-spike";
const char kAvailabilityTopic[] = "opta-spike/status";
const char kPriceTopic[] = "opta-spike/price";
const char kHaStatusTopic[] = "homeassistant/status";
// Общая часть описаний: доступность и устройство, к которому HA привяжет сущности
const char kDeviceJson[] =
    "\"avty_t\":\"opta-spike/status\","
    "\"dev\":{\"ids\":[\"opta_spike\"],\"name\":\"Opta (прототип MQTT)\","
    "\"mf\":\"Arduino\",\"mdl\":\"Opta WiFi\"}";

}  // namespace

MqttSpike::MqttSpike()
    : thread_(osPriorityNormal, kStackSize, stack_, "mqtt"),
      mqtt_(wifiClient_) {}

bool MqttSpike::begin() {
    return thread_.start(mbed::callback(this, &MqttSpike::run)) == osOK;
}

void MqttSpike::configure(const char* host, uint16_t port, const char* user,
    const char* password) {
    mutex_.lock();
    strncpy(host_, host, sizeof(host_) - 1);
    host_[sizeof(host_) - 1] = '\0';
    port_ = port;
    strncpy(user_, user, sizeof(user_) - 1);
    user_[sizeof(user_) - 1] = '\0';
    strncpy(password_, password, sizeof(password_) - 1);
    password_[sizeof(password_) - 1] = '\0';
    ++generation_;
    mutex_.unlock();
}

void MqttSpike::setUplink(Uplink uplink) {
    mutex_.lock();
    uplink_ = uplink;
    mutex_.unlock();
}

void MqttSpike::setSnapshot(const Snapshot& snapshot) {
    mutex_.lock();
    snapshot_ = snapshot;
    mutex_.unlock();
}

bool MqttSpike::connected() {
    mutex_.lock();
    const bool result = connected_;
    mutex_.unlock();
    return result;
}

bool MqttSpike::connecting() {
    mutex_.lock();
    const bool result = connecting_;
    mutex_.unlock();
    return result;
}

void MqttSpike::printStatus() {
    mutex_.lock();
    const bool configured = host_[0] != '\0';
    char host[sizeof(host_)];
    strcpy(host, host_);
    const uint16_t port = port_;
    const bool isConnected = connected_;
    const bool isConnecting = connecting_;
    const uint32_t connects = connectCount_;
    const uint32_t publishes = publishCount_;
    const int lastError = lastError_;
    mutex_.unlock();

    if (!configured) {
        Serial.println("[MQTT] Брокер не задан. Формат: mqtt <IP> <пользователь> <пароль> [порт]; mqtt off — выключить.");
        return;
    }
    Serial.print("[MQTT] Брокер ");
    Serial.print(host);
    Serial.print(":");
    Serial.print(port);
    Serial.print(isConnected ? " — подключено" : (isConnecting ? " — подключение..." : " — нет соединения"));
    Serial.print("; подключений с запуска: ");
    Serial.print(connects);
    Serial.print(", публикаций: ");
    Serial.print(publishes);
    if (!isConnected && lastError != 0) {
        Serial.print("; последняя ошибка: ");
        Serial.print(lastError);
    }
    Serial.println();
}

// =============================================================================
// ПОТОК
// =============================================================================
void MqttSpike::run() {
    for (;;) {
        step();
        delay(100);
    }
}

void MqttSpike::step() {
    mutex_.lock();
    const bool configured = host_[0] != '\0';
    const uint32_t generation = generation_;
    const Uplink uplink = uplink_;
    const Snapshot snapshot = snapshot_;
    mutex_.unlock();

    const bool open = activeUplink_ != Uplink::None;
    if (open) {
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
        if (hasLastAttempt_ && nowMs - lastAttemptMs_ < kRetryMs) return;
        hasLastAttempt_ = true;
        lastAttemptMs_ = nowMs;
        configGeneration_ = generation;
        if (!connectBroker(uplink)) return;
        publishDiscovery();
        publishedValid_ = false;
    }

    // Входящие: только перезапуск HA — тогда описания публикуются заново
    const int size = mqtt_.parseMessage();
    if (size > 0) {
        const String topic = mqtt_.messageTopic();
        char payload[16] = {};
        size_t length = 0;
        while (mqtt_.available() && length < sizeof(payload) - 1) {
            payload[length++] = static_cast<char>(mqtt_.read());
        }
        while (mqtt_.available()) mqtt_.read();
        if (topic == kHaStatusTopic && strcmp(payload, "online") == 0) {
            log_.println("[MQTT] Home Assistant перезапущен: описания публикуются заново.");
            publishDiscovery();
            publishedValid_ = false;
        }
    }

    const uint32_t nowMs = millis();
    bool changed = snapshot.hasPrice != published_.hasPrice ||
        snapshot.priceEurKwh != published_.priceEurKwh;
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        changed = changed || snapshot.outputs[i] != published_.outputs[i];
    }
    if (!publishedValid_ || changed ||
        nowMs - lastPublishMs_ >= kRepublishMs) {
        publishState(snapshot);
        published_ = snapshot;
        publishedValid_ = true;
        lastPublishMs_ = nowMs;
    }
}

bool MqttSpike::connectBroker(Uplink uplink) {
    mutex_.lock();
    char host[sizeof(host_)];
    char user[sizeof(user_)];
    char password[sizeof(password_)];
    strcpy(host, host_);
    strcpy(user, user_);
    strcpy(password, password_);
    const uint16_t port = port_;
    connecting_ = true;
    mutex_.unlock();

    if (uplink == Uplink::Ethernet) {
        mqtt_.setClient(ethernetClient_);
    } else {
        mqtt_.setClient(wifiClient_);
    }
    mqtt_.setId(kClientId);
    mqtt_.setUsernamePassword(user, password);
    mqtt_.setKeepAliveInterval(60UL * 1000UL);
    mqtt_.setConnectionTimeout(10UL * 1000UL);
    // Завещание: брокер сам опубликует offline, если плата пропадет
    mqtt_.beginWill(kAvailabilityTopic, true, 1);
    mqtt_.print("offline");
    mqtt_.endWill();

    log_.print("[MQTT] Подключение к ");
    log_.print(host);
    log_.print(":");
    log_.print(port);
    log_.print(" через ");
    log_.print(uplinkName(uplink));
    log_.println("...");
    const uint32_t startMs = millis();
    const bool ok = mqtt_.connect(host, port) != 0;
    const uint32_t durationMs = millis() - startMs;

    mutex_.lock();
    connecting_ = false;
    lastError_ = ok ? 0 : mqtt_.connectError();
    if (ok) {
        connected_ = true;
        ++connectCount_;
    }
    mutex_.unlock();

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
    log_.print("[MQTT] Подключено за ");
    log_.print(durationMs);
    log_.println(" мс.");
    mqtt_.subscribe(kHaStatusTopic);
    publish(kAvailabilityTopic, "online", true);
    return true;
}

void MqttSpike::closeConnection(const char* reason) {
    mqtt_.stop();
    activeUplink_ = Uplink::None;
    mutex_.lock();
    connected_ = false;
    mutex_.unlock();
    log_.print("[MQTT] Соединение закрыто: ");
    log_.print(reason);
    log_.println(".");
}

void MqttSpike::publish(const char* topic, const char* payload, bool retain) {
    const size_t length = strlen(payload);
    if (!mqtt_.beginMessage(topic, length, retain)) return;
    mqtt_.write(reinterpret_cast<const uint8_t*>(payload), length);
    mqtt_.endMessage();
    mutex_.lock();
    ++publishCount_;
    mutex_.unlock();
}

void MqttSpike::publishDiscovery() {
    char topic[96];
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Цена\",\"uniq_id\":\"opta_spike_price\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"EUR/kWh\",\"dev_cla\":\"monetary\",\"sug_dsp_prc\":4,%s}",
        kPriceTopic, kDeviceJson);
    publish("homeassistant/sensor/opta_spike/price/config", payload, true);

    for (uint8_t i = 0; i < kChannelCount; ++i) {
        snprintf(topic, sizeof(topic),
            "homeassistant/binary_sensor/opta_spike/relay%u/config", i + 1U);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Реле %u\",\"uniq_id\":\"opta_spike_relay%u\","
            "\"stat_t\":\"opta-spike/relay/%u\",\"dev_cla\":\"power\",%s}",
            i + 1U, i + 1U, i + 1U, kDeviceJson);
        publish(topic, payload, true);
    }
    log_.println("[MQTT] Описания для Home Assistant опубликованы.");
}

void MqttSpike::publishState(const Snapshot& snapshot) {
    char topic[32];
    char value[24];
    if (snapshot.hasPrice) {
        // printf с плавающей точкой в ядре может быть не собран: 5 знаков вручную,
        // цена бывает отрицательной
        const long scaled = lroundf(snapshot.priceEurKwh * 100000.0f);
        const unsigned long magnitude = static_cast<unsigned long>(scaled < 0 ? -scaled : scaled);
        snprintf(value, sizeof(value), "%s%lu.%05lu", scaled < 0 ? "-" : "",
            magnitude / 100000UL, magnitude % 100000UL);
    } else {
        strcpy(value, "unknown");
    }
    publish(kPriceTopic, value, true);
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        snprintf(topic, sizeof(topic), "opta-spike/relay/%u", i + 1U);
        publish(topic, snapshot.outputs[i] ? "ON" : "OFF", true);
    }
}
