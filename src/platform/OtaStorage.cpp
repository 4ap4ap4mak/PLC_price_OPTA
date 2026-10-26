#include "OtaStorage.h"

#include <BlockDevice.h>
#include <stdio.h>

#include <stm32h7xx_hal_rtc_ex.h>

// Определен в варианте OPTA ядра (variant.cpp, Instance = RTC в initVariant())
extern RTC_HandleTypeDef RTCHandle;

static const char kUpdatePath[] = "/fs/UPDATE.BIN";
// Область данных загрузчика: байт 1 — его версия
static const uint32_t kBootloaderDataAddress = 0x08000000UL + 0x1F000UL;
// Признак «есть обновление» и тип хранилища: QSPI | FAT | MBR (как QSPI_FLASH_FATFS_MBR
// в Arduino_Portenta_OTA)
static const uint32_t kUpdateMagic = 0x07AA;
static const uint32_t kStorageQspiFatMbr = (1U << 2) | (1U << 5) | (1U << 7);

uint8_t OtaStorage::bootloaderVersion() {
    return reinterpret_cast<const volatile uint8_t*>(kBootloaderDataAddress)[1];
}

bool OtaStorage::begin(Print& log) {
    if (file_ != nullptr || mounted_) abort();
    if (partition_ == nullptr) {
        mbed::BlockDevice* root = mbed::BlockDevice::get_default_instance();
        if (root == nullptr) {
            log.println("[FIRMWARE] QSPI-flash недоступна.");
            return false;
        }
        partition_ = new mbed::MBRBlockDevice(root, kPartition);
    }
    int result = fs_.mount(partition_);
    if (result != 0) {
        // Раздел OTA предназначен только для образа обновления: форматирование ничего
        // не теряет. Если нет самой разметки MBR, reformat тоже не пройдет.
        log.print("[FIRMWARE] Раздел OTA не смонтирован (");
        log.print(result);
        log.println("), форматирование FAT...");
        result = fs_.reformat(partition_);
        if (result != 0) {
            log.print("[FIRMWARE] Раздел OTA на QSPI недоступен (");
            log.print(result);
            log.println("): нужна разметка QSPI, см. docs/OPERATIONS.md.");
            return false;
        }
    }
    mounted_ = true;
    remove(kUpdatePath);
    file_ = fopen(kUpdatePath, "wb");
    if (file_ == nullptr) {
        log.println("[FIRMWARE] Не удалось создать файл UPDATE.BIN на QSPI.");
        unmount();
        return false;
    }
    return true;
}

bool OtaStorage::write(const uint8_t* data, size_t length) {
    return file_ != nullptr && fwrite(data, 1, length, file_) == length;
}

bool OtaStorage::finish() {
    if (file_ == nullptr) return false;
    const bool ok = fclose(file_) == 0;
    file_ = nullptr;
    if (!ok) remove(kUpdatePath);
    unmount();
    return ok;
}

bool OtaStorage::beginRead(Print& log) {
    if (file_ != nullptr || mounted_) abort();
    if (partition_ == nullptr || fs_.mount(partition_) != 0) {
        log.println("[FIRMWARE] Раздел OTA не смонтирован для чтения.");
        return false;
    }
    mounted_ = true;
    file_ = fopen(kUpdatePath, "rb");
    if (file_ == nullptr) {
        log.println("[FIRMWARE] Не удалось открыть UPDATE.BIN для чтения.");
        unmount();
        return false;
    }
    return true;
}

int OtaStorage::read(uint8_t* data, size_t length) {
    if (file_ == nullptr) return -1;
    const size_t count = fread(data, 1, length, file_);
    if (count == 0 && ferror(file_)) return -1;
    return static_cast<int>(count);
}

void OtaStorage::abort() {
    if (file_ != nullptr) {
        fclose(file_);
        file_ = nullptr;
    }
    if (mounted_) remove(kUpdatePath);
    unmount();
}

void OtaStorage::unmount() {
    if (mounted_) {
        fs_.unmount();
        mounted_ = false;
    }
}

void OtaStorage::armBootloader(uint32_t length) {
    HAL_PWR_EnableBkUpAccess();
    HAL_RTCEx_BKUPWrite(&RTCHandle, RTC_BKP_DR0, kUpdateMagic);
    HAL_RTCEx_BKUPWrite(&RTCHandle, RTC_BKP_DR1, kStorageQspiFatMbr);
    HAL_RTCEx_BKUPWrite(&RTCHandle, RTC_BKP_DR2, kPartition);
    HAL_RTCEx_BKUPWrite(&RTCHandle, RTC_BKP_DR3, length);
}
