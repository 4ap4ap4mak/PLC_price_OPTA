// Тесты lib/httpparse: поля urlencoded-формы, query и форма POST /save.
// Фиксируют особенности монолита (поиск поля подстрокой, декодирование %XX).
#include <string.h>
#include <string>
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

    int8_t day = 99;
    TEST_ASSERT_TRUE(queryChartDay("/?relay=1&day=1", day));
    TEST_ASSERT_EQUAL_INT8(1, day);
    TEST_ASSERT_TRUE(queryChartDay("/?day=0&relay=1", day));
    TEST_ASSERT_EQUAL_INT8(0, day);
    TEST_ASSERT_TRUE(queryChartDay("/chart.svg?relay=2&day=-1", day));
    TEST_ASSERT_EQUAL_INT8(-1, day);
    TEST_ASSERT_FALSE(queryChartDay("/?day=2", day));
    TEST_ASSERT_FALSE(queryChartDay("/?day=-2", day));
    TEST_ASSERT_FALSE(queryChartDay("/?day=", day));
    TEST_ASSERT_FALSE(queryChartDay("/?day=1x", day));
    TEST_ASSERT_FALSE(queryChartDay("/", day));
}

// KI-010: страница показывает то, что в ее запросе, без памяти о прошлых запросах
static void test_page_selection_from_own_request() {
    PageSelection s = pageSelection("/?relay=3&day=1");
    TEST_ASSERT_EQUAL_UINT8(2, s.channelIndex);
    TEST_ASSERT_EQUAL_INT8(1, s.chartDay);
    s = pageSelection("/?relay=2&day=-1");
    TEST_ASSERT_EQUAL_UINT8(1, s.channelIndex);
    TEST_ASSERT_EQUAL_INT8(-1, s.chartDay);

    // Следующий запрос без параметров — снова реле 1 и сегодня
    s = pageSelection("/");
    TEST_ASSERT_EQUAL_UINT8(0, s.channelIndex);
    TEST_ASSERT_EQUAL_INT8(0, s.chartDay);
    // Неверные значения — тоже по умолчанию, по отдельности
    s = pageSelection("/?relay=9&day=1");
    TEST_ASSERT_EQUAL_UINT8(0, s.channelIndex);
    TEST_ASSERT_EQUAL_INT8(1, s.chartDay);
    s = pageSelection("/?relay=4&day=5");
    TEST_ASSERT_EQUAL_UINT8(3, s.channelIndex);
    TEST_ASSERT_EQUAL_INT8(0, s.chartDay);
}

// D-042: при подключенном Home Assistant отклоняется все, кроме чтения
static void test_changes_state() {
    TEST_ASSERT_FALSE(changesState("GET"));
    TEST_ASSERT_FALSE(changesState("HEAD"));
    TEST_ASSERT_TRUE(changesState("POST"));
    TEST_ASSERT_TRUE(changesState("PUT"));
    TEST_ASSERT_TRUE(changesState("DELETE"));
    TEST_ASSERT_TRUE(changesState("get"));  // метод HTTP чувствителен к регистру
    TEST_ASSERT_TRUE(changesState(""));
}

static void test_home_location() {
    char location[kHomeLocationSize];
    formatHomeLocation(0, 0, location, sizeof(location));
    TEST_ASSERT_EQUAL_STRING("/?relay=1", location);
    formatHomeLocation(kChannelCount - 1, 0, location, sizeof(location));
    TEST_ASSERT_EQUAL_STRING("/?relay=4", location);
    // Адрес возврата открывает тот же канал
    TEST_ASSERT_EQUAL_UINT8(kChannelCount - 1, pageSelection(location).channelIndex);
}

// D-030: адрес возврата открывает ту же вкладку графика.
static void test_home_location_keeps_chart_day() {
    char location[kHomeLocationSize];
    formatHomeLocation(1, -1, location, sizeof(location));
    TEST_ASSERT_EQUAL_STRING("/?relay=2&day=-1", location);
    PageSelection s = pageSelection(location);
    TEST_ASSERT_EQUAL_UINT8(1, s.channelIndex);
    TEST_ASSERT_EQUAL_INT8(-1, s.chartDay);
    formatHomeLocation(kChannelCount - 1, 1, location, sizeof(location));
    TEST_ASSERT_EQUAL_STRING("/?relay=4&day=1", location);
    TEST_ASSERT_EQUAL_INT8(1, pageSelection(location).chartDay);
}

