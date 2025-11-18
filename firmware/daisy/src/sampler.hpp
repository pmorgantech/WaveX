#pragma once

#include <math.h>
#include <stdint.h>

#include "../memory.h"  // For SampleMemMgr and wxsamp_t

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

class Sampler {
   public:
    void Init(uint32_t sample_rate) {
        sr_ = sample_rate;
        // Allocate a smaller initial buffer to avoid heap issues
        // Will grow dynamically as needed during recording
        buffer_.reserve(1024);  // Start with 1KB instead of 192KB
    }
    void StartRec() {
        buffer_.clear();
        rec_ = true;
    }
    void StopRec() { rec_ = false; }
    void StartPlay(float rate = 1.0f) {
        play_ = true;
        ph_ = 0.0f;
        inc_ = rate;
    }
    void StopPlay() { play_ = false; }

    void FeedInputBlock(const float* in, size_t n) {
        if (!rec_)
            return;
        buffer_.reserve(buffer_.size() + n);
        for (size_t i = 0; i < n; i++) {
            float x = in[i] * 32767.0f;
            if (x > 32767.0f)
                x = 32767.0f;
            if (x < -32768.0f)
                x = -32768.0f;
            buffer_.push_back((int16_t)x);
        }
    }

    float Next() {
        if (!play_ || buffer_.empty())
            return 0.0f;
        size_t i = (size_t)ph_;
        size_t j = (i + 1 < buffer_.size()) ? i + 1 : i;
        float frac = ph_ - (float)i;
        float a = (float)buffer_[i];
        float b = (float)buffer_[j];
        ph_ += inc_;
        if (ph_ >= (float)buffer_.size())
            play_ = false;
        return ((a * (1.0f - frac) + b * frac) * (1.0f / 32767.0f));
    }

    void MakePreview(size_t start, size_t end, size_t decim, std::vector<int16_t>& out) const {
        if (end > buffer_.size())
            end = buffer_.size();
        if (start > end)
            start = end;
        out.clear();
        if (decim == 0)
            decim = 1;
        out.reserve((end - start) / decim + 1);
        for (size_t i = start; i < end; i += decim)
            out.push_back(buffer_[i]);
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

   private:
    uint32_t sr_ = 48000u;
    std::vector<int16_t> buffer_;
    bool rec_ = false;
    bool play_ = false;
    float ph_ = 0.0f;
    float inc_ = 1.0f;
};
