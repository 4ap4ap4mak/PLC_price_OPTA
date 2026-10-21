// Запись HTTP-ответов в сетевой клиент (Wi-Fi или Ethernet — оба arduino::Client).
// Блокирует до 5 с без прогресса передачи, сбрасывая watchdog.
#pragma once

#include <Arduino.h>
#include <api/Client.h>

// Передать length байт кусками по 256. false — передано не всё (пишет [WEB] в лог).
bool writeClientData(arduino::Client& client, const uint8_t* data, size_t length);

// Сборка страницы в String, чтобы отправить ее с Content-Length.
class StringPageWriter : public Print {
public:
    explicit StringPageWriter(String& output) : page(output) {}

    size_t write(uint8_t value) override;
    size_t write(const uint8_t* data, size_t length) override;
    // Не хватило памяти: страница неполная.
    bool hasFailed() const {
        return failed;
    }

private:
    String& page;
    bool failed = false;
};

// Ответ кусками по мере формирования: без Content-Length, конец тела — закрытие
// соединения. Для истории: 512 строк не помещаются в String целиком.
class BufferedClientWriter : public Print {
public:
    explicit BufferedClientWriter(arduino::Client& client) : client_(client) {}

    size_t write(uint8_t value) override;
    size_t write(const uint8_t* data, size_t length) override;
    // Отправить остаток буфера. false — ответ передан не полностью.
    bool finish();

private:
    bool sendBuffer();

    arduino::Client& client_;
    uint8_t buffer_[1024];
    size_t used_ = 0;
    bool failed_ = false;
};
