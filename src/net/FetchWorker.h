// Рабочий поток сетевых запросов (шаг 2g, D-008, D-026): TLS-запросы Elering и NTP
// идут в отдельном rtos::Thread, loop() не блокируется (KI-006).
//
// Два независимых задания — цена (для Controller, порт PriceSource) и время (NTP для
// main.cpp); поток выполняет их по очереди, NTP первым. Основной поток ставит задание
// (start*) и забирает результат (poll*), поток отдает его под мьютексом.
//
// Лог потока не пишется в Serial напрямую: строки копятся в буфере и печатаются
// основным потоком в flushLog() и poll*() целыми строками, иначе строки двух потоков
// перемешались бы.
//
// Watchdog поток не сбрасывает: если сетевой вызов завис, loop() продолжает работать,
// а stuck() сообщает о задании дольше kStuckMs — main.cpp перезагружает устройство,
// как раньше это сделал бы watchdog (D-026).
#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>
#include <mbed.h>

#include <ControllerPorts.h>
#include <PriceSeries.h>

#include "EleringPriceSource.h"

// Print, который копит строки рабочего потока для печати основным потоком.
class LineLog : public Print {
public:
    // Рабочий поток. Строка уходит в общий буфер по '\n'; длиннее kLineCap — обрезается.
    size_t write(uint8_t c) override;
    // Основной поток: напечатать накопленные строки в out.
    void flushTo(Print& out);

private:
    static constexpr size_t kLineCap = 384;
    static constexpr size_t kBufferCap = 3072;

    rtos::Mutex mutex_;
    // Только рабочий поток
    char line_[kLineCap];
    size_t lineLen_ = 0;
    bool lineTruncated_ = false;
    // Общие, под mutex_
    char buffer_[kBufferCap];
    size_t bufferLen_ = 0;
    uint16_t droppedLines_ = 0;
};

class FetchWorker : public controller::PriceSource {
public:
    // Задание дольше этого — поток завис в сетевом вызове. Обычный запрос: DNS до 15 с,
    // TLS-соединение, чтение ответа до 20 с.
    static constexpr uint32_t kStuckMs = 3UL * 60UL * 1000UL;

    FetchWorker();
    // setup(): запуск потока. false — поток не запустился: start* отвечают false,
    // и Controller считает каждый запрос неудачным.
    bool begin();

    // PriceSource — основной поток (Controller)
    bool startCurrent() override;
    bool startPrices(const char* path, prices::PriceSeries& out) override;
    controller::FetchStatus poll(float& eurKwh, long& timestamp) override;

    // NTP — основной поток (main.cpp). false — запрос времени уже идет.
    bool startTimeSync();
    // При Done — epoch UTC в момент atMs (millis()).
    controller::FetchStatus pollTimeSync(uint32_t& epoch, uint32_t& atMs);

    // Основной поток, каждый loop(): напечатать строки лога рабочего потока.
    void flushLog() { log_.flushTo(Serial); }
    // Задание выполняется дольше kStuckMs.
    bool stuck(uint32_t nowMs);

private:
    enum class JobState : uint8_t { Idle, Queued, Running, Done, Failed };
    static constexpr uint32_t kJobFlag = 1;
    static constexpr uint32_t kStackSize = 16 * 1024;

    void run();
    void runTimeJob();
    void runPriceJob();
    // Результат задания: Busy, пока оно идет; Done/Failed — один раз.
    static controller::FetchStatus takeResult(JobState& state);

    MBED_ALIGN(8) unsigned char stack_[kStackSize];
    rtos::Thread thread_;
    rtos::Mutex mutex_;
    rtos::EventFlags flags_;
    LineLog log_;
    bool started_ = false;  // пишется в begin() до первых заданий
    // Только рабочий поток
    EleringPriceSource elering_;
    arduino::WiFiUDP ntpUdp_;
    bool ntpUdpStarted_ = false;

    // Общие, под mutex_
    JobState priceState_ = JobState::Idle;
    bool priceCurrent_ = false;              // текущая цена или цены за период
    char pricePath_[96] = {};
    prices::PriceSeries* priceOut_ = nullptr; // во время задания пишет только поток
    float priceEurKwh_ = 0.0f;
    long priceTimestamp_ = 0;
    JobState timeState_ = JobState::Idle;
    uint32_t timeEpoch_ = 0;
    uint32_t timeAtMs_ = 0;
    bool running_ = false;
    uint32_t runningSinceMs_ = 0;
};
