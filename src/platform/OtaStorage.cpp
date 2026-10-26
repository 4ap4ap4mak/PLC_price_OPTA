#include "OtaStorage.h"

#include <BlockDevice.h>
#include <fcntl.h>
#include <string.h>

#include <stm32h7xx_hal_rtc_ex.h>

#include <FwUpdate.h>

// Определен в варианте OPTA ядра (variant.cpp, Instance = RTC в initVariant())
extern RTC_HandleTypeDef RTCHandle;

// Путь в разделе: загрузчик ищет /fs/UPDATE.BIN
static const char kUpdateName[] = "UPDATE.BIN";
// Признак «есть обновление» и тип хранилища: QSPI | FAT | MBR (как QSPI_FLASH_FATFS_MBR
// в Arduino_Portenta_OTA)
static const uint32_t kUpdateMagic = 0x07AA;
static const uint32_t kStorageQspiFatMbr = (1U << 2) | (1U << 5) | (1U << 7);

uint8_t OtaStorage::bootloaderVersion() {
    // Загрузчик не меняется, пока работает приложение: область просматривается один раз
    // (KI-030: у загрузчика 22 версия — в коде, а не в блоке данных)
    static const uint8_t version = fwupdate::bootloaderVersion(
        reinterpret_cast<const uint8_t*>(fwupdate::kBootloaderBase), fwupdate::kBootloaderSize,
        fwupdate::kBootloaderBase);
    return version;
}

bool OtaStorage::begin(Print& log) {
    abort();
    if (partition_ == nullptr) {
        mbed::BlockDevice* root = mbed::BlockDevice::get_default_instance();
        if (root == nullptr) {
            log.println("[FIRMWARE] QSPI-flash недоступна.");
            return false;
        }
        partition_ = new mbed::MBRBlockDevice(root, kPartition);
    }
    // Раздел OTA предназначен только для образа обновления: форматирование ничего не
    // теряет. Если нет самой разметки MBR, reformat не пройдет.
    const int result = fs_.reformat(partition_);
    if (result != 0) {
        log.print("[FIRMWARE] Раздел OTA на QSPI недоступен (");
        log.print(result);
        log.println("): нужна разметка QSPI, см. docs/OPERATIONS.md.");
        return false;
    }
    mounted_ = true;
    const int opened = file_.open(&fs_, kUpdateName, O_WRONLY | O_CREAT | O_TRUNC);
    if (opened != 0) {
        log.print("[FIRMWARE] Не удалось создать файл UPDATE.BIN на QSPI (");
        log.print(opened);
        log.println(").");
        unmount();
        return false;
    }
    fileOpen_ = true;
    writing_ = true;
    bufferLen_ = 0;
    lastError_ = 0;
    return true;
}

bool OtaStorage::flushBuffer() {
    if (bufferLen_ == 0) return true;
    const ssize_t written = file_.write(buffer_, bufferLen_);
    if (written != static_cast<ssize_t>(bufferLen_)) {
        lastError_ = written < 0 ? static_cast<int>(written) : -1;
        return false;
    }
    bufferLen_ = 0;
    return true;
}

bool OtaStorage::write(const uint8_t* data, size_t length) {
    if (!writing_) return false;
    while (length > 0) {
        size_t part = kBlockSize - bufferLen_;
        if (part > length) part = length;
        memcpy(buffer_ + bufferLen_, data, part);
        bufferLen_ += part;
        data += part;
        length -= part;
        if (bufferLen_ == kBlockSize && !flushBuffer()) return false;
    }
    return true;
}

bool OtaStorage::finish() {
    if (!writing_) return false;
    writing_ = false;
    bool ok = flushBuffer();
    const int closed = file_.close();
    fileOpen_ = false;
    if (ok && closed != 0) lastError_ = closed;
    ok = ok && closed == 0;
    if (!ok) fs_.remove(kUpdateName);
    unmount();
    return ok;
}

bool OtaStorage::beginRead(Print& log) {
    abort();
    if (partition_ == nullptr || fs_.mount(partition_) != 0) {
        log.println("[FIRMWARE] Раздел OTA не смонтирован для чтения.");
        return false;
    }
    mounted_ = true;
    if (file_.open(&fs_, kUpdateName, O_RDONLY) != 0) {
        log.println("[FIRMWARE] Не удалось открыть UPDATE.BIN для чтения.");
        unmount();
        return false;
    }
    fileOpen_ = true;
    return true;
}

int OtaStorage::read(uint8_t* data, size_t length) {
    if (!fileOpen_) return -1;
    return static_cast<int>(file_.read(data, length));
}

void OtaStorage::abort() {
    if (fileOpen_) {
        file_.close();
        fileOpen_ = false;
    }
    if (writing_) {
        writing_ = false;
        fs_.remove(kUpdateName);
    }
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
