// Строка лога в Serial: "[TAG] текст" и перевод строки, как Serial.println.
// Строка собирается в буфере kLogLineSize байт (с тегом); длиннее — обрезается.
//
// Имя не logf: так называется функция из <math.h>.
// Дробные числа пока печатать через Serial.print(value, digits): поддержка %f
// в printf ядра mbed на железе не проверена.
#pragma once

#include <stddef.h>

constexpr size_t kLogLineSize = 256;

void logLine(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
