#include "CrashReport.h"

#include <Arduino.h>
#include <drivers/ResetReason.h>
#include <platform/mbed_error.h>

#include <stm32h7xx_hal_rtc_ex.h>

#include "Log.h"

// Определен в варианте OPTA ядра (variant.cpp, Instance = RTC в initVariant())
extern RTC_HandleTypeDef RTCHandle;

static const uint32_t kCrashMagic = 0x45525230UL;  // "ERR0"
static const uint32_t kRegMagic = RTC_BKP_DR24;
static const uint32_t kRegStatus = RTC_BKP_DR25;
static const uint32_t kRegAddress = RTC_BKP_DR26;
static const uint32_t kRegValue = RTC_BKP_DR27;
static const uint32_t kRegStackSize = RTC_BKP_DR28;
static const uint32_t kRegStackFree = RTC_BKP_DR29;

// Вызывается ядром из mbed_error() при фатальной ошибке, в том числе из обработчика
// HardFault: только запись регистров и сброс, без Serial и памяти.
void mbed_error_hook(const mbed_error_ctx* error) {
    HAL_PWR_EnableBkUpAccess();
    HAL_RTCEx_BKUPWrite(&RTCHandle, kRegStatus, static_cast<uint32_t>(error->error_status));
    HAL_RTCEx_BKUPWrite(&RTCHandle, kRegAddress, error->error_address);
    HAL_RTCEx_BKUPWrite(&RTCHandle, kRegValue, error->error_value);
    HAL_RTCEx_BKUPWrite(&RTCHandle, kRegStackSize, error->thread_stack_size);
    // Стек растет вниз от thread_stack_mem + size: отрицательный запас — переполнение
    HAL_RTCEx_BKUPWrite(&RTCHandle, kRegStackFree,
        error->thread_current_sp - error->thread_stack_mem);
    HAL_RTCEx_BKUPWrite(&RTCHandle, kRegMagic, kCrashMagic);
    NVIC_SystemReset();
}

static const char* resetReasonText(reset_reason_t reason) {
    switch (reason) {
        case RESET_REASON_POWER_ON: return "включение питания";
        case RESET_REASON_PIN_RESET: return "кнопка RESET или загрузка прошивки";
        case RESET_REASON_BROWN_OUT: return "просадка питания";
        case RESET_REASON_SOFTWARE: return "программная перезагрузка";
        case RESET_REASON_WATCHDOG: return "watchdog: основной цикл завис";
        case RESET_REASON_LOCKUP: return "блокировка ядра процессора";
        case RESET_REASON_MULTIPLE: return "несколько причин сразу";
        default: break;
    }
    return "не определена";
}

static const char* errorCodeText(int code) {
    switch (code) {
        case MBED_ERROR_CODE_HARDFAULT_EXCEPTION: return "HardFault";
        case MBED_ERROR_CODE_MEMMANAGE_EXCEPTION: return "MemManage";
        case MBED_ERROR_CODE_BUSFAULT_EXCEPTION: return "BusFault";
        case MBED_ERROR_CODE_USAGEFAULT_EXCEPTION: return "UsageFault";
        case MBED_ERROR_CODE_OUT_OF_MEMORY: return "нехватка памяти";
        case MBED_ERROR_CODE_STACK_OVERFLOW: return "переполнение стека";
        case MBED_ERROR_CODE_ASSERTION_FAILED: return "assert";
        case MBED_ERROR_CODE_MUTEX_LOCK_FAILED: return "ошибка мьютекса";
        default: break;
    }
    return "другая ошибка";
}

// Поток узнается по размеру стека: основной — MBED_CONF_RTOS_MAIN_THREAD_STACK_SIZE,
// FetchWorker и WifiConnector — их kStackSize, служебные потоки ядра (в т. ч. чтение
// сокетов TLS-клиента) — MBED_CONF_RTOS_THREAD_STACK_SIZE.
static const char* threadText(uint32_t stackSize) {
    switch (stackSize) {
        case 32768: return "основной (loop)";
        case 16384: return "сетевых запросов (FetchWorker)";
        case 8192: return "подключения Wi-Fi (WifiConnector)";
        case 4096: return "служебный ядра (чтение сокета и др.)";
        default: break;
    }
    return "неизвестный";
}

void printBootReport() {
    const reset_reason_t reason = mbed::ResetReason::get();
    logLine("BOOT", "Причина запуска: %s (RCC RSR 0x%08lX).", resetReasonText(reason),
        static_cast<unsigned long>(mbed::ResetReason::get_raw()));

    if (HAL_RTCEx_BKUPRead(&RTCHandle, kRegMagic) != kCrashMagic) return;
    const mbed_error_status_t status =
        static_cast<mbed_error_status_t>(HAL_RTCEx_BKUPRead(&RTCHandle, kRegStatus));
    const uint32_t stackSize = HAL_RTCEx_BKUPRead(&RTCHandle, kRegStackSize);
    const int32_t stackFree = static_cast<int32_t>(HAL_RTCEx_BKUPRead(&RTCHandle, kRegStackFree));
    logLine("BOOT", "Прошлый запуск остановлен фатальной ошибкой: %s (0x%08lX), адрес 0x%08lX, значение 0x%08lX.",
        errorCodeText(MBED_GET_ERROR_CODE(status)), static_cast<unsigned long>(status),
        static_cast<unsigned long>(HAL_RTCEx_BKUPRead(&RTCHandle, kRegAddress)),
        static_cast<unsigned long>(HAL_RTCEx_BKUPRead(&RTCHandle, kRegValue)));
    logLine("BOOT", "Поток %s: стек %lu байт, запас в момент ошибки %ld байт.",
        threadText(stackSize), static_cast<unsigned long>(stackSize),
        static_cast<long>(stackFree));

    HAL_PWR_EnableBkUpAccess();
    HAL_RTCEx_BKUPWrite(&RTCHandle, kRegMagic, 0);
}
