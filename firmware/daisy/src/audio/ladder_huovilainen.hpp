#pragma once

// First-party port of DaisySP's LadderFilter (Source/Filters/ladder.h and
// ladder.cpp at the submodule's pinned commit), which is itself a port of the
// Teensy Audio Library ladder by Richard van Hoesel. MIT; the original header
// is retained verbatim below as its terms require.
//
// WHY A PORT RATHER THAN THE VENDORED OBJECT. The oversampling factor is a
// private constant in DaisySP's class and its state can only be cleared by
// re-initialising the tuning, and the two things WaveX needs from this
// filter are exactly a choice of oversampling factor (the 4x model measured
// ~40 points of the block budget for eight voices, 2026-09-14) and a Reset()
// that a stolen voice can call without retuning. The template parameter is
// the oversampling factor; HuovilainenLadder<4> is arithmetic-for-arithmetic
// the DaisySP filter (ladder_huovilainen_test.cpp pins that against the
// vendored object, bit for bit on the host), and HuovilainenLadder<2> is the
// same model at Huovilainen's own published rate, at about half the cost.
// Nothing else differs between the two, so an A/B between them hears only
// the oversampling.
//
// Model: Huovilainen's non-linear Moog ladder (CMJ 2006) with van Hoesel's
// tuning polynomials, a rational tanh in the feedback path, linear input
// interpolation across the oversampled passes, and Valimaki/Huovilainen's
// weighted stage sums for the 12 dB and HP/BP responses.
//
// HAL-free: plain float arithmetic, host-testable. Lives inside Voice, so it
// inherits VoiceManager's DTCM placement.