// D-030: страница истории знает реле, с которого пришли; без параметра — реле 1.
static void test_history_selection() {
    TEST_ASSERT_EQUAL_UINT8(2, pageSelection("/history?relay=3").channelIndex);
    TEST_ASSERT_EQUAL_UINT8(0, pageSelection("/history").channelIndex);
    TEST_ASSERT_EQUAL_UINT8(0, pageSelection("/history?relay=0").channelIndex);
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
    "&force_hours=4&holiday_hours=6&return_local=";

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
    TEST_ASSERT_FALSE(c.unusedBlockEnabled);
    TEST_ASSERT_EQUAL_UINT8(4, c.forceOnHours);
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
    TEST_ASSERT_EQUAL_STRING("Night hours exceed the window",
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

// Шаг 2c: флажок старого «непрерывного блока» сбрасывается при сохранении,
// длительность принудительного включения — поле force_hours 1…24.
static void test_settings_form_force_hours() {
    resetCurrent();
    current[1].unusedBlockEnabled = true;
    SettingsForm form;
    char body[600];
    snprintf(body, sizeof(body), "%s&block_enabled=1", kValidBody);
    ASSERT_ENUM(FormError::Ok, parseForm(body, form));
    TEST_ASSERT_FALSE(form.config.unusedBlockEnabled);
    TEST_ASSERT_EQUAL_UINT8(4, form.config.forceOnHours);

    snprintf(body, sizeof(body), "%s", kValidBody);
    memcpy(strstr(body, "force_hours=4"), "force_hours=0", 13);
    ASSERT_ENUM(FormError::InvalidScenario, parseForm(body, form));
    memcpy(strstr(body, "force_hours=0"), "block_hours=4", 13);  // старое имя поля
    ASSERT_ENUM(FormError::InvalidScenario, parseForm(body, form));
}

static void test_force_form() {
    ForceForm form;
    const char* start = "relay=3&action=start";
    TEST_ASSERT_TRUE(parseForceForm(start, strlen(start), form));
    TEST_ASSERT_EQUAL_UINT8(2, form.channelIndex);
    TEST_ASSERT_TRUE(form.start);

    const char* stop = "action=stop&relay=1";
    TEST_ASSERT_TRUE(parseForceForm(stop, strlen(stop), form));
    TEST_ASSERT_EQUAL_UINT8(0, form.channelIndex);
    TEST_ASSERT_FALSE(form.start);

    const char* bad[] = {"relay=5&action=start", "action=start", "relay=1",
        "relay=1&action=toggle", "relay=1&action="};
    for (const char* body : bad) {
        TEST_ASSERT_FALSE_MESSAGE(parseForceForm(body, strlen(body), form), body);
    }
}

// D-034: рычаг ручного режима со страницы.
static void test_manual_form() {
    ManualForm form;
    const char* on = "relay=2&action=on&chart_day=1";
    TEST_ASSERT_TRUE(parseManualForm(on, strlen(on), form));
    TEST_ASSERT_EQUAL_UINT8(1, form.channelIndex);
    ASSERT_ENUM(ManualAction::On, form.action);
    TEST_ASSERT_EQUAL_INT8(1, form.chartDay);

    const char* off = "action=off&relay=4";
    TEST_ASSERT_TRUE(parseManualForm(off, strlen(off), form));
    TEST_ASSERT_EQUAL_UINT8(3, form.channelIndex);
    ASSERT_ENUM(ManualAction::Off, form.action);
    TEST_ASSERT_EQUAL_INT8(0, form.chartDay);

    const char* release = "relay=1&action=auto";
    TEST_ASSERT_TRUE(parseManualForm(release, strlen(release), form));
    ASSERT_ENUM(ManualAction::Release, form.action);

    const char* bad[] = {"relay=0&action=on", "action=on", "relay=1",
        "relay=1&action=start", "relay=1&action="};
    for (const char* body : bad) {
        TEST_ASSERT_FALSE_MESSAGE(parseManualForm(body, strlen(body), form), body);
    }
}

// D-038: кнопки обновления прошивки.
static void test_firmware_form() {
    FirmwareForm form;
    const char* check = "relay=3&action=check&chart_day=-1";
    TEST_ASSERT_TRUE(parseFirmwareForm(check, strlen(check), form));
    TEST_ASSERT_EQUAL_UINT8(2, form.channelIndex);
    ASSERT_ENUM(FirmwareAction::Check, form.action);
    TEST_ASSERT_EQUAL_INT8(-1, form.chartDay);

    const char* install = "action=install&relay=1";
    TEST_ASSERT_TRUE(parseFirmwareForm(install, strlen(install), form));
    TEST_ASSERT_EQUAL_UINT8(0, form.channelIndex);
    ASSERT_ENUM(FirmwareAction::Install, form.action);
    TEST_ASSERT_EQUAL_INT8(0, form.chartDay);

    const char* bad[] = {"relay=5&action=check", "action=check", "relay=1",
        "relay=1&action=update", "relay=1&action="};
    for (const char* body : bad) {
        TEST_ASSERT_FALSE_MESSAGE(parseFirmwareForm(body, strlen(body), form), body);
    }
}

// D-030: вкладка графика из формы; нет поля или неверное — сегодня.
static void test_forms_chart_day() {
    ForceForm force;
    const char* withDay = "relay=2&action=start&chart_day=-1";
    TEST_ASSERT_TRUE(parseForceForm(withDay, strlen(withDay), force));
    TEST_ASSERT_EQUAL_INT8(-1, force.chartDay);
    const char* noDay = "relay=2&action=stop";
    TEST_ASSERT_TRUE(parseForceForm(noDay, strlen(noDay), force));
    TEST_ASSERT_EQUAL_INT8(0, force.chartDay);
    const char* badDay = "relay=2&action=stop&chart_day=7";
    TEST_ASSERT_TRUE(parseForceForm(badDay, strlen(badDay), force));
    TEST_ASSERT_EQUAL_INT8(0, force.chartDay);

    resetCurrent();
    std::string body = std::string(kValidBody) + "&chart_day=1";
    SettingsForm form;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FormError::Ok),
        static_cast<int>(parseForm(body.c_str(), form)));
    TEST_ASSERT_EQUAL_INT8(1, form.chartDay);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FormError::Ok),
        static_cast<int>(parseForm(kValidBody, form)));
    TEST_ASSERT_EQUAL_INT8(0, form.chartDay);
}

