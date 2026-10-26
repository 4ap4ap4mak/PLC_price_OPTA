#include "FetchWorker.h"

#include <Elapsed.h>
#include <string.h>

#include "NtpQuery.h"

using controller::FetchStatus;

// =============================================================================
// ЛОГ РАБОЧЕГО ПОТОКА
// =============================================================================
size_t LineLog::write(uint8_t c) {
    if (c != '\n') {
        if (lineLen_ < kLineCap) {
            line_[lineLen_++] = static_cast<char>(c);
        } else {
            lineTruncated_ = true;
        }
        return 1;
    }

    const char kTruncated[] = "...";
    const size_t extra = lineTruncated_ ? sizeof(kTruncated) - 1 : 0;
    mutex_.lock();
    if (bufferLen_ + lineLen_ + extra + 1 <= kBufferCap) {
        memcpy(buffer_ + bufferLen_, line_, lineLen_);
        bufferLen_ += lineLen_;
        if (extra > 0) {
            memcpy(buffer_ + bufferLen_, kTruncated, extra);
            bufferLen_ += extra;
        }
        buffer_[bufferLen_++] = '\n';
    } else {
        ++droppedLines_;
    }
    mutex_.unlock();
    lineLen_ = 0;
    lineTruncated_ = false;
    return 1;
}

void LineLog::flushTo(Print& out) {
    // Печать вне мьютекса: медленный Serial не должен задерживать рабочий поток
    static char copy[kBufferCap];
    mutex_.lock();
    const size_t length = bufferLen_;
    memcpy(copy, buffer_, length);
    bufferLen_ = 0;
    const uint16_t dropped = droppedLines_;
    droppedLines_ = 0;
    mutex_.unlock();

    if (length > 0) {
        out.write(reinterpret_cast<const uint8_t*>(copy), length);
    }
    if (dropped > 0) {
        out.print("[LOG] Пропущено строк сетевого потока: ");
        out.println(dropped);
    }
}

// =============================================================================
// РАБОЧИЙ ПОТОК
// =============================================================================
FetchWorker::FetchWorker()
    : thread_(osPriorityNormal, kStackSize, stack_, "fetch"),
      elering_(log_),
      firmware_(log_) {}

// Приоритет как у loop(): RTX делит время между ними по очереди (round-robin), и
// TLS-рукопожатие не останавливает веб-страницу и консоль.
bool FetchWorker::begin() {
    started_ = thread_.start(mbed::callback(this, &FetchWorker::run)) == osOK;
    return started_;
}

void FetchWorker::run() {
    for (;;) {
        flags_.wait_any(kJobFlag);
        for (;;) {
            mutex_.lock();
            const bool timeJob = timeState_ == JobState::Queued;
            const bool priceJob = !timeJob && priceState_ == JobState::Queued;
            const bool firmwareJob = !timeJob && !priceJob &&
                firmwareState_ == JobState::Queued;
            if (timeJob) timeState_ = JobState::Running;
            if (priceJob) priceState_ = JobState::Running;
            if (firmwareJob) firmwareState_ = JobState::Running;
            if (timeJob || priceJob || firmwareJob) {
                running_ = true;
                runningSinceMs_ = millis();
            }
            mutex_.unlock();

            if (timeJob) {
                runTimeJob();
            } else if (priceJob) {
                runPriceJob();
            } else if (firmwareJob) {
                runFirmwareJob();
            } else {
                break;
            }
        }
    }
}

void FetchWorker::runTimeJob() {
    mutex_.lock();
    const Uplink uplink = timeUplink_;
    mutex_.unlock();
    uint32_t epoch = 0;
    uint32_t atMs = 0;
    bool ok = false;
    if (!routeThrough(uplink)) {
        log_.println("[NTP] Нет связи с интернетом: синхронизация не выполнена.");
    } else if (uplink == Uplink::Ethernet) {
        ok = queryNtpTime(ethernetNtpUdp_, log_, epoch, atMs);
    } else {
        ok = queryNtpTime(wifiNtpUdp_, log_, epoch, atMs);
    }
    mutex_.lock();
    timeEpoch_ = epoch;
    timeAtMs_ = atMs;
    timeState_ = ok ? JobState::Done : JobState::Failed;
    running_ = false;
    mutex_.unlock();
}

void FetchWorker::runPriceJob() {
    char path[sizeof(pricePath_)];
    mutex_.lock();
    const bool current = priceCurrent_;
    const Uplink uplink = priceUplink_;
    memcpy(path, pricePath_, sizeof(path));
    prices::PriceSeries* out = priceOut_;
    mutex_.unlock();

    float eurKwh = 0.0f;
    long timestamp = 0;
    const bool ok = current
        ? elering_.fetchCurrent(uplink, eurKwh, timestamp)
        : out != nullptr && elering_.fetchPrices(uplink, path, *out);

    mutex_.lock();
    priceEurKwh_ = eurKwh;
    priceTimestamp_ = timestamp;
    priceState_ = ok ? JobState::Done : JobState::Failed;
    running_ = false;
    mutex_.unlock();
}

