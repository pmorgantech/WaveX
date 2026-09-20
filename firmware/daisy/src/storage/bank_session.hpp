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
        bool (*stop_tracks)(uint16_t);
        void (*publish)();
    };
    BankSession(SampleMemMgr&, AudioEngine::SamplePool&, uint8_t*, uint32_t, Boundary);
    ~BankSession();
    BankSession(const BankSession&) = delete;
    BankSession& operator=(const BankSession&) = delete;
    bool Request(const Protocol::BankOpMessage&, bool external_busy = false);
    bool RequestSlotOperation(const Protocol::BankSlotOpMessage&, bool external_busy = false);
    // Foreground only. No queue: busy or unmatched events are ignored.
    bool ProgramChange(const Protocol::MidiProgramMessage&, bool external_busy = false);
    void Pump();
    bool Busy() const { return status_.busy != 0; }
    bool ReplyPending() const { return reply_; }
    void ReplySent() { reply_ = false; }
    const Protocol::BankStatusMessage& Status() const { return status_; }

   private:
    friend class ProjectSession;
    // Project transactions hold the foreground storage lease. Capture uses the
    // file identity, not the embedded display name. Restore cannot perform I/O
    // or fail: call only with a validated index/name at the Project commit.
    void CaptureProjectPath(char (&path)[Protocol::BROWSE_PATH_MAX]) const;
    static bool ProjectBankName(const char* path, char (&name)[24]);
    void RestoreProject(const char* name, const BankFile::Index& index);
    struct Candidate {
        Wxi::InstrumentFile document;
        AudioEngine::Tracks tracks;
        AudioEngine::SamplePool::Record records[AudioEngine::SamplePool::kCapacity];
        AudioEngine::SamplePool pool{records};
    };
    enum class Phase { Idle, Begin, File, Snapshot, Index, Stage, Load, Commit, PreloadNext };
    bool BeginRequest(const Protocol::BankOpMessage&,
                      bool external_busy,
                      uint16_t targets,
                      int source_slot = -1);
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
    uint16_t preload_slot_ = 0, recall_targets_ = 0;
    uint32_t program_id_ = 0;
    int source_slot_ = -1;
    Phase phase_ = Phase::Idle;
    bool reply_ = false;
};
}  // namespace WaveX::Storage
