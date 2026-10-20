// Тесты lib/httpparse: поля urlencoded-формы, query и форма POST /save.
// Фиксируют особенности монолита (поиск поля подстрокой, декодирование %XX).
#include <string.h>
#include <unity.h>

#include <HttpParse.h>

#include "../helpers/TestHelpers.h"

using namespace httpparse;
using testing::riga;

void setUp() {}
void tearDown() {}

static char value[kMaxFormBody + 1];

static size_t field(const char* body, const char* name) {
    return formField(body, strlen(body), name, value);
}

static void test_form_field_basic_and_decoding() {
    TEST_ASSERT_EQUAL_size_t(3, field("a=1&bb=xyz&c=", "bb"));
    TEST_ASSERT_EQUAL_STRING("xyz", value);
    TEST_ASSERT_EQUAL_size_t(0, field("a=1&bb=xyz&c=", "c"));
    TEST_ASSERT_EQUAL_size_t(0, field("a=1", "missing"));
    TEST_ASSERT_EQUAL_STRING("", value);
    field("ssid=My+Home%21%2F", "ssid");
    TEST_ASSERT_EQUAL_STRING("My Home!/", value);
    // Неполная последовательность в конце остается как есть
    field("p=ab%4", "p");
    TEST_ASSERT_EQUAL_STRING("ab%4", value);
}

static void test_form_field_quirks() {
    // Поле ищется подстрокой: "ssid=" находится внутри "manual_ssid="
    field("manual_ssid=Home&ssid=Other", "ssid");
    TEST_ASSERT_EQUAL_STRING("Home", value);
    // Нешестнадцатеричный %XX дает нулевой байт: длина 3, строка обрывается
    TEST_ASSERT_EQUAL_size_t(3, field("x=a%G1b", "x"));
    TEST_ASSERT_EQUAL_STRING("a", value);
}

static void test_parse_numbers() {
    float f = 0;
    int i = 0;
    TEST_ASSERT_TRUE(parseFormFloat("0.25", 4, 0, 100, f));
    TEST_ASSERT_FLOAT_WITHIN(1e-7f, 0.25f, f);
    TEST_ASSERT_TRUE(parseFormFloat("-1.5", 4, -100, 100, f));
    TEST_ASSERT_FALSE(parseFormFloat("", 0, 0, 100, f));
    TEST_ASSERT_FALSE(parseFormFloat("1x", 2, 0, 100, f));
    TEST_ASSERT_FALSE(parseFormFloat("101", 3, 0, 100, f));
    TEST_ASSERT_FALSE(parseFormFloat("nan", 3, -100, 100, f));
    TEST_ASSERT_TRUE(parseFormInteger("24", 2, 1, 24, i));
    TEST_ASSERT_EQUAL_INT(24, i);
    TEST_ASSERT_FALSE(parseFormInteger("25", 2, 1, 24, i));
    TEST_ASSERT_FALSE(parseFormInteger("2.5", 3, 1, 24, i));
    TEST_ASSERT_FALSE(parseFormInteger("", 0, 1, 24, i));
}

static void test_query_selection() {
    uint8_t index = 99;
    TEST_ASSERT_TRUE(queryChannel("/?relay=3&day=1", index));
    TEST_ASSERT_EQUAL_UINT8(2, index);
    TEST_ASSERT_TRUE(queryChannel("/chart.svg?relay=4", index));
    TEST_ASSERT_EQUAL_UINT8(3, index);
    TEST_ASSERT_FALSE(queryChannel("/?relay=5", index));
    TEST_ASSERT_FALSE(queryChannel("/?relay=0", index));
    TEST_ASSERT_FALSE(queryChannel("/?day=1&relay=2", index));  // только "?relay="
    TEST_ASSERT_FALSE(queryChannel("/", index));

    uint8_t day = 99;
    TEST_ASSERT_TRUE(queryChartDay("/?relay=1&day=1", day));
    TEST_ASSERT_EQUAL_UINT8(1, day);
    TEST_ASSERT_TRUE(queryChartDay("/?day=0&relay=1", day));
    TEST_ASSERT_EQUAL_UINT8(0, day);
    TEST_ASSERT_FALSE(queryChartDay("/?day=2", day));
    TEST_ASSERT_FALSE(queryChartDay("/?day=", day));
    TEST_ASSERT_FALSE(queryChartDay("/?day=1x", day));
    TEST_ASSERT_FALSE(queryChartDay("/", day));
}