/* Ported from Audio Library for Teensy, Ladder Filter
 * Copyright (c) 2021, Richard van Hoesel
 * Copyright (c) 2024, Infrasonic Audio LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
//-----------------------------------------------------------
// Huovilainen New Moog (HNM) model as per CMJ jun 2006
// Richard van Hoesel, v. 1.03, Feb. 14 2021
// v1.7 (Infrasonic/Daisy) add configurable filter mode
// v1.6 (Infrasonic/Daisy) removes polyphase FIR, uses 4x linear
//      oversampling for performance reasons
// v1.5 adds polyphase FIR or Linear interpolation
// v1.4 FC extended to 18.7kHz, max res to 1.8, 4x oversampling,
//      and a minor Q-tuning adjustment
// v.1.03 adds oversampling, extended resonance,
// and exposes parameters input_drive and passband_gain
// v.1.02 now includes both cutoff and resonance "CV" modulation inputs
// please retain this header if you use this code.
//-----------------------------------------------------------

#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

template <uint8_t kOversample>
class HuovilainenLadder {
    static_assert(kOversample >= 1 && kOversample <= 8, "oversampling factor");

   public:
    enum class Mode : uint8_t { LP24, LP12, BP24, BP12, HP24, HP12 };

    static constexpr float kMaxResonance = 1.8f;

    // Same defaults as DaisySP's Init(): 5 kHz, resonance 0.2, passband gain
    // 0.5 and input drive 0.5 (which halves the level; VoiceFilter sets its
    // own). Also clears the state.
    void Init(float sample_rate) {
        sample_rate_ = sample_rate;
        sr_int_recip_ = 1.0f / (sample_rate * static_cast<float>(kOversample));
        alpha_ = 1.0f;
        K_ = 1.0f;
        Fbase_ = 1000.0f;
        Qadjust_ = 1.0f;
        mode_ = Mode::LP24;
        Reset();
        SetPassbandGain(0.5f);
        SetInputDrive(0.5f);
        SetFreq(5000.f);
        SetRes(0.2f);
    }

    // Clears the stage and interpolator state; tuning, drive and mode stay.
    void Reset() {
        for (int i = 0; i < 4; ++i) {
            z0_[i] = 0.0f;
            z1_[i] = 0.0f;
        }
        oldinput_ = 0.0f;
    }

    float Process(float in) {
        const float input = in * drive_scaled_;
        float total = 0.0f;
        float interp = 0.0f;
        for (size_t os = 0; os < kOversample; os++) {
            const float in_interp = (interp * oldinput_ + (1.0f - interp) * input);
            float u = in_interp - (z1_[3] - pbg_ * in_interp) * K_ * Qadjust_;
            u = FastTanh(u);
            const float stage1 = Lpf(u, 0);
            const float stage2 = Lpf(stage1, 1);
            const float stage3 = Lpf(stage2, 2);
            const float stage4 = Lpf(stage3, 3);
            total += WeightedSum(u, stage1, stage2, stage3, stage4) * kInterpolationRecip;
            interp += kInterpolationRecip;
        }
        oldinput_ = input;
        return total;
    }

    // Hz, clamped to 5 .. 0.425 * sample rate.
    void SetFreq(float freq) {
        Fbase_ = freq;
        ComputeCoeffs(freq);
    }

    // 0 .. 1.8; stable self-oscillation at the top. K = 4 * res.
    void SetRes(float res) {
        res = Clamp(res, 0.0f, kMaxResonance);
        K_ = 4.0f * res;
    }

    // 0 .. 0.5: compensates the passband loss at high resonance.
    void SetPassbandGain(float pbg) {
        pbg_ = Clamp(pbg, 0.0f, 0.5f);
        SetInputDrive(drive_);
    }

    // 0 .. 4 into the tanh; below 1 it is an attenuation.
    void SetInputDrive(float odrv) {
        drive_ = odrv > 0.0f ? odrv : 0.0f;
        if (drive_ > 1.0f) {
            drive_ = drive_ < 4.0f ? drive_ : 4.0f;
            // max is 4 when pbg = 0, and 2.5 when pbg is 0.5
            drive_scaled_ = 1.0f + (drive_ - 1.0f) * (1.0f - pbg_);
        } else {
            drive_scaled_ = drive_;
        }
    }

    void SetFilterMode(Mode mode) { mode_ = mode; }
    Mode GetFilterMode() const { return mode_; }

   private:
    static constexpr float kInterpolationRecip = 1.0f / static_cast<float>(kOversample);
    static constexpr float kPi = 3.1415927410125732421875f;  // DaisySP's PI_F

    static float Clamp(float v, float lo, float hi) {
        v = v > lo ? v : lo;
        return v < hi ? v : hi;
    }

    // DaisySP's fast_tanh, verbatim: one divide per pass.
    static float FastTanh(float x) {
        if (x > 3.0f)
            return 1.0f;
        if (x < -3.0f)
            return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    float Lpf(float s, int i) {
        //             (1.0 / 1.3)   (0.3 / 1.3)
        float ft = s * 0.76923077f + 0.23076923f * z0_[i] - z1_[i];
        ft = ft * alpha_ + z1_[i];
        z1_[i] = ft;
        z0_[i] = s;
        return ft;
    }

    void ComputeCoeffs(float freq) {
        freq = Clamp(freq, 5.0f, sample_rate_ * 0.425f);
        const float wc = freq * 2.0f * kPi * sr_int_recip_;
        const float wc2 = wc * wc;
        alpha_ = 0.9892f * wc - 0.4324f * wc2 + 0.1381f * wc * wc2 - 0.0202f * wc2 * wc2;
        // revised hfQ (rvh - feb 14 2021)
        Qadjust_ = 1.006f + 0.0536f * wc - 0.095f * wc2 - 0.05f * wc2 * wc2;
    }

    // Weighted filter stage mixing to achieve the selected response, as
    // described in "Oscillator and Filter Algorithms for Virtual Analog
    // Synthesis", Valimaki and Huovilainen, CMJ 2006.
    float WeightedSum(float u, float s1, float s2, float s3, float s4) const {
        switch (mode_) {
            case Mode::LP24:
                return s4;
            case Mode::LP12:
                return s2;
            case Mode::BP24:
                return (s2 + s4) * 4.0f - s3 * 8.0f;
            case Mode::BP12:
                return (s1 - s2) * 2.0f;
            case Mode::HP24:
                return u + s4 - ((s1 + s3) * 4.0f) + s2 * 6.0f;
            case Mode::HP12:
                return u + s2 - s1 * 2.0f;
            default:
                return 0.0f;
        }
    }

    float sample_rate_ = 48000.0f, sr_int_recip_ = 0.0f;
    float alpha_ = 1.0f;
    float z0_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float z1_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float K_ = 1.0f;
    float Fbase_ = 1000.0f;
    float Qadjust_ = 1.0f;
    float pbg_ = 0.5f;
    float drive_ = 0.5f, drive_scaled_ = 0.5f;
    float oldinput_ = 0.0f;
    Mode mode_ = Mode::LP24;
};

}  // namespace AudioEngine
}  // namespace WaveX
