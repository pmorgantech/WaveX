#pragma once
#include "memory_sections.h"
#include "spi_protocol/protocol.h"

#include "lfo_sine.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
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
        return LfoControl::CyclePhase(beat_phase, division);
    }
    WAVEX_ITCM_CODE_NAMED("lfo.Start")
    void Start(const Protocol::InstLfoSettings& settings,
               uint32_t sample_rate,
               float pitch_ratio,
               uint64_t frame_clock,
               uint64_t beat_clock,
               uint32_t beat_step,
               uint32_t offset,
               uint32_t random_seed,
               uint8_t index) {
        Configure(settings, sample_rate, pitch_ratio);
        rate_scale_q16_ = 65536;
        elapsed_frames_ = 0;
        // This clock advances even while every physical voice is idle.
        phase_ = settings.retrigger || !enabled_ ? 0
                 : division_ ? DivideBeatPhase(beat_clock + uint64_t{beat_step} * offset, division_)
                             : (frame_clock + offset) * rate_step_;
        seed_ = settings.retrigger ? random_seed : 0x9e3779b9u * (index + 1u);
        Recompute();
    }
    // Changes the sound at its current phase and note age. Gate/free policy
    // takes effect at the next note admission; neither edit nor Revert retriggers.
    WAVEX_ITCM_CODE_NAMED("lfo.UpdateSettings")
    void UpdateSettings(const Protocol::InstLfoSettings& settings,
                        uint32_t sample_rate,
                        float pitch_ratio) {
        if (ConfigurationMatches(settings, sample_rate, pitch_ratio))
            return;
        Configure(settings, sample_rate, pitch_ratio);
        Recompute();
    }
    float Advance(uint32_t frames, uint32_t beat_step) {
        if (!frames)
            return value_;
        elapsed_frames_ += std::min(frames, UINT32_MAX - elapsed_frames_);
        if (!enabled_)
            return 0;
        uint64_t advance = division_ ? DivideBeatPhase(uint64_t{beat_step} * frames, division_)
                                     : uint64_t{rate_step_} * frames;
        if (rate_scale_q16_ != 65536) {
            // Split before multiplication so long host-test blocks cannot
            // overflow the intermediate Q16 product. No phase/age reset.
            advance =
                (advance >> 16) * rate_scale_q16_ + ((advance & 65535u) * rate_scale_q16_ >> 16);
            advance = std::clamp(
                advance, uint64_t{min_rate_step_} * frames, uint64_t{max_rate_step_} * frames);
        }
        phase_ += advance;
        Recompute();
        return value_;
    }
    float Value() const { return value_; }
    void SetRateMultiplier(float multiplier) {
        if (!(multiplier >= .0625f && multiplier <= 16.f))
            multiplier = 1.f;
        rate_scale_q16_ = static_cast<uint32_t>(multiplier * 65536.f);
    }
    float Phase() const {
        return static_cast<float>(static_cast<uint32_t>(phase_)) / 4294967296.0f;
    }

   private:
    bool ConfigurationMatches(const Protocol::InstLfoSettings& settings,
                              uint32_t sample_rate,
                              float pitch_ratio) const {
        return configured_ && configured_sample_rate_ == (sample_rate ? sample_rate : 48000) &&
               (!settings.pitch_follow || settings.sync_div ||
                configured_pitch_ratio_ == pitch_ratio) &&
               std::memcmp(&settings_, &settings, sizeof(settings)) == 0;
    }
    WAVEX_ITCM_CODE_NAMED("lfo.Configure")
    void Configure(const Protocol::InstLfoSettings& settings,
                   uint32_t sample_rate,
                   float pitch_ratio) {
        // Physical voices commonly retrigger the same sound. Reuse only the
        // derived configuration; Start still resets note age, phase and seed.
        if (ConfigurationMatches(settings, sample_rate, pitch_ratio))
            return;
        settings_ = settings;
        sample_rate = sample_rate ? sample_rate : 48000;
        configured_sample_rate_ = sample_rate;
        configured_pitch_ratio_ = pitch_ratio;
        configured_ = true;
        wave_ = settings.wave;
        division_ = settings.sync_div;
        enabled_ = wave_ <= 4 && LfoControl::ValidDivision(division_);
        float hz = settings.rate_hz;
        if (!(hz >= 0 && hz <= 1000))
            hz = 1;
        if (settings.pitch_follow && !division_)
            hz *= pitch_ratio;
        if (!(hz >= 0))
            hz = 1;
        hz = std::clamp(hz, LfoControl::kMinRateHz, LfoControl::kMaxRateHz);
        rate_step_ = static_cast<uint32_t>(
            std::min(4294967295.0, static_cast<double>(hz) * 4294967296.0 / sample_rate));
        min_rate_step_ = static_cast<uint32_t>(LfoControl::kMinRateHz * 4294967296.0 / sample_rate);
        max_rate_step_ = static_cast<uint32_t>(
            std::min(4294967295.0, LfoControl::kMaxRateHz * 4294967296.0 / sample_rate));
        auto frames = [sample_rate](float seconds) {
            if (!(seconds >= 0 && seconds <= 600))
                seconds = 0;
            return static_cast<uint32_t>(seconds * static_cast<float>(sample_rate));
        };
        delay_frames_ = frames(settings.delay_s);
        fade_frames_ = frames(settings.fade_s);
        fade_scale_ = fade_frames_ ? 1.0f / static_cast<float>(fade_frames_) : 1.0f;
    }
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
    Protocol::InstLfoSettings settings_;
    uint32_t configured_sample_rate_ = 0;
    float configured_pitch_ratio_ = 0;
    bool configured_ = false;
    uint64_t phase_ = 0;
    uint32_t rate_step_ = 0, delay_frames_ = 0, fade_frames_ = 0, elapsed_frames_ = 0, seed_ = 0;
    uint32_t rate_scale_q16_ = 65536, min_rate_step_ = 0, max_rate_step_ = 0;
    float fade_scale_ = 1, value_ = 0;
    uint8_t wave_ = 0, division_ = 0;
    bool enabled_ = false;
};
}  // namespace WaveX::AudioEngine
