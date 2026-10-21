// Тесты lib/userbutton: антидребезг, короткое и долгое нажатие кнопки USER
// (как в loop() до шага 2d), переполнение millis().
#include <unity.h>

#include <PressDetector.h>

#include "../helpers/TestHelpers.h"

using userbutton::Event;
using userbutton::PressDetector;

void setUp() {}
void tearDown() {}

// Держать уровень pressed с шагом 1 мс от from до to включительно; вернуть первое
// событие (и время в atMs) или None.
static Event hold(PressDetector& button, bool pressed, uint32_t from, uint32_t to,
    uint32_t* atMs = nullptr) {
    for (uint32_t t = from;; ++t) {
        Event event = button.update(t, pressed);
        if (event != Event::None) {
            if (atMs != nullptr) *atMs = t;
            return event;
        }
        if (t == to) return Event::None;
    }
}

static void test_released_button_is_quiet() {
    PressDetector button;
    ASSERT_ENUM(Event::None, hold(button, false, 0, 10000));
}

// Новое состояние подтверждается, когда держится строго дольше 50 мс
static void test_press_confirmed_after_debounce() {
    PressDetector button;
    hold(button, false, 0, 1000);
    uint32_t at = 0;
    ASSERT_ENUM(Event::Pressed, hold(button, true, 1001, 2000, &at));
    TEST_ASSERT_EQUAL_UINT32(1001 + 51, at);
}

static void test_bounce_ignored() {
    PressDetector button;
    hold(button, false, 0, 1000);
    // Дребезг: уровень меняется чаще, чем раз в 50 мс
    for (uint32_t t = 1001; t < 1300; t += 20) {
        ASSERT_ENUM(Event::None, button.update(t, true));
        ASSERT_ENUM(Event::None, button.update(t + 10, false));
    }
    ASSERT_ENUM(Event::None, hold(button, false, 1300, 2000));
}

static void test_short_press_on_release() {
    PressDetector button;
    hold(button, false, 0, 1000);
    ASSERT_ENUM(Event::Pressed, hold(button, true, 1001, 1100));
    ASSERT_ENUM(Event::None, hold(button, true, 1053, 3000));
    uint32_t at = 0;
    ASSERT_ENUM(Event::ShortPress, hold(button, false, 3001, 4000, &at));
    TEST_ASSERT_EQUAL_UINT32(3001 + 51, at);
    ASSERT_ENUM(Event::None, hold(button, false, at + 1, 10000));
}

// Долгое нажатие — через 5 с после подтвержденного нажатия, не дожидаясь отпускания,
// один раз; отпускание после него не дает короткого нажатия
static void test_long_press_reported_once() {
    PressDetector button;
    hold(button, false, 0, 1000);
    uint32_t pressedAt = 0;
    ASSERT_ENUM(Event::Pressed, hold(button, true, 1001, 1100, &pressedAt));
    uint32_t at = 0;
    ASSERT_ENUM(Event::LongPress, hold(button, true, pressedAt + 1, pressedAt + 6000, &at));
    TEST_ASSERT_EQUAL_UINT32(pressedAt + 5000, at);
    ASSERT_ENUM(Event::None, hold(button, true, at + 1, at + 20000));
    ASSERT_ENUM(Event::None, hold(button, false, at + 20001, at + 30000));
}

static void test_release_just_before_long_press_is_short() {
    PressDetector button;
    hold(button, false, 0, 1000);
    uint32_t pressedAt = 0;
    hold(button, true, 1001, 1100, &pressedAt);
    // Отпускание подтверждается на 51-й мс: начать его за 52 мс до 5 с
    const uint32_t releaseStart = pressedAt + 5000 - 52;
    ASSERT_ENUM(Event::None, hold(button, true, pressedAt + 1, releaseStart - 1));
    ASSERT_ENUM(Event::ShortPress, hold(button, false, releaseStart, releaseStart + 100));
}

// Нажатая при включении кнопка тоже дает нажатие после антидребезга
static void test_pressed_at_boot() {
    PressDetector button;
    uint32_t at = 0;
    ASSERT_ENUM(Event::Pressed, hold(button, true, 0, 1000, &at));
    TEST_ASSERT_EQUAL_UINT32(51, at);
}

static void test_survives_millis_overflow() {
    PressDetector button;
    const uint32_t start = 0xFFFFFFFFUL - 2000;
    hold(button, false, start - 1000, start);
    uint32_t pressedAt = 0;
    ASSERT_ENUM(Event::Pressed, hold(button, true, start + 1, start + 100, &pressedAt));
    uint32_t at = 0;
    ASSERT_ENUM(Event::LongPress, hold(button, true, pressedAt + 1, pressedAt + 6000, &at));
    TEST_ASSERT_EQUAL_UINT32(pressedAt + 5000, at);
    TEST_ASSERT_TRUE(at < start);  // millis() уже переполнился
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_released_button_is_quiet);
    RUN_TEST(test_press_confirmed_after_debounce);
    RUN_TEST(test_bounce_ignored);
    RUN_TEST(test_short_press_on_release);
    RUN_TEST(test_long_press_reported_once);
    RUN_TEST(test_release_just_before_long_press_is_short);
    RUN_TEST(test_pressed_at_boot);
    RUN_TEST(test_survives_millis_overflow);
    return UNITY_END();
}
