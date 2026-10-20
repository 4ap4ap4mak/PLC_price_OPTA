// Тесты lib/history: кольцевой буфер переключений, время событий до NTP, формат.
#include <string.h>
#include <unity.h>

#include <SwitchHistory.h>

#include "../helpers/TestHelpers.h"

using namespace history;
using testing::riga;

void setUp() {}
void tearDown() {}

static SwitchHistory events;

static void test_newest_first() {
    events = SwitchHistory();
    TEST_ASSERT_EQUAL_size_t(0, events.size());
    events.record(100, 1, kDeviceEvent, false, Cause::Startup);
    events.record(200, 2, 0, true, Cause::Night);
    events.record(300, 3, 1, false, Cause::Ceiling);
    TEST_ASSERT_EQUAL_size_t(3, events.size());
    TEST_ASSERT_EQUAL_UINT32(300, events.newest(0).epoch);
    ASSERT_ENUM(Cause::Ceiling, events.newest(0).cause);
    TEST_ASSERT_EQUAL_UINT8(1, events.newest(0).channel);
    TEST_ASSERT_EQUAL_UINT32(200, events.newest(1).epoch);
    TEST_ASSERT_TRUE(events.newest(1).on);
    TEST_ASSERT_EQUAL_UINT8(kDeviceEvent, events.newest(2).channel);
    TEST_ASSERT_EQUAL_UINT32(0, events.dropped());
}

static void test_ring_drops_oldest() {
    events = SwitchHistory();
    const size_t total = SwitchHistory::kCapacity + 5;
    for (size_t i = 0; i < total; ++i) {
        events.record(static_cast<uint32_t>(1000 + i), static_cast<uint32_t>(i), 0, i % 2 == 0,
            Cause::Smart);
    }
    TEST_ASSERT_EQUAL_size_t(SwitchHistory::kCapacity, events.size());
    TEST_ASSERT_EQUAL_UINT32(5, events.dropped());
    TEST_ASSERT_EQUAL_UINT32(1000 + total - 1, events.newest(0).epoch);
    TEST_ASSERT_EQUAL_UINT32(1000 + 5, events.newest(SwitchHistory::kCapacity - 1).epoch);
}

// Запуск записывается до синхронизации часов; время проставляется задним числом.
static void test_fill_missing_epochs() {
    events = SwitchHistory();
    events.record(0, 0, kDeviceEvent, false, Cause::Startup);
    events.record(0, 30, 0, true, Cause::Manual);
    events.record(5000, 90, 0, false, Cause::Manual);
    events.fillMissingEpochs(4000, 100);
    TEST_ASSERT_EQUAL_UINT32(3900, events.newest(2).epoch);
    TEST_ASSERT_EQUAL_UINT32(3930, events.newest(1).epoch);
    TEST_ASSERT_EQUAL_UINT32(5000, events.newest(0).epoch);
}

static void test_format_event_time() {
    char text[32];
    SwitchEvent event = {riga(2026, 10, 20, 3, 15) + 7, 0, 0, true, Cause::Night};
    formatEventTime(event, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("20.10.2026 03:15:07", text);
    event = {riga(2026, 1, 5, 23, 59), 0, 0, true, Cause::Night};
    formatEventTime(event, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("05.01.2026 23:59:00", text);
    event = {0, 42, kDeviceEvent, false, Cause::Startup};
    formatEventTime(event, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("+42 с от запуска", text);
}

static void test_cause_names() {
    for (uint8_t cause = 0; cause <= static_cast<uint8_t>(Cause::NoPrice); ++cause) {
        TEST_ASSERT_TRUE(strlen(causeName(static_cast<Cause>(cause))) > 0);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_newest_first);
    RUN_TEST(test_ring_drops_oldest);
    RUN_TEST(test_fill_missing_epochs);
    RUN_TEST(test_format_event_time);
    RUN_TEST(test_cause_names);
    return UNITY_END();
}
