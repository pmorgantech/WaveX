#include "card_service.hpp"

#include "audio/audio_engine.h"
#include "comm/daisy_filesystem.h"
#include "comm/mcu_link.h"
#include "sd_sdio.h"
#include "sys/system.h"

#include "card_format_job.hpp"
namespace WaveX::Storage::CardService {
namespace {
CardFormatJob job;
}
bool Busy() {
    return job.Busy();
}
void Request(const Protocol::CardOpMessage& request) {
#if WAVEX_DAISY_SD_CARD_ENABLED && WAVEX_DAISY_SD_CARD_BACKEND == 1
    job.Handle(request,
               daisy::System::GetNow(),
               SdSdio::MediaGeneration(),
               SdSdio::CanFormat(),
               AudioEngine::StorageJobBusy(),
               SdSdio::IsMounted());
#else
    job.Handle(request, daisy::System::GetNow(), 0, false, false, false);
#endif
}
void Pump() {
    if (job.Dirty() &&
        Comm::LinkSend(Protocol::MSG_CARD_STATE, &job.State(), sizeof(job.State())) >= 0)
        job.Sent();
#if WAVEX_DAISY_SD_CARD_ENABLED && WAVEX_DAISY_SD_CARD_BACKEND == 1
    if (!job.TakeRun(daisy::System::GetNow(), SdSdio::MediaGeneration()))
        return;
    if (AudioEngine::StorageJobBusy()) {
        job.Complete(Protocol::CARD_BUSY, SdSdio::IsMounted());
        return;
    }
    if (!AudioEngine::PrepareCardFormat()) {
        job.Complete(Protocol::CARD_AUDIO_BUSY, SdSdio::IsMounted());
        return;
    }
    Comm::NotifyStorageLost();
    const bool formatted = SdSdio::FormatCard();
    AudioEngine::FinishCardFormat();
    if (SdSdio::IsMounted())
        Comm::NotifyStorageAvailable();
    job.Complete(formatted ? Protocol::CARD_OK : Protocol::CARD_IO, SdSdio::IsMounted());
#endif
}
}  // namespace WaveX::Storage::CardService