void FetchWorker::runFirmwareJob() {
    mutex_.lock();
    const bool download = firmwareDownload_;
    const Uplink uplink = firmwareUplink_;
    fwupdate::Manifest manifest = firmwareManifest_;
    mutex_.unlock();

    const fwupdate::Failure failure = download
        ? firmware_.download(uplink, manifest, otaStorage_,
              mbed::callback(this, &FetchWorker::onFirmwareProgress))
        : firmware_.fetchManifest(uplink, manifest);

    mutex_.lock();
    if (!download && failure == fwupdate::Failure::None) firmwareManifest_ = manifest;
    firmwareFailure_ = failure;
    firmwareState_ = failure == fwupdate::Failure::None ? JobState::Done : JobState::Failed;
    running_ = false;
    mutex_.unlock();
}

void FetchWorker::onFirmwareProgress(uint32_t bytes) {
    mutex_.lock();
    firmwareBytes_ = bytes;
    runningSinceMs_ = millis();
    mutex_.unlock();
}

// =============================================================================
// ОСНОВНОЙ ПОТОК
// =============================================================================
FetchStatus FetchWorker::takeResult(JobState& state) {
    switch (state) {
        case JobState::Idle: return FetchStatus::Idle;
        case JobState::Queued:
        case JobState::Running: return FetchStatus::Busy;
        case JobState::Done: state = JobState::Idle; return FetchStatus::Done;
        case JobState::Failed: state = JobState::Idle; return FetchStatus::Failed;
    }
    return FetchStatus::Idle;
}

void FetchWorker::setUplink(Uplink uplink) {
    mutex_.lock();
    uplink_ = uplink;
    mutex_.unlock();
}

bool FetchWorker::startCurrent() {
    mutex_.lock();
    const bool idle = started_ && priceState_ == JobState::Idle;
    if (idle) {
        priceState_ = JobState::Queued;
        priceUplink_ = uplink_;
        priceCurrent_ = true;
        priceOut_ = nullptr;
    }
    mutex_.unlock();
    if (idle) flags_.set(kJobFlag);
    return idle;
}

bool FetchWorker::startPrices(const char* path, prices::PriceSeries& out) {
    const size_t length = strlen(path);
    if (length >= sizeof(pricePath_)) return false;
    mutex_.lock();
    const bool idle = started_ && priceState_ == JobState::Idle;
    if (idle) {
        priceState_ = JobState::Queued;
        priceUplink_ = uplink_;
        priceCurrent_ = false;
        memcpy(pricePath_, path, length + 1);
        priceOut_ = &out;
    }
    mutex_.unlock();
    if (idle) flags_.set(kJobFlag);
    return idle;
}

// Сначала результат, потом лог: поток пишет все строки задания до того, как
// отметит его выполненным, поэтому строки печатаются раньше реакции Controller.
FetchStatus FetchWorker::poll(float& eurKwh, long& timestamp) {
    mutex_.lock();
    const FetchStatus status = takeResult(priceState_);
    if (status == FetchStatus::Done) {
        eurKwh = priceEurKwh_;
        timestamp = priceTimestamp_;
    }
    mutex_.unlock();
    flushLog();
    return status;
}

bool FetchWorker::startTimeSync() {
    mutex_.lock();
    const bool idle = started_ && timeState_ == JobState::Idle;
    if (idle) {
        timeState_ = JobState::Queued;
        timeUplink_ = uplink_;
    }
    mutex_.unlock();
    if (idle) flags_.set(kJobFlag);
    return idle;
}

FetchStatus FetchWorker::pollTimeSync(uint32_t& epoch, uint32_t& atMs) {
    mutex_.lock();
    const FetchStatus status = takeResult(timeState_);
    if (status == FetchStatus::Done) {
        epoch = timeEpoch_;
        atMs = timeAtMs_;
    }
    mutex_.unlock();
    flushLog();
    return status;
}

bool FetchWorker::startFirmwareCheck() {
    mutex_.lock();
    const bool idle = started_ && firmwareState_ == JobState::Idle;
    if (idle) {
        firmwareState_ = JobState::Queued;
        firmwareUplink_ = uplink_;
        firmwareDownload_ = false;
    }
    mutex_.unlock();
    if (idle) flags_.set(kJobFlag);
    return idle;
}

bool FetchWorker::startFirmwareDownload(const fwupdate::Manifest& manifest) {
    mutex_.lock();
    const bool idle = started_ && firmwareState_ == JobState::Idle;
    if (idle) {
        firmwareState_ = JobState::Queued;
        firmwareUplink_ = uplink_;
        firmwareDownload_ = true;
        firmwareManifest_ = manifest;
        firmwareBytes_ = 0;
    }
    mutex_.unlock();
    if (idle) flags_.set(kJobFlag);
    return idle;
}

FetchStatus FetchWorker::pollFirmware(fwupdate::Manifest& manifest,
    fwupdate::Failure& failure) {
    mutex_.lock();
    const FetchStatus status = takeResult(firmwareState_);
    if (status == FetchStatus::Done) manifest = firmwareManifest_;
    if (status == FetchStatus::Failed) failure = firmwareFailure_;
    mutex_.unlock();
    flushLog();
    return status;
}

uint32_t FetchWorker::firmwareBytes() {
    mutex_.lock();
    const uint32_t bytes = firmwareBytes_;
    mutex_.unlock();
    return bytes;
}

bool FetchWorker::stuck(uint32_t nowMs) {
    mutex_.lock();
    // Отметку пишет этот поток, nowMs снят основным раньше: разность может быть
    // «отрицательной» — это не зависание (KI-031)
    const bool result = running_ && elapsed::atLeast(nowMs, runningSinceMs_, kStuckMs);
    mutex_.unlock();
    return result;
}
