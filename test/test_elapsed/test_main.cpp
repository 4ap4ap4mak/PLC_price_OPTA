// Тесты lib/elapsed: интервал millis() с отметкой из другого потока (KI-031).
#include <unity.h>

#include <Elapsed.h>

static const uint32_t kStuckMs = 3UL * 60UL * 1000UL;

void setUp() {}
void tearDown() {}

void test_interval_passed_and_not_yet() {
    TEST_ASSERT_TRUE(elapsed::atLeast(1000 + kStuckMs, 1000, kStuckMs));
    TEST_ASSERT_TRUE(elapsed::atLeast(1000 + kStuckMs + 1, 1000, kStuckMs));
    TEST_ASSERT_FALSE(elapsed::atLeast(1000 + kStuckMs - 1, 1000, kStuckMs));
    TEST_ASSERT_FALSE(elapsed::atLeast(1000, 1000, kStuckMs));
}

// KI-031: поток записал отметку после того, как loop() снял now. Раньше
// now - since давало ~2^32 мс, задание считалось зависшим и устройство перезагружалось.
void test_bug_ki031_since_ahead_of_now_is_not_stuck() {
    TEST_ASSERT_FALSE(elapsed::atLeast(5000, 5001, kStuckMs));
    TEST_ASSERT_FALSE(elapsed::atLeast(5000, 5250, kStuckMs));
    // То же у переполнения millis(): now до перехода через 0, отметка — после
    TEST_ASSERT_FALSE(elapsed::atLeast(0xFFFFFFF0UL, 0x00000010UL, kStuckMs));
}

void test_interval_across_millis_overflow() {
    const uint32_t since = 0xFFFFFFFFUL - 1000UL;
    TEST_ASSERT_FALSE(elapsed::atLeast(since + 1000UL, since, kStuckMs));
    TEST_ASSERT_TRUE(elapsed::atLeast(since + kStuckMs, since, kStuckMs));  // через 0
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_interval_passed_and_not_yet);
    RUN_TEST(test_bug_ki031_since_ahead_of_now_is_not_stuck);
    RUN_TEST(test_interval_across_millis_overflow);
    return UNITY_END();
}
