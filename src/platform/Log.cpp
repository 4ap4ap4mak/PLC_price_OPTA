#include "Log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

void logLine(const char* tag, const char* format, ...) {
    char line[kLogLineSize];
    int prefix = snprintf(line, sizeof(line), "[%s] ", tag);
    if (prefix < 0) return;
    if (static_cast<size_t>(prefix) < sizeof(line)) {
        va_list args;
        va_start(args, format);
        vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
        va_end(args);
    }
    Serial.println(line);
}

void SerialLogSink::printText(const char* text) {
    Serial.print(text);
}

void SerialLogSink::printSigned(long value) {
    Serial.print(value);
}

void SerialLogSink::printUnsigned(unsigned long value) {
    Serial.print(value);
}

void SerialLogSink::printFloat(float value, uint8_t digits) {
    Serial.print(value, digits);
}