static ChannelConfig current[kChannelCount];

static void resetCurrent() {
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        channelconfig::setDefaults(current[i]);
    }
    current[1].holidayReturnEpoch = riga(2026, 11, 1, 18);
}

static const char* kValidBody =
    "relay=2&safety=0.35&threshold=0.02&smart_enabled=1&smart_hours=5"
    "&wakeup_enabled=1&wakeup_hours=2&wakeup_time=06%3A45"
    "&night_enabled=1&night_start=22%3A00&night_end=06%3A00&night_hours=3"
    "&block_hours=4&holiday_hours=6&return_local=";

static FormError parseForm(const char* body, SettingsForm& form, uint32_t now = 0,
    bool wifi = false) {
    return parseSettingsForm(body, strlen(body), current, now, wifi, form);
}

static void test_settings_form_valid() {
    resetCurrent();
    SettingsForm form;
    ASSERT_ENUM(FormError::Ok, parseForm(kValidBody, form));
    TEST_ASSERT_EQUAL_UINT8(1, form.channelIndex);
    const ChannelConfig& c = form.config;
    TEST_ASSERT_EQUAL_UINT16(5, c.version);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.35f, c.safetyCeilingEurKwh);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.02f, c.alwaysOnBelowPriceEurKwh);
    TEST_ASSERT_TRUE(c.smartThresholdEnabled);
    TEST_ASSERT_EQUAL_UINT8(5, c.smartThresholdHours);
    TEST_ASSERT_TRUE(c.wakeupEnabled);
    TEST_ASSERT_EQUAL_UINT8(2, c.wakeupHours);
    TEST_ASSERT_EQUAL_UINT16(6 * 60 + 45, c.wakeupTimeMinuteOfDay);
    TEST_ASSERT_TRUE(c.nightPrepEnabled);
    TEST_ASSERT_EQUAL_UINT16(22 * 60, c.nightStartMinuteOfDay);
    TEST_ASSERT_EQUAL_UINT16(6 * 60, c.nightEndMinuteOfDay);
    TEST_ASSERT_EQUAL_UINT8(3, c.nightCheapHours);
    TEST_ASSERT_FALSE(c.consecutiveBlockEnabled);
    TEST_ASSERT_EQUAL_UINT8(4, c.consecutiveBlockHours);
    TEST_ASSERT_FALSE(c.holidayEnabled);
    TEST_ASSERT_EQUAL_UINT8(6, c.holidayOnHours);
    // Пустая дата возвращения сохраняет прежнюю
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 11, 1, 18), c.holidayReturnEpoch);
    TEST_ASSERT_FALSE(form.hasWifi);
    TEST_ASSERT_FALSE(form.restartRequested);
}

