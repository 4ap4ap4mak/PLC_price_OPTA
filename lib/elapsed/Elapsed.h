// Интервал millis(), отметку начала которого пишет другой поток (KI-031).
//
// Основной поток снимает now = millis() в начале итерации loop(), а рабочий поток может
// записать отметку начала задания (или последнего прогресса) уже после этого. Тогда
// отметка «впереди» now, и разность now - since в uint32_t дает почти 2^32 мс вместо
// отрицательной — «задание висит 49 дней». Здесь такая отметка считается текущей.
//
// Чистый модуль L0: без Arduino, покрывается тестами test_elapsed.
#pragma once

#include <stdint.h>

namespace elapsed {

// От sinceMs до nowMs прошло не меньше intervalMs. Переполнение millis() учитывается:
// разность до 2^31 мс (~24,8 суток) — прошедшее время, больше — отметка впереди now.
inline bool atLeast(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs) {
    const uint32_t passed = nowMs - sinceMs;
    return passed < 0x80000000UL && passed >= intervalMs;
}

}  // namespace elapsed
