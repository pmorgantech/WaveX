#pragma once
#include "recorder.hpp"
#include "sample_pool.hpp"
#include "snapshot_mailbox.hpp"
#include "storage/recording_save.hpp"
#include "voice_manager.hpp"

namespace WaveX::AudioEngine {
class RecordingSession {
   public:
    void Init(SamplePool* pool,
              SampleMemMgr* memory,
              void (*publish)(const LoadedSampleInfo&),
              int (*send)(uint16_t, const void*, uint16_t));
    void Request(const Protocol::RecordOpMessage&, bool storage_busy);
    void Pump(uint32_t now);
    void Commands(VoiceManager& voices);
    void Monitor(
        const float* left, const float* right, float* out_l, float* out_r, uint32_t frames);
    void Process(
        const float* left, const float* right, float* out_l, float* out_r, uint32_t frames);
    bool Busy() const;
    bool Owns(uint16_t sample) const { return sample && status_.sample_id == sample; }

   private:
    uint32_t next_admit_ms_ = 0;
    struct Command {
        uint32_t id = 0;
        uint8_t op = Protocol::REC_GET, source = 0, monitor = 0;
        VoiceTriggerParams preview;
    };
    struct Meter {
        uint16_t left = 0, right = 0;
        uint32_t square_l = 0, square_r = 0, clips = 0;
    };
    void Complete(uint32_t request, uint8_t op, uint8_t error);
    void FreeScratch();
    void Discard();
    bool Prepare(const Protocol::RecordOpMessage&);
    SamplePool* pool_ = nullptr;
    SampleMemMgr* memory_ = nullptr;
    void (*publish_)(const LoadedSampleInfo&) = nullptr;
    int (*send_packet_)(uint16_t, const void*, uint16_t) = nullptr;
    Recording::Capture capture_;
    Storage::RecordingSave save_;
    SnapshotMailbox<Command> commands_;
    SnapshotMailbox<Meter> meters_;
    std::atomic<uint32_t> acknowledged_{0};
    std::atomic<bool> detached_{true};
    Protocol::RecordStatusMessage status_;
    Command pending_;
    wxsamp_t take_{}, history_{}, ring_{};
    int16_t* pcm_ = nullptr;
    bool saved_ = false, send_ = false;
    uint32_t last_send_ = 0;
    // Callback ownership only.
    uint64_t preview_group_ = 0;
    Meter meter_;
    uint8_t source_ = 0, monitor_ = 0;
    bool capturing_ = false;
    uint8_t command_error_ = Protocol::REC_OK;
};
}  // namespace WaveX::AudioEngine
