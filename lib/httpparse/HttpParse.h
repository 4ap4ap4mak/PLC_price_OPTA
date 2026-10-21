// Разбор query и urlencoded-формы веб-страницы настроек.
// Чистый модуль L0: без Arduino, String и сети, не логирует, без динамической памяти.
//
// Перенесено из монолита без изменения поведения, включая особенности:
// поле ищется как подстрока "name=" в любом месте тела (без учета границ полей),
// а "%XX" декодируется через strtoul, поэтому "%G1" дает байт 0.
// Тело может содержать нулевые байты: длина передается явно, а сравнения строк
// работают как в Arduino String (strcmp/strtol останавливаются на нуле).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ChannelConfig.h>

namespace httpparse {

using channelconfig::ChannelConfig;
using channelconfig::kChannelCount;

// Максимальный размер тела POST /save и POST /force.
constexpr size_t kMaxFormBody = 2048;

// Строка запроса "МЕТОД ПУТЬ ВЕРСИЯ" (без \r\n и пробелов по краям). Как в монолите
// (шаг 2f): метод — до первого пробела, путь — между первым и вторым пробелом.
// Нет первого пробела — метод пустой; нет второго — путь по умолчанию "/".
// Разбор идет до нулевого байта (как String::indexOf).
struct RequestLine {
    size_t methodLength;  // метод — line[0, methodLength)
    bool hasPath;         // false — путь "/"
    size_t pathStart;     // путь — line[pathStart, pathStart + pathLength), может быть пустым
    size_t pathLength;
};
void parseRequestLine(const char* line, RequestLine& out);

// Заголовок "Content-Length: N" без учета регистра имени. true — это он, тогда
// value = N как atol: пробелы перед числом пропускаются, нечисловое значение дает 0.
// Иначе false, value не меняется.
bool parseContentLength(const char* headerLine, long& value);

// Значение поля: декодирует %XX и '+' в out. Возвращает длину значения
// (0, если поля нет). out должен вмещать не меньше bodyLen + 1 байт.
size_t formField(const char* body, size_t bodyLen, const char* name, char* out);

// Число из текста целиком, в пределах [minimum, maximum]. Пустой текст — false.
bool parseFormFloat(const char* text, size_t len, float minimum, float maximum, float& value);
bool parseFormInteger(const char* text, size_t len, int minimum, int maximum, int& value);

// GET /?relay=N — номер канала 1…kChannelCount → индекс. false, если параметра нет
// или номер вне диапазона.
bool queryChannel(const char* path, uint8_t& channelIndex);
// GET /?…day=D (или &day=D), D — -1 (вчера), 0 (сегодня) или 1 (завтра).
bool queryChartDay(const char* path, int8_t& day);

enum class FormError : uint8_t {
    Ok,
    InvalidChannel,
    InvalidScenario,
    NightHoursExceedWindow,
    InvalidReturnDate,
    ReturnDateNotFuture,
    InvalidWifi
};
// Текст тела ответа 400, как в монолите.
const char* formErrorText(FormError error);

struct SettingsForm {
    uint8_t channelIndex;   // заполнен при любой ошибке, кроме InvalidChannel
    ChannelConfig config;   // новая конфигурация выбранного канала
    bool hasWifi;           // введены новые учетные данные Wi-Fi
    char ssid[33];
    char password[64];
    bool restartRequested;  // кнопка «Сохранить и перезапустить»
};

// Форма POST /save → новая конфигурация канала. current — текущие конфигурации
// (непереданные поля берутся оттуда). now == 0 — часы не синхронизированы,
// тогда дата возвращения из отпуска проверяется только на непустоту.
FormError parseSettingsForm(const char* body, size_t bodyLen,
    const ChannelConfig current[kChannelCount], uint32_t now, bool wifiFieldsVisible,
    SettingsForm& out);

struct ForceForm {
    uint8_t channelIndex;
    bool start;  // true — включить на forceOnHours (или перезапустить), false — остановить
};

// Форма POST /force: relay=N&action=start|stop. false — неверный канал или действие.
bool parseForceForm(const char* body, size_t bodyLen, ForceForm& out);

}  // namespace httpparse