static void test_settings_form_errors() {
    resetCurrent();
    SettingsForm form;
    ASSERT_ENUM(FormError::InvalidChannel, parseForm("relay=5&safety=1", form));
    ASSERT_ENUM(FormError::InvalidChannel, parseForm("safety=1", form));

    // Ошибка в остальных полях: номер канала уже известен
    ASSERT_ENUM(FormError::InvalidScenario, parseForm("relay=3&safety=1", form));
    TEST_ASSERT_EQUAL_UINT8(2, form.channelIndex);

    char body[600];
    snprintf(body, sizeof(body), "%s", kValidBody);
    char* threshold = strstr(body, "threshold=0.02");
    memcpy(threshold, "threshold=0.40", 14);  // порог выше предела
    ASSERT_ENUM(FormError::InvalidScenario, parseForm(body, form));

    snprintf(body, sizeof(body), "%s", kValidBody);
    memcpy(strstr(body, "night_end=06"), "night_end=22", 12);  // начало == конец
    ASSERT_ENUM(FormError::InvalidScenario, parseForm(body, form));

    snprintf(body, sizeof(body), "%s", kValidBody);
    memcpy(strstr(body, "night_hours=3"), "night_hours=9", 13);  // окно 8 часов
    ASSERT_ENUM(FormError::NightHoursExceedWindow, parseForm(body, form));
    TEST_ASSERT_EQUAL_STRING("Night heating hours exceed the window",
        formErrorText(FormError::NightHoursExceedWindow));

    snprintf(body, sizeof(body), "%sbad-date", kValidBody);
    ASSERT_ENUM(FormError::InvalidReturnDate, parseForm(body, form));
    snprintf(body, sizeof(body), "%s2026-03-29T03%%3A30", kValidBody);  // нет такого времени
    ASSERT_ENUM(FormError::InvalidReturnDate, parseForm(body, form));
}

static void test_settings_form_holiday() {
    resetCurrent();
    SettingsForm form;
    char body[700];
    snprintf(body, sizeof(body), "%s2026-10-25T18%%3A00&holiday_enabled=1", kValidBody);
    ASSERT_ENUM(FormError::Ok, parseForm(body, form, riga(2026, 10, 20, 12)));
    TEST_ASSERT_TRUE(form.config.holidayEnabled);
    TEST_ASSERT_EQUAL_UINT32(riga(2026, 10, 25, 18), form.config.holidayReturnEpoch);

    // Дата в прошлом при синхронизированных часах
    ASSERT_ENUM(FormError::ReturnDateNotFuture, parseForm(body, form, riga(2026, 10, 26, 12)));
    // Часы не синхронизированы (now == 0): прошлое не проверяется
    ASSERT_ENUM(FormError::Ok, parseForm(body, form, 0));

    // Отпуск без даты у канала без сохраненной даты
    snprintf(body, sizeof(body), "%s&holiday_enabled=1", kValidBody);
    char* relay = strstr(body, "relay=2");
    relay[6] = '1';
    ASSERT_ENUM(FormError::ReturnDateNotFuture, parseForm(body, form));
}

static void test_settings_form_wifi_and_restart() {
    resetCurrent();
    SettingsForm form;
    char body[800];
    snprintf(body, sizeof(body), "%s&manual_ssid=My+Net&password=p%%40ss&action=save_restart",
        kValidBody);
    // Поля Wi-Fi игнорируются, если на странице их нет
    ASSERT_ENUM(FormError::Ok, parseForm(body, form, 0, false));
    TEST_ASSERT_FALSE(form.hasWifi);
    TEST_ASSERT_TRUE(form.restartRequested);

    ASSERT_ENUM(FormError::Ok, parseForm(body, form, 0, true));
    TEST_ASSERT_TRUE(form.hasWifi);
    TEST_ASSERT_EQUAL_STRING("My Net", form.ssid);
    TEST_ASSERT_EQUAL_STRING("p@ss", form.password);

    // Пустой manual_ssid → поле ssid; но "ssid=" сначала находится внутри "manual_ssid="
    snprintf(body, sizeof(body), "%s&manual_ssid=&ssid=Other", kValidBody);
    ASSERT_ENUM(FormError::Ok, parseForm(body, form, 0, true));
    TEST_ASSERT_FALSE(form.hasWifi);

    snprintf(body, sizeof(body), "%s&manual_ssid=%s", kValidBody,
        "123456789012345678901234567890123");  // 33 символа
    ASSERT_ENUM(FormError::InvalidWifi, parseForm(body, form, 0, true));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_form_field_basic_and_decoding);
    RUN_TEST(test_form_field_quirks);
    RUN_TEST(test_parse_numbers);
    RUN_TEST(test_query_selection);
    RUN_TEST(test_settings_form_valid);
    RUN_TEST(test_settings_form_errors);
    RUN_TEST(test_settings_form_holiday);
    RUN_TEST(test_settings_form_wifi_and_restart);
    return UNITY_END();
}
