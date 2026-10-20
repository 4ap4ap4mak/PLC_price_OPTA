// Тесты lib/forceon: таймер принудительного включения по millis() (D-006, D-007,
// docs/SCENARIOS.md §6): полный срок, перезапуск, остановка, переполнение millis().
#include <unity.h>

#include <ForceOnTimer.h>

using forceon::ForceOnTimer;
using forceon::formatRemaining;

void setUp() {}
void tearDown() {}

static const uint32_t kHourMs = 3600UL * 1000UL;

static void test_inactive_by_default() {
    ForceOnTimer timer;
    TEST_ASSERT_FALSE(timer.active());
    TEST_ASSERT_EQUAL_UINT32(0, timer.remainingMs(12345));
    TEST_ASSERT_FALSE(timer.expire(12345));
}

static void test_runs_full_duration() {
    ForceOnTimer timer;
    timer.start(1000, 3);
    TEST_ASSERT_TRUE(timer.active());
    TEST_ASSERT_EQUAL_UINT32(3 * kHourMs, timer.remainingMs(1000));
    TEST_ASSERT_FALSE(timer.expire(1000 + 3 * kHourMs - 1));
    TEST_ASSERT_TRUE(timer.active());
    TEST_ASSERT_EQUAL_UINT32(1, timer.remainingMs(1000 + 3 * kHourMs - 1));
    TEST_ASSERT_TRUE(timer.expire(1000 + 3 * kHourMs));
    TEST_ASSERT_FALSE(timer.active());
    // Истечение сообщается один раз
    TEST_ASSERT_FALSE(timer.expire(1000 + 3 * kHourMs + 5));
}

static void test_restart_counts_from_zero() {
    ForceOnTimer timer;
    timer.start(0, 2);
    timer.start(kHourMs + 500, 2);  // повторное нажатие через час
    TEST_ASSERT_FALSE(timer.expire(2 * kHourMs));
    TEST_ASSERT_EQUAL_UINT32(kHourMs + 500, timer.remainingMs(2 * kHourMs));
    TEST_ASSERT_TRUE(timer.expire(3 * kHourMs + 500));
}

static void test_stop() {
    ForceOnTimer timer;
    timer.start(0, 1);
    timer.stop();
    TEST_ASSERT_FALSE(timer.active());
    TEST_ASSERT_EQUAL_UINT32(0, timer.remainingMs(10));
    TEST_ASSERT_FALSE(timer.expire(2 * kHourMs));
}

static void test_survives_millis_overflow() {
    ForceOnTimer timer;
    const uint32_t start = 0xFFFFFFFFUL - kHourMs / 2;
    timer.start(start, 1);
    const uint32_t afterWrap = start + kHourMs / 2 + 1000;  // millis() уже переполнился
    TEST_ASSERT_TRUE(afterWrap < start);
    TEST_ASSERT_FALSE(timer.expire(afterWrap));
    TEST_ASSERT_EQUAL_UINT32(kHourMs / 2 - 1000, timer.remainingMs(afterWrap));
    TEST_ASSERT_TRUE(timer.expire(start + kHourMs));
}

static void test_hours_clamped() {
    ForceOnTimer timer;
    timer.start(0, 0);
    TEST_ASSERT_EQUAL_UINT32(kHourMs, timer.remainingMs(0));
    timer.start(0, 30);
    TEST_ASSERT_EQUAL_UINT32(24 * kHourMs, timer.remainingMs(0));
}

static void test_format_remaining() {
    char text[24];
    formatRemaining(3 * kHourMs, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("3 ч 00 мин", text);
    formatRemaining(2 * kHourMs + 4 * 60000UL + 1, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("2 ч 05 мин", text);  // минуты вверх
    formatRemaining(45 * 60000UL, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("45 мин", text);
    formatRemaining(1, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("1 мин", text);
    formatRemaining(24 * kHourMs, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("24 ч 00 мин", text);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_inactive_by_default);
    RUN_TEST(test_runs_full_duration);
    RUN_TEST(test_restart_counts_from_zero);
    RUN_TEST(test_stop);
    RUN_TEST(test_survives_millis_overflow);
    RUN_TEST(test_hours_clamped);
    RUN_TEST(test_format_remaining);
    return UNITY_END();
}
