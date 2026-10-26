#include "FirmwareUpdater.h"

#include <ChannelConfig.h>

#include "../FirmwareVersion.h"
#include "../platform/Log.h"
#include "../platform/OtaStorage.h"

using channelconfig::kChannelCount;
using controller::FetchStatus;

const char* FirmwareUpdater::blockReason() const {
    if (channels_.anyForceOnActive()) {
        return "идет принудительное включение — дождитесь конца или остановите его";
    }
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        if (channels_.manual(i)) return "включен ручной режим — верните реле автоматике";
    }
    return nullptr;
}

bool FirmwareUpdater::newerAvailable() const {
    fwupdate::Version current = {};
    return hasLatest_ && fwupdate::parseVersion(kFirmwareVersion, current) &&
        fwupdate::compareVersions(latest_.version, current) > 0;
}

bool FirmwareUpdater::busy() const {
    return testing_ || state_ == State::Checking || state_ == State::Downloading ||
        state_ == State::Installing;
}

FirmwareUpdater::Request FirmwareUpdater::requestTest(FetchWorker::FirmwareTest test,
    bool online) {
    if (busy()) return Request::Busy;
    if (test == FetchWorker::FirmwareTest::Network && !online) return Request::NoNetwork;
    if (!worker_.startFirmwareTest(test)) return Request::Busy;
    logLine("FIRMWARE", "Диагностика: %s; реле работают как обычно.",
        test == FetchWorker::FirmwareTest::Storage
            ? "запись 551020 байт на QSPI без сети, чтение обратно"
            : "загрузка последнего релиза без записи на QSPI");
    testing_ = true;
    return Request::Started;
}

FirmwareUpdater::Request FirmwareUpdater::requestCheck(bool online) {
    lastRequest_ = startCheck(online);
    return lastRequest_;
}

FirmwareUpdater::Request FirmwareUpdater::requestInstall(bool online) {
    lastRequest_ = startInstall(online);
    return lastRequest_;
}

FirmwareUpdater::Request FirmwareUpdater::startCheck(bool online) {
    if (busy()) return Request::Busy;
    if (!online) return Request::NoNetwork;
    if (!worker_.startFirmwareCheck()) return Request::Busy;
    logLine("FIRMWARE", "Проверка новой версии на GitHub (установлена %s).", kFirmwareVersion);
    state_ = State::Checking;
    return Request::Started;
}

FirmwareUpdater::Request FirmwareUpdater::startInstall(bool online) {
    if (busy()) return Request::Busy;
    if (!newerAvailable()) return Request::NothingNew;
    if (blockReason() != nullptr) return Request::Blocked;
    if (!OtaStorage::bootloaderSupportsOta()) return Request::Bootloader;
    if (!online) return Request::NoNetwork;
    if (!worker_.startFirmwareDownload(latest_)) return Request::Busy;
    logLine("FIRMWARE", "Загрузка версии %s (%lu байт); реле работают как обычно.",
        latest_.versionText, static_cast<unsigned long>(latest_.size));
    installCancelled_ = false;
    state_ = State::Downloading;
    return Request::Started;
}

void FirmwareUpdater::finishCheck(uint32_t nowMs, bool ok) {
    schedule_.markChecked(nowMs, ok);
    checkedAtUtc_ = clock_.synced() ? clock_.nowUtc() : 0;
}

