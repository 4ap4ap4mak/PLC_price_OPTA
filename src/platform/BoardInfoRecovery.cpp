#include "BoardInfoRecovery.h"

#include <Arduino.h>

#include <stm32h7xx_hal_rtc_ex.h>

// Вариант OPTA ядра (variant.cpp): данные платы — из OTP или, если OTP не прочитан,
// из внутренней flash по 0x0801F000
extern uint8_t* boardInfo();
// Определен в варианте OPTA ядра (variant.cpp, Instance = RTC в initVariant())
extern RTC_HandleTypeDef RTCHandle;

static const uint8_t* const kFallbackBoardInfo = reinterpret_cast<const uint8_t*>(0x0801F000UL);
// DR0…DR3 — флаг обновления загрузчику (OtaStorage); DR31 — свободен
static const uint32_t kRebootFlagRegister = RTC_BKP_DR31;
static const uint32_t kRebootFlag = 0x4F545021UL;
// Самая долгая операция микросхемы, которую делает прошивка (стирание блока 64 КБ), —
// до ~1,3 с; загрузчик после сброса добавляет свое время
static const uint32_t kSettleMs = 1500;

BoardInfoRecovery recoverBoardInfo() {
    HAL_PWR_EnableBkUpAccess();
    const bool rebootedForRecovery =
        HAL_RTCEx_BKUPRead(&RTCHandle, kRebootFlagRegister) == kRebootFlag;
    if (rebootedForRecovery) HAL_RTCEx_BKUPWrite(&RTCHandle, kRebootFlagRegister, 0);

    // Только сравнение указателя: сама прошивка к QSPI здесь не обращается
    if (boardInfo() != kFallbackBoardInfo) {
        return rebootedForRecovery ? BoardInfoRecovery::AfterReboot : BoardInfoRecovery::NotNeeded;
    }
    if (rebootedForRecovery) return BoardInfoRecovery::Failed;

    // Реле еще не настроены (setup() не дошел до выходов): перезагрузка безопасна
    HAL_RTCEx_BKUPWrite(&RTCHandle, kRebootFlagRegister, kRebootFlag);
    delay(kSettleMs);
    NVIC_SystemReset();
    return BoardInfoRecovery::Failed;
}