static void assertRequestLine(const char* line, const char* method, const char* path) {
    RequestLine parsed;
    parseRequestLine(line, parsed);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(method), parsed.methodLength, line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(line, method, parsed.methodLength), line);
    if (path == nullptr) {
        TEST_ASSERT_FALSE_MESSAGE(parsed.hasPath, line);
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(parsed.hasPath, line);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(path), parsed.pathLength, line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(line + parsed.pathStart, path, parsed.pathLength), line);
}

static void test_request_line() {
    assertRequestLine("GET / HTTP/1.1", "GET", "/");
    assertRequestLine("POST /save HTTP/1.1", "POST", "/save");
    assertRequestLine("GET /?relay=2&day=-1 HTTP/1.0", "GET", "/?relay=2&day=-1");
    // Особенности монолита: без версии путь не выделяется (по умолчанию "/"),
    // без пробела метод пустой, два пробела подряд дают пустой путь
    assertRequestLine("GET /history", "GET", nullptr);
    assertRequestLine("GARBAGE", "", nullptr);
    assertRequestLine("", "", nullptr);
    assertRequestLine("GET  HTTP/1.1", "GET", "");
    assertRequestLine(" / HTTP/1.1", "", "/");
}

static void test_content_length_header() {
    long value = -99;
    TEST_ASSERT_TRUE(parseContentLength("Content-Length: 123", value));
    TEST_ASSERT_EQUAL_INT32(123, value);
    TEST_ASSERT_TRUE(parseContentLength("CONTENT-LENGTH:42", value));
    TEST_ASSERT_EQUAL_INT32(42, value);
    TEST_ASSERT_TRUE(parseContentLength("content-length: -5", value));
    TEST_ASSERT_EQUAL_INT32(-5, value);
    // Как String::toInt: нечисловое значение — 0, хвост после цифр игнорируется
    TEST_ASSERT_TRUE(parseContentLength("Content-Length: abc", value));
    TEST_ASSERT_EQUAL_INT32(0, value);
    TEST_ASSERT_TRUE(parseContentLength("Content-Length: 17xyz", value));
    TEST_ASSERT_EQUAL_INT32(17, value);
    TEST_ASSERT_TRUE(parseContentLength("Content-Length:", value));
    TEST_ASSERT_EQUAL_INT32(0, value);

    value = 7;
    TEST_ASSERT_FALSE(parseContentLength("Content-Type: text/plain", value));
    TEST_ASSERT_FALSE(parseContentLength("Content-Length 5", value));
    TEST_ASSERT_FALSE(parseContentLength(" Content-Length: 5", value));
    TEST_ASSERT_FALSE(parseContentLength("Content-Len", value));
    TEST_ASSERT_FALSE(parseContentLength("", value));
    TEST_ASSERT_EQUAL_INT32(7, value);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_request_line);
    RUN_TEST(test_content_length_header);
    RUN_TEST(test_form_field_basic_and_decoding);
    RUN_TEST(test_form_field_quirks);
    RUN_TEST(test_parse_numbers);
    RUN_TEST(test_query_selection);
    RUN_TEST(test_page_selection_from_own_request);
    RUN_TEST(test_changes_state);
    RUN_TEST(test_home_location);
    RUN_TEST(test_home_location_keeps_chart_day);
    RUN_TEST(test_history_selection);
    RUN_TEST(test_settings_form_valid);
    RUN_TEST(test_settings_form_errors);
    RUN_TEST(test_settings_form_holiday);
    RUN_TEST(test_settings_form_wifi_and_restart);
    RUN_TEST(test_settings_form_force_hours);
    RUN_TEST(test_force_form);
    RUN_TEST(test_manual_form);
    RUN_TEST(test_firmware_form);
    RUN_TEST(test_forms_chart_day);
    return UNITY_END();
}
