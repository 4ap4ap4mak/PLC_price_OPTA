// Образ новой прошивки на QSPI и флаг загрузчику Arduino (D-038).
//
// Протокол загрузчика Opta (версия 22 и новее; так же работают Arduino_Portenta_OTA и
// Arduino Cloud): файл UPDATE.BIN в FAT на разделе 2 MBR внешней QSPI-flash (раздел OTA
// 5 МБ из разметки QSPIFormat), в регистрах RTC backup — признак 0x07AA, тип хранилища,
// номер раздела и длина. После перезагрузки загрузчик переписывает приложение из файла
// и запускает его. Раздел 1 (прошивка модуля Wi-Fi и сертификаты) не трогается.
//
// Раздел форматируется перед каждой записью: он только для образа, а оборванная запись
// (перезагрузка, питание) оставила бы потерянные кластеры. Данные уходят в файл блоками
// ровно по сектору (kBlockSize): FatFs пишет их прямо в сектора, а таблицу FAT — один
// раз при закрытии, а не на каждый кластер (KI-029).
//
// begin/write/finish/abort/beginRead/read — только рабочий поток FetchWorker,
// armBootloader — основной поток перед перезагрузкой. Watchdog здесь не сбрасывается
// (D-026).
#pragma once

#include <Arduino.h>
#include <FATFileSystem.h>
#include <MBRBlockDevice.h>
#include <filesystem/File.h>

class OtaStorage {
public:
    // Версия загрузчика (KI-030: блок данных новых загрузчиков или код загрузчика 22);
    // 0 — не найдена.
    static uint8_t bootloaderVersion();
    // Загрузчик умеет ставить прошивку из QSPI.
    static bool bootloaderSupportsOta() { return bootloaderVersion() >= kMinBootloaderVersion; }

    // Отформатировать раздел OTA и открыть новый UPDATE.BIN. false — причина в log.
    bool begin(Print& log);
    // Дописать данные. false — ошибка записи (код — lastError()).
    bool write(const uint8_t* data, size_t length);
    // Код последней ошибки файловой системы (отрицательный), 0 — не было.
    int lastError() const { return lastError_; }
    // Дописать остаток, закрыть файл и размонтировать раздел. false — запись не
    // завершилась.
    bool finish();
    // Прервать запись (файл удаляется) или закрыть чтение (файл остается).
    void abort();

    // Открыть записанный UPDATE.BIN для проверки после finish() — так же, через FAT, его
    // прочитает загрузчик. Закрыть — abort().
    bool beginRead(Print& log);
    // Прочитать до length байт; 0 — конец файла, <0 — ошибка.
    int read(uint8_t* data, size_t length);

    // Записать флаг загрузчику: при следующем запуске поставить UPDATE.BIN длиной length.
    static void armBootloader(uint32_t length);

private:
    static constexpr uint8_t kMinBootloaderVersion = 22;
    static constexpr int kPartition = 2;
    // Сектор FAT раздела OTA (разметка QSPIFormat, сектор стирания QSPI 4 КБ)
    static constexpr size_t kBlockSize = 4096;

    // Записать накопленный буфер в файл.
    bool flushBuffer();
    void unmount();

    mbed::MBRBlockDevice* partition_ = nullptr;
    mbed::FATFileSystem fs_{"fs"};
    bool mounted_ = false;
    mbed::File file_;
    bool fileOpen_ = false;
    bool writing_ = false;
    int lastError_ = 0;
    size_t bufferLen_ = 0;
    uint8_t buffer_[kBlockSize] = {};
};
