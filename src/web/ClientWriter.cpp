#include "ClientWriter.h"

#include <string.h>

#include "../platform/Watchdog.h"

bool writeClientData(arduino::Client& client, const uint8_t* data, size_t length) {
    size_t sent = 0;
    unsigned long startedAt = millis();
    while (sent < length && millis() - startedAt < 5000UL) {
        kickWatchdog();
        size_t chunkSize = length - sent;
        if (chunkSize > 256) chunkSize = 256;
        size_t written = client.write(data + sent, chunkSize);
        if (written > 0) {
            sent += written;
            startedAt = millis();
        } else {
            delay(2);
        }
    }
    if (sent != length) {
        Serial.print("[WEB] Ошибка передачи HTTP-ответа: отправлено ");
        Serial.print(sent);
        Serial.print(" из ");
        Serial.print(length);
        Serial.println(" байт.");
        return false;
    }
    return true;
}

size_t StringPageWriter::write(uint8_t value) {
    if (failed || !page.concat(static_cast<char>(value))) {
        failed = true;
        return 0;
    }
    return 1;
}

size_t StringPageWriter::write(const uint8_t* data, size_t length) {
    if (failed || !page.concat(data, static_cast<unsigned int>(length))) {
        failed = true;
        return 0;
    }
    return length;
}

size_t BufferedClientWriter::write(uint8_t value) {
    return write(&value, 1);
}

size_t BufferedClientWriter::write(const uint8_t* data, size_t length) {
    size_t done = 0;
    while (done < length) {
        if (used_ == sizeof(buffer_) && !sendBuffer()) return done;
        size_t chunk = length - done;
        if (chunk > sizeof(buffer_) - used_) chunk = sizeof(buffer_) - used_;
        memcpy(buffer_ + used_, data + done, chunk);
        used_ += chunk;
        done += chunk;
    }
    return length;
}

bool BufferedClientWriter::finish() {
    return sendBuffer();
}

bool BufferedClientWriter::sendBuffer() {
    if (failed_) return false;
    if (used_ > 0) {
        failed_ = !writeClientData(client_, buffer_, used_);
        used_ = 0;
    }
    return !failed_;
}
