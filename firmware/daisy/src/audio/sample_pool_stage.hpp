#pragma once
#include "sample_pool.hpp"

namespace WaveX::AudioEngine {
// Foreground-only private ownership table for a whole-Project load. The live
// pool must remain immutable while Active(); its payloads stay allocated.
// Candidate Tracks begin empty and each is loaded at most once. This keeps
// the loader's ordinary per-Track release from releasing a borrowed sample.
class SamplePoolStage {
   public:
    SamplePoolStage(SamplePool& live, SamplePool& candidate, SampleMemMgr& memory)
        : live_(live), candidate_(candidate), memory_(memory) {}
    ~SamplePoolStage() { Rollback(); }
    SamplePoolStage(const SamplePoolStage&) = delete;
    SamplePoolStage& operator=(const SamplePoolStage&) = delete;
    bool Begin() {
        if (active_ || &live_ == &candidate_)
            return false;
        candidate_.CopyStateFrom(live_);
        candidate_.ForEach([](SamplePool::Record& record) { record.used_by = 0; });
        active_ = true;
        return true;
    }
    bool Active() const { return active_; }
    // Caller has cancelled/drained the current loader job first. Only samples
    // admitted into the candidate are released; live ids/handles/undo survive.
    void Rollback() {
        if (!active_)
            return;
        candidate_.ForEach([&](SamplePool::Record& record) {
            if (!live_.Find(record.sample_id))
                Release(record);
        });
        active_ = false;
    }
    // Requires the audio stop fence and a COMPLETE prepared Track bank.
    // This metadata copy cannot fail or allocate. Only now retire old PCM
    // that the new Tracks and explicit user pins do not retain.
    bool Commit() {
        if (!active_)
            return false;
        candidate_.ForEach([&](SamplePool::Record& record) {
            if (!record.used_by && !record.pinned)
                Release(record);
        });
        live_.CopyStateFrom(candidate_);
        active_ = false;
        return true;
    }

   private:
    void Release(SamplePool::Record& record) {
        const uint16_t id = record.sample_id;
        if (record.payload.handle.len)
            memory_.release(&record.payload.handle);
        candidate_.Remove(id);
    }
    SamplePool& live_;
    SamplePool& candidate_;
    SampleMemMgr& memory_;
    bool active_ = false;
};
}  // namespace WaveX::AudioEngine
