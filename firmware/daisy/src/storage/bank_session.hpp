#pragma once
#include "audio/sample_pool_stage.hpp"
#include "audio/sfz_loader.hpp"
#include "bank_file_job.hpp"

namespace WaveX::Storage {
// Foreground Bank owner. All competing storage/Pool/Track edits are excluded
// while Busy(). FIL windows stay in AXI SRAM; large recall scratch uses the
// sample allocator. Files are immutable: edits always publish named copies.
class BankSession {
   public:
    struct Boundary {
        bool (*stop_track)(uint8_t);
        void (*publish)();
    };
    BankSession(SampleMemMgr&, AudioEngine::SamplePool&, uint8_t*, uint32_t, Boundary);
    ~BankSession();
    BankSession(const BankSession&) = delete;
    BankSession& operator=(const BankSession&) = delete;
    bool Request(const Protocol::BankOpMessage&, bool external_busy = false);
    void Pump();
    bool Busy() const { return status_.busy != 0; }
    bool ReplyPending() const { return reply_; }
    void ReplySent() { reply_ = false; }
    const Protocol::BankStatusMessage& Status() const { return status_; }

   private:
    struct Candidate {
        Wxi::InstrumentFile document;
        AudioEngine::Tracks tracks;
        AudioEngine::SamplePool::Record records[AudioEngine::SamplePool::kCapacity];
        AudioEngine::SamplePool pool{records};
    };
    enum class Phase { Idle, Begin, File, Snapshot, Index, Stage, Load, Commit };
    void Finish(uint8_t);
    void RefreshSlot();
    void Cleanup();
    uint8_t FileError() const;
    uint8_t InstrumentError(uint8_t) const;
    SampleMemMgr& memory_;
    AudioEngine::SamplePool& pool_;
    uint8_t* io_;
    uint32_t io_bytes_;
    Boundary boundary_;
    BankFileJob file_;
    BankFile::Index index_{};
    Candidate* candidate_ = nullptr;
    wxsamp_t candidate_mem_{};
    std::optional<AudioEngine::SamplePoolStage> stage_;
    Protocol::BankOpMessage request_;
    Protocol::BankStatusMessage status_;
    Phase phase_ = Phase::Idle;
    bool reply_ = false;
};
}  // namespace WaveX::Storage
