#pragma once

#include <math.h>
#include <stdint.h>

#include "../memory.h"  // For SampleMemMgr and wxsamp_t

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Roadmap Phase 1 item 3: recording must be real-time-safe. `buffer_` used
// to be a std::vector<int16_t> that grew via push_back() during
// FeedInputBlock() - if a recording exceeded the 1024-sample initial
// reserve() (barely 21ms at 48kHz), every subsequent sample would trigger a
// heap reallocation from inside whatever context calls FeedInputBlock(),
// violating AGENTS.md constraint #1 (no heap allocation in the audio path).
// `buffer_` is now a single fixed-capacity extent preallocated once from
// the SDRAM slab/extent allocator (memory.h, SampleMemMgr) at Init() time
// (main-loop/setup context - allocation itself is fine there, just never
// again afterward). FeedInputBlock() only ever writes within that fixed
// capacity and silently stops (rather than growing) once full.
class Sampler {
   public:
    // Preallocates a `max_frames`-sample (mono, int16) recording buffer
    // from `mem`. If the allocation fails (e.g. SDRAM exhausted),
    // recording/playback become safe no-ops rather than crashing.
    void Init(uint32_t sample_rate, SampleMemMgr& mem, uint32_t max_frames) {
        sr_ = sample_rate;
        capacity_ = max_frames;
        length_ = 0;
        mem_ = &mem;
        buffer_ = nullptr;
        if (capacity_ == 0)
            return;
        wxsamp_t handle{};
        if (!mem_->alloc(capacity_ * static_cast<uint32_t>(sizeof(int16_t)), &handle))
            return;
        void* p = nullptr;
        if (!mem_->ptr(handle, &p))
            return;
        handle_ = handle;
        buffer_ = static_cast<int16_t*>(p);
    }

    void StartRec() {
        length_ = 0;
        rec_ = true;
    }
    void StopRec() { rec_ = false; }
    void StartPlay(float rate = 1.0f) {
        play_ = true;
        ph_ = 0.0f;
        inc_ = rate;
    }
    void StopPlay() { play_ = false; }

    // Callback-safe: writes at most `capacity_ - length_` samples into the
    // preallocated buffer, never reallocates. Recording past capacity
    // simply stops (the tail is dropped) rather than growing or
    // overflowing.
    void FeedInputBlock(const float* in, size_t n) {
        if (!rec_ || !buffer_)
            return;
        for (size_t i = 0; i < n && length_ < capacity_; i++) {
            float x = in[i] * 32767.0f;
            if (x > 32767.0f)
                x = 32767.0f;
            if (x < -32768.0f)
                x = -32768.0f;
            buffer_[length_++] = (int16_t)x;
        }
    }

    float Next() {
        if (!play_ || !buffer_ || length_ == 0)
            return 0.0f;
        size_t i = (size_t)ph_;
        size_t j = (i + 1 < length_) ? i + 1 : i;
        float frac = ph_ - (float)i;
        float a = (float)buffer_[i];
        float b = (float)buffer_[j];
        ph_ += inc_;
        if (ph_ >= (float)length_)
            play_ = false;
        return ((a * (1.0f - frac) + b * frac) * (1.0f / 32767.0f));
    }

    void MakePreview(size_t start, size_t end, size_t decim, std::vector<int16_t>& out) const {
        if (end > length_)
            end = length_;
        if (start > end)
            start = end;
        out.clear();
        if (decim == 0)
            decim = 1;
        out.reserve((end - start) / decim + 1);
        for (size_t i = start; i < end; i += decim)
            out.push_back(buffer_ ? buffer_[i] : 0);
    }

    static void BlockMeters(const float* in, size_t n, float& rms, float& peak) {
        double acc = 0.0;
        float pk = 0.0f;
        for (size_t i = 0; i < n; i++) {
            float x = in[i];
            float ax = x < 0.0f ? -x : x;
            acc += (double)x * (double)x;
            if (ax > pk)
                pk = ax;
        }
        rms = n ? (float)sqrt(acc / (double)n) : 0.0f;
        peak = pk;
    }

    // For tests and diagnostics.
    uint32_t Capacity() const { return capacity_; }
    uint32_t Length() const { return length_; }

   private:
    uint32_t sr_ = 48000u;
    int16_t* buffer_ = nullptr;  // preallocated extent from mem_; not owned/freed here
    uint32_t capacity_ = 0;      // allocated frames
    uint32_t length_ = 0;        // frames actually recorded
    wxsamp_t handle_{};
    SampleMemMgr* mem_ = nullptr;
    bool rec_ = false;
    bool play_ = false;
    float ph_ = 0.0f;
    float inc_ = 1.0f;
};
