// Образ новой прошивки на QSPI и флаг загрузчику Arduino (D-038).
//
// Протокол загрузчика Opta (версия 22 и новее; так же работают Arduino_Portenta_OTA и
// Arduino Cloud): файл UPDATE.BIN в FAT на разделе 2 MBR внешней QSPI-flash (раздел OTA
// 5 МБ из разметки QSPIFormat), в регистрах RTC backup — признак 0x07AA, тип хранилища,
// номер раздела и длина. После перезагрузки загрузчик переписывает приложение из файла
// и запускает его. Раздел 1 (прошивка модуля Wi-Fi и сертификаты) не трогается.
//
// begin/write/finish/abort — только рабочий поток FetchWorker, armBootloader — основной
// поток перед перезагрузкой. Watchdog здесь не сбрасывается (D-026).
#pragma once

#include <Arduino.h>
#include <FATFileSystem.h>
#include <MBRBlockDevice.h>

class OtaStorage {
public:
    // Версия загрузчика из его области данных; 0 — не прочитана.
    static uint8_t bootloaderVersion();
    // Загрузчик умеет ставить прошивку из QSPI.
    static bool bootloaderSupportsOta() { return bootloaderVersion() >= kMinBootloaderVersion; }

    // Смонтировать раздел OTA (если в нем нет FAT — отформатировать), удалить старый
    // UPDATE.BIN и открыть новый. false — причина в log.
    bool begin(Print& log);
    bool write(const uint8_t* data, size_t length);
    // Закрыть файл и размонтировать раздел. false — запись не завершилась.
    bool finish();
    // Прервать: закрыть и удалить файл, размонтировать.
    void abort();

    // Записать флаг загрузчику: при следующем запуске поставить UPDATE.BIN длиной length.
    static void armBootloader(uint32_t length);

private:
    static constexpr uint8_t kMinBootloaderVersion = 22;
    static constexpr int kPartition = 2;

    void unmount();

    mbed::MBRBlockDevice* partition_ = nullptr;
    mbed::FATFileSystem fs_{"fs"};
    bool mounted_ = false;
    FILE* file_ = nullptr;
};
