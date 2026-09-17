#pragma once
#include "audio/mixer_control_handoff.hpp"
#include "audio/sample_pool_stage.hpp"
#include "audio/sfz_loader.hpp"
#include "project_file_job.hpp"
#include "project_patterns.hpp"
#include "sequencer/pattern_exchange.hpp"
#include "wxcf/bank_file.hpp"

namespace WaveX::Storage {
// Single foreground session/file owner. All session mutations and competing SD
// jobs must be gated while Busy(). Keep this FIL-bearing object in AXI SRAM.
// The allocator owns both the retained Project and private transaction scratch.
class ProjectSession {
   public:
    struct Boundary {
        bool (*stop_voices)();  // called only after callback acknowledges transport pause
        void (*publish)();      // republish live bindings after commit or rollback
    };
    ProjectSession(SampleMemMgr&,
                   AudioEngine::SamplePool&,
                   Sequencer::PatternExchange&,
                   AudioEngine::MixerControlHandoff&,
                   uint8_t* io,
                   uint32_t bytes,
                   Boundary);
    ~ProjectSession();  // caller must drain any callback-owned exchange first
    bool Request(const Protocol::ProjectOpMessage&, bool external_busy = false);
    bool RequestPattern(const Protocol::SeqSlotOpMessage&, bool external_busy = false);
    ProjectPatterns& Patterns() { return patterns_; }
    void Pump();
    bool Busy() const { return status_.busy || patterns_.Busy(); }
    const Protocol::ProjectStatusMessage& Status() const { return status_; }
    bool ReplyPending() const { return reply_; }
    void ReplySent() { reply_ = false; }
    const Sequencer::Project* Current() const { return current_; }

   private:
    enum class Phase {
        Idle,
        Allocate,
        Clone,
        Capture,
        Assets,
        Snapshot,
        Save,
        Pause,
        Read,
        Stage,
        Bank,
        LoadTrack,
        SampleEdits,
        Commit,
        Install,
        Cleanup
    };
    struct Candidate {
        BankFile::Index bank;
        AudioEngine::Tracks tracks;
        AudioEngine::SamplePool::Record records[AudioEngine::SamplePool::kCapacity];
        AudioEngine::SamplePool pool{records};
    };
    static bool ReadBank(void*, void*, size_t);
    static bool BankEof(void*);
    void Finish(uint8_t error);
    void Complete();
    void Promote();
    void ReleaseScratch();
    void CaptureSession();
    void SnapshotPath(uint8_t track, char* out, size_t size) const;
    bool Allocate(uint32_t bytes, wxsamp_t&, void**);
    uint8_t FileError() const;
    uint8_t InstrumentError(uint8_t error) const;
    SampleMemMgr& memory_;
    AudioEngine::SamplePool& pool_;
    Sequencer::PatternExchange& exchange_;
    AudioEngine::MixerControlHandoff& mixer_;
    uint8_t* io_;
    uint32_t io_bytes_;
    Boundary boundary_;
    ProjectPatterns patterns_;
    ProjectFileJob file_;
    FIL bank_file_{};
    bool bank_open_ = false;
    std::optional<BankFile::IndexDecoder> bank_decoder_;
    Sequencer::Project* current_ = nullptr;
    Sequencer::Project* scratch_ = nullptr;
    Candidate* candidate_ = nullptr;
    wxsamp_t current_mem_{}, scratch_mem_{}, candidate_mem_{};
    std::optional<AudioEngine::SamplePoolStage> pool_stage_;
    Protocol::ProjectOpMessage request_;
    Protocol::ProjectStatusMessage status_;
    Phase phase_ = Phase::Idle;
    uint32_t clone_offset_ = 0;
    uint16_t sample_ = 0, owned_snapshots_ = 0;
    uint8_t track_ = 0, result_ = Protocol::PROJECT_OK;
    bool reply_ = false, owned_directory_ = false, paused_ = false, track_started_ = false;
    char directory_[96]{};
};
}  // namespace WaveX::Storage
