// Тесты lib/timesync: расписание запросов NTP (KI-034).
#include <unity.h>

#include <TimeSyncSchedule.h>

using timesync::Schedule;
using timesync::due;

void setUp() {}
void tearDown() {}

static const uint32_t kRetry = 60UL * 1000UL;
static const uint32_t kResync = 6UL * 3600UL * 1000UL;

static void test_first_request_is_due() {
    Schedule schedule;
    TEST_ASSERT_TRUE(due(schedule, false, 0, kRetry, kResync));
    TEST_ASSERT_TRUE(due(schedule, true, 123, kRetry, kResync));
}

static void test_not_synced_retries_every_minute() {
    Schedule schedule;
    schedule.requested = true;
    schedule.lastRequestMs = 1000;
    TEST_ASSERT_FALSE(due(schedule, false, 1000 + kRetry - 1, kRetry, kResync));
    TEST_ASSERT_TRUE(due(schedule, false, 1000 + kRetry, kRetry, kResync));
}

static void test_synced_waits_six_hours() {
    Schedule schedule;
    schedule.requested = true;
    schedule.lastRequestMs = 1000;
    TEST_ASSERT_FALSE(due(schedule, true, 1000 + kRetry, kRetry, kResync));
    TEST_ASSERT_FALSE(due(schedule, true, 1000 + kResync - 1, kRetry, kResync));
    TEST_ASSERT_TRUE(due(schedule, true, 1000 + kResync, kRetry, kResync));
}

// KI-034: неудача после синхронизации — повтор через минуту, а не через 6 часов
static void test_failure_after_sync_retries_every_minute() {
    Schedule schedule;
    schedule.requested = true;
    schedule.lastFailed = true;
    schedule.lastRequestMs = 1000;
    TEST_ASSERT_FALSE(due(schedule, true, 1000 + kRetry - 1, kRetry, kResync));
    TEST_ASSERT_TRUE(due(schedule, true, 1000 + kRetry, kRetry, kResync));
}

static void test_millis_overflow() {
    Schedule schedule;
    schedule.requested = true;
    schedule.lastFailed = true;
    schedule.lastRequestMs = 0xFFFFFFFFUL - 1000;
    TEST_ASSERT_FALSE(due(schedule, true, 5000, kRetry, kResync));
    TEST_ASSERT_TRUE(due(schedule, true, kRetry - 1000, kRetry, kResync));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_request_is_due);
    RUN_TEST(test_not_synced_retries_every_minute);
    RUN_TEST(test_synced_waits_six_hours);
    RUN_TEST(test_failure_after_sync_retries_every_minute);
    RUN_TEST(test_millis_overflow);
    return UNITY_END();
}
