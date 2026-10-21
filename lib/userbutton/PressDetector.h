// Распознавание нажатий кнопки USER: антидребезг, короткое и долгое нажатие.
// Чистый модуль L0: без Arduino, не логирует. Пин читает src/platform/UserButton,
// время — параметр nowMs (millis()); сравнения устойчивы к переполнению millis().
#pragma once

#include <stdint.h>

namespace userbutton {

// Кнопка должна держать новое состояние дольше этого времени (строго больше).
constexpr uint32_t kDebounceMs = 50;
// Удержание не меньше этого времени — долгое нажатие (без ожидания отпускания).
constexpr uint32_t kLongPressMs = 5000;

enum class Event : uint8_t {
    None,
    Pressed,     // кнопка нажата (после антидребезга)
    ShortPress,  // отпущена раньше kLongPressMs
    LongPress    // удерживается kLongPressMs; сообщается один раз за нажатие
};

class PressDetector {
public:
    // Вызывать в каждом loop(). pressed — кнопка сейчас нажата. За вызов — не больше
    // одного события.
    Event update(uint32_t nowMs, bool pressed);

private:
    bool lastReading_ = false;
    bool stablePressed_ = false;
    uint32_t lastChangeMs_ = 0;
    uint32_t pressedAtMs_ = 0;
    bool pressActive_ = false;
    bool longReported_ = false;
};

}  // namespace userbutton
