// Подключение Wi-Fi к сохраненной сети в отдельном потоке (KI-006, D-029).
//
// В ядре mbed WiFi.begin() блокирует: поиск сети, подключение (таймаут 7 с) и DHCP —
// обычно несколько секунд, а при сбое DHCP до минуты. Поток выполняет одну попытку
// и отдает ее итог; основной поток тем временем обслуживает Ethernet, страницу,
// консоль, кнопку и реле.
//
// Пока идет попытка, основной поток не трогает WiFi, кроме WiFi.status(): точка
// настройки, сокеты Wi-Fi и следующая попытка ждут итога (NetworkManager).
// Watchdog поток не сбрасывает: зависание ловит stuck(), как у FetchWorker (D-026).
#pragma once

#include <Arduino.h>
#include <mbed.h>

#include <WifiCredentialStore.h>

class WifiConnector {
public:
    // Попытка дольше этого — поток завис в WiFi.begin(). Обычная попытка — поиск сети
    // и подключение до 7 с, DHCP до минуты.
    static constexpr uint32_t kStuckMs = 3UL * 60UL * 1000UL;

    enum class Result : uint8_t { Idle, Busy, Connected, Failed };

    WifiConnector();
    // setup(): запуск потока. false — поток не запустился, тогда start() подключает
    // синхронно и блокирует, как до D-029.
    bool begin();

    // Основной поток. Начать попытку; false — предыдущая еще идет.
    bool start(const char* ssid, const char* password);
    // Итог попытки: Busy, пока она идет; Connected/Failed — один раз.
    Result poll();
    // Длительность последней завершенной попытки, мс.
    uint32_t lastDurationMs();
    // Попытка идет дольше kStuckMs.
    bool stuck(uint32_t nowMs);

private:
    enum class JobState : uint8_t { Idle, Queued, Running, Done, Failed };
    static constexpr uint32_t kJobFlag = 1;
    static constexpr uint32_t kStackSize = 8 * 1024;

    void run();
    // WiFi.begin() с учетными данными задания; true — подключено.
    bool connect(uint32_t& durationMs);

    MBED_ALIGN(8) unsigned char stack_[kStackSize];
    rtos::Thread thread_;
    rtos::Mutex mutex_;
    rtos::EventFlags flags_;
    bool started_ = false;  // пишется в begin() до первой попытки

    // Общие, под mutex_. Учетные данные во время попытки читает только поток.
    char ssid_[sizeof(configstore::WifiCredentials::ssid)] = {};
    char password_[sizeof(configstore::WifiCredentials::password)] = {};
    JobState state_ = JobState::Idle;
    uint32_t runningSinceMs_ = 0;
    uint32_t durationMs_ = 0;
};
