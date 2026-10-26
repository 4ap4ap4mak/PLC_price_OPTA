// Обновление прошивки с GitHub Releases (D-038): проверка версии по кнопке на странице
// (или команде консоли) и раз в сутки, установка — только по кнопке.
//
// Проверка и загрузка идут в потоке FetchWorker; здесь — основной поток: расписание,
// результаты, запреты и перезагрузка. Установка запрещена, пока на любом реле идет
// принудительное включение или включен ручной режим: перезагрузка сбросила бы их
// (D-038, выбор пользователя). Запрет проверяется и при запуске, и после загрузки —
// если за время загрузки включили одно из них, установка отменяется.
//
// Загруженный и проверенный образ (длина, таблица векторов, SHA-256) лежит на QSPI;
// флаг загрузчику ставится непосредственно перед перезагрузкой. Загрузчик переписывает
// приложение (~10–30 с, реле выключены) и запускает новую версию; настройки во flash
// не трогаются. Отката нет: неудачную прошивку восстанавливают по USB.
#pragma once

#include <Arduino.h>

#include <Controller.h>
#include <ControllerPorts.h>
#include <FwUpdate.h>

#include "../net/FetchWorker.h"

class FirmwareUpdater {
public:
    enum class State : uint8_t {
        Idle,           // еще не проверяли
        Checking,
        UpToDate,       // установлена последняя версия (или новее)
        Available,      // есть новая версия
        CheckFailed,
        Downloading,
        DownloadFailed, // новая версия известна, установка не удалась
        Installing      // образ готов, перезагрузка
    };

    // Ответ на запрос пользователя.
    enum class Request : uint8_t {
        Started,
        Busy,           // уже идет проверка или загрузка
        NoNetwork,
        NothingNew,     // нет версии новее установленной
        Blocked,        // принудительное включение или ручной режим
        Bootloader      // загрузчик не умеет ставить из QSPI
    };

    FirmwareUpdater(FetchWorker& worker, const controller::Controller& channels,
        const controller::Clock& clock)
        : worker_(worker), channels_(channels), clock_(clock) {}

    // Каждый loop(): автоматическая проверка, результаты потока, установка.
    void service(uint32_t nowMs, bool online);

    // Запросы пользователя (страница, консоль); ответ запоминается для страницы.
    Request requestCheck(bool online);
    Request requestInstall(bool online);
    // Диагностика KI-029 (консоль firmware test qspi|net): итог — строкой [FIRMWARE].
    // Состояние обновления не меняется, флаг загрузчику не ставится.
    Request requestTest(FetchWorker::FirmwareTest test, bool online);
    // Ответ на последний запрос пользователя; Started — показывать нечего.
    Request lastRequest() const { return lastRequest_; }

    State state() const { return state_; }
    // Последний релиз, если проверка хоть раз удалась.
    const fwupdate::Manifest* latest() const { return hasLatest_ ? &latest_ : nullptr; }
    fwupdate::Failure failure() const { return failure_; }
    // Время последней удачной или неудачной проверки (UTC), 0 — неизвестно.
    uint32_t checkedAtUtc() const { return checkedAtUtc_; }
    uint32_t downloadedBytes() { return worker_.firmwareBytes(); }
    // Почему установка сейчас запрещена; nullptr — разрешена.
    const char* blockReason() const;
    // Последняя установка отменена: за время загрузки включили запрет.
    bool installCancelled() const { return installCancelled_; }

    static const char* stateName(State state);
    static const char* requestText(Request request);

private:
    bool busy() const;
    bool newerAvailable() const;
    Request startCheck(bool online);
    Request startInstall(bool online);
    void finishCheck(uint32_t nowMs, bool ok);
    void install();

    FetchWorker& worker_;
    const controller::Controller& channels_;
    const controller::Clock& clock_;
    fwupdate::CheckSchedule schedule_;
    State state_ = State::Idle;
    fwupdate::Manifest latest_ = {};
    bool hasLatest_ = false;
    fwupdate::Failure failure_ = fwupdate::Failure::None;
    uint32_t checkedAtUtc_ = 0;
    bool installCancelled_ = false;
    Request lastRequest_ = Request::Started;
    bool testing_ = false;
    // Диагностика KI-029: последняя строка «основной цикл жив» во время задания
    uint32_t aliveLoggedMs_ = 0;
};
