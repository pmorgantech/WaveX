#pragma once
#include "spi_protocol/protocol.h"

#include "lfo_sine.hpp"
#include <algorithm>
#include <cstdint>
namespace WaveX::AudioEngine {
// Callback-owned phase and delayed-vibrato state. Settings are copied at
// note admission, never borrowed from the foreground Instrument.
class VoiceLfo {
   public:
    static uint32_t BeatStep(float bpm, uint32_t sample_rate) {
        if (!(bpm >= 1 && bpm <= 1000))
            bpm = 120;
        sample_rate = sample_rate ? sample_rate : 48000;
        return static_cast<uint32_t>(
            std::min(4294967295.0, static_cast<double>(bpm) * 4294967296.0 / (60.0 * sample_rate)));
    }
    static uint64_t DivideBeatPhase(uint64_t beat_phase, uint8_t division) {
        return division <= 3 ? beat_phase << (3 - division) : beat_phase >> (division - 3);
    }
    void Start(const Protocol::InstLfoSettings& settings,
               uint32_t sample_rate,
               float pitch_ratio,
               uint64_t frame_clock,
               uint64_t beat_clock,
               uint32_t beat_step,
               uint32_t offset,
               uint32_t random_seed,
               uint8_t index) {
        sample_rate = sample_rate ? sample_rate : 48000;
        wave_ = settings.wave;
        division_ = settings.sync_div;
        enabled_ = wave_ <= 4 && division_ <= 7;
        float hz = settings.rate_hz;
        if (!(hz >= 0 && hz <= 1000))
            hz = 1;
        if (settings.pitch_follow && !division_)
            hz *= pitch_ratio;
        if (!(hz >= 0))
            hz = 1;
        hz = std::clamp(hz, .02f, 20.0f);
        rate_step_ = static_cast<uint32_t>(
            std::min(4294967295.0, static_cast<double>(hz) * 4294967296.0 / sample_rate));
        auto frames = [sample_rate](float seconds) {
            if (!(seconds >= 0 && seconds <= 600))
                seconds = 0;
            return static_cast<uint32_t>(seconds * static_cast<float>(sample_rate));
        };
        delay_frames_ = frames(settings.delay_s);
        fade_frames_ = frames(settings.fade_s);
        fade_scale_ = fade_frames_ ? 1.0f / static_cast<float>(fade_frames_) : 1.0f;
        elapsed_frames_ = 0;
        // This clock advances even while every physical voice is idle.
        phase_ = settings.retrigger || !enabled_ ? 0
                 : division_ ? DivideBeatPhase(beat_clock + uint64_t{beat_step} * offset, division_)
                             : (frame_clock + offset) * rate_step_;
        seed_ = settings.retrigger ? random_seed : 0x9e3779b9u * (index + 1u);
        Recompute();
    }
    float Advance(uint32_t frames, uint32_t beat_step) {
        if (!frames)
            return value_;
        if (!enabled_)
            return 0;
        phase_ += division_ ? DivideBeatPhase(uint64_t{beat_step} * frames, division_)
                            : uint64_t{rate_step_} * frames;
        const uint32_t end = delay_frames_ + fade_frames_;
        const uint32_t remaining = end - std::min(elapsed_frames_, end);
        elapsed_frames_ += std::min(frames, remaining);
        Recompute();
        return value_;
    }
    float Value() const { return value_; }
    float Phase() const {
        return static_cast<float>(static_cast<uint32_t>(phase_)) / 4294967296.0f;
    }

   private:
    void Recompute() {
        if (!enabled_ || elapsed_frames_ < delay_frames_) {
            value_ = 0;
            return;
        }
        const float phase = Phase();
        float wave = 0;
        switch (wave_) {
            case 0:
                wave = LfoSine(phase * 6.2831853071795865f);
                break;
            case 1:
                wave = phase < .5f ? 4 * phase - 1 : 3 - 4 * phase;
                break;
            case 2:
                wave = 2 * phase - 1;
                break;
            case 3:
                wave = phase < .5f ? 1 : -1;
                break;
            case 4: {
                uint32_t x = seed_ ^ (static_cast<uint32_t>(phase_ >> 32) + 0x2545f491u);
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                wave = static_cast<float>(x >> 8) / 8388608.0f - 1;
            } break;
        }
        const float gain =
            fade_frames_
                ? std::min(1.0f, static_cast<float>(elapsed_frames_ - delay_frames_) * fade_scale_)
                : 1.0f;
        value_ = wave * gain;
    }
    uint64_t phase_ = 0;
    uint32_t rate_step_ = 0, delay_frames_ = 0, fade_frames_ = 0, elapsed_frames_ = 0, seed_ = 0;
    float fade_scale_ = 1, value_ = 0;
    uint8_t wave_ = 0, division_ = 0;
    bool enabled_ = false;
};
}  // namespace WaveX::AudioEngine
