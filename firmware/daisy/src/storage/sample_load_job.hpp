#pragma once

#include "ff.h"

#include "audio/sample_pool.hpp"
#include "wxcf/sample_file.hpp"

namespace WaveX::Storage {
// Main-loop owner of a private allocation and read-only file. No Pool identity
// is published until every byte, metadata validation and close have succeeded.
// Keep this object in AXI SRAM (FIL contains a DMA-visible sector window).
class SampleLoadJob {
   public:
    static constexpr uint32_t kReadBytes = 4096;
    bool Begin(const Protocol::SampleLoadMessage& request);
    void Pump(AudioEngine::SamplePool& pool,
              SampleMemMgr& memory,
              uint8_t* aligned_io,
              uint32_t io_bytes);
    void Cancel(SampleMemMgr& memory);
    bool Busy() const { return phase_ != Phase::Idle; }
    uint32_t BytesRead() const { return copied_; }
    const Protocol::SampleStatusMessage& Status() const { return status_; }
    bool ReplyPending() const { return reply_pending_; }
    void ReplySent() { reply_pending_ = false; }
    SampleLoadJob() = default;
    SampleLoadJob(const SampleLoadJob&) = delete;
    SampleLoadJob& operator=(const SampleLoadJob&) = delete;

   private:
    enum class Phase { Idle, Open, Header, Sidecar, Allocate, Read, Commit };
    void Fail(SampleMemMgr& memory, Protocol::SampleLoadFailReason reason);
    void Complete(AudioEngine::SamplePool& pool, AudioEngine::SamplePool::Record& record);
    FIL file_{};
    Protocol::SampleLoadMessage request_{};
    Protocol::SampleStatusMessage status_{};
    AudioEngine::ResidentSampleInfo resident_{};
    SampleFile::Document saved_{};
    wxsamp_t handle_{};
    uint8_t* destination_ = nullptr;
    uint32_t copied_ = 0;
    Phase phase_ = Phase::Idle;
    bool open_ = false, reply_pending_ = false;
};
}  // namespace WaveX::Storage