void FirmwareUpdater::service(uint32_t nowMs, bool online) {
    if (!busy() && schedule_.due(nowMs, online)) {
        // Автоматическая проверка только сообщает о версии, не ставит ее
        if (startCheck(online) != Request::Started) schedule_.markChecked(nowMs, false);
    }

    // Диагностика KI-029: если устройство замрет, последняя такая строка покажет, жив ли
    // был основной цикл, когда остановился рабочий поток
    if (busy() && state_ != State::Checking && nowMs - aliveLoggedMs_ >= 1000UL) {
        aliveLoggedMs_ = nowMs;
        logLine("FWDBG", "Основной цикл жив: %lu мс, байт %lu.",
            static_cast<unsigned long>(nowMs),
            static_cast<unsigned long>(worker_.firmwareBytes()));
    }

    fwupdate::Manifest manifest = {};
    fwupdate::Failure failure = fwupdate::Failure::None;
    const FetchStatus status = worker_.pollFirmware(manifest, failure);
    if (status == FetchStatus::Idle || status == FetchStatus::Busy) return;
    // Новый итог заменяет отказ на прошлый запрос пользователя
    lastRequest_ = Request::Started;

    if (testing_) {
        testing_ = false;
        if (status == FetchStatus::Done) {
            logLine("FIRMWARE", "Диагностика пройдена.");
        } else {
            logLine("FIRMWARE", "Диагностика не пройдена: %s.", fwupdate::failureText(failure));
        }
        return;
    }

    if (state_ == State::Checking) {
        finishCheck(nowMs, status == FetchStatus::Done);
        if (status == FetchStatus::Failed) {
            failure_ = failure;
            state_ = State::CheckFailed;
            logLine("FIRMWARE", "Проверка не удалась: %s.", fwupdate::failureText(failure));
            return;
        }
        latest_ = manifest;
        hasLatest_ = true;
        failure_ = fwupdate::Failure::None;
        state_ = newerAvailable() ? State::Available : State::UpToDate;
        if (state_ == State::Available) {
            logLine("FIRMWARE", "Доступна версия %s (установлена %s); установка — кнопкой на странице или командой firmware install.",
                latest_.versionText, kFirmwareVersion);
        } else {
            logLine("FIRMWARE", "Установлена последняя версия %s.", kFirmwareVersion);
        }
        return;
    }

    if (state_ == State::Downloading) {
        if (status == FetchStatus::Failed) {
            failure_ = failure;
            state_ = State::DownloadFailed;
            logLine("FIRMWARE", "Загрузка не удалась: %s; установленная версия %s работает дальше.",
                fwupdate::failureText(failure), kFirmwareVersion);
            return;
        }
        // Запрет мог появиться за время загрузки: тогда образ остается на QSPI без флага,
        // загрузчик его не тронет
        const char* reason = blockReason();
        if (reason != nullptr) {
            installCancelled_ = true;
            state_ = State::Available;
            logLine("FIRMWARE", "Установка отменена: %s.", reason);
            return;
        }
        install();
    }
}

void FirmwareUpdater::install() {
    state_ = State::Installing;
    logLine("FIRMWARE", "Установка версии %s: перезагрузка, загрузчик перепишет прошивку (до 30 с, реле выключены).",
        latest_.versionText);
    OtaStorage::armBootloader(latest_.size);
    Serial.flush();
    delay(500);
    NVIC_SystemReset();
}

const char* FirmwareUpdater::stateName(State state) {
    switch (state) {
        case State::Idle: return "не проверялось";
        case State::Checking: return "идет проверка";
        case State::UpToDate: return "установлена последняя версия";
        case State::Available: return "доступна новая версия";
        case State::CheckFailed: return "проверка не удалась";
        case State::Downloading: return "идет загрузка";
        case State::DownloadFailed: return "загрузка не удалась";
        case State::Installing: return "установка, перезагрузка";
    }
    return "?";
}

const char* FirmwareUpdater::requestText(Request request) {
    switch (request) {
        case Request::Started: return "запущено";
        case Request::Busy: return "уже идет проверка или загрузка";
        case Request::NoNetwork: return "нет связи с интернетом";
        case Request::NothingNew: return "нет версии новее установленной — сначала проверьте обновление";
        case Request::Blocked: return "установка запрещена, пока идет принудительное включение или включен ручной режим";
        case Request::Bootloader: return "загрузчик устройства не поддерживает обновление из QSPI";
    }
    return "?";
}
