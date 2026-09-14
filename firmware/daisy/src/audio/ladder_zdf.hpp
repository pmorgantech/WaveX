#pragma once

// Zero-delay-feedback (topology-preserving-transform) Moog ladder, per
// Zavalishin, "The Art of VA Filter Design", ch. 5, with the response mixing
// of Valimaki and Huovilainen (CMJ 2006) for the 12 dB and HP/BP outputs.
// First-party; no third-party code.
//
// WHY A SECOND LADDER. ladder_huovilainen.hpp is a unit-delay-feedback
// ladder and needs oversampling to keep its tuning and its aliasing under
// control: four passes per sample, each with a tanh, and it measured ~40
// points of the block budget for eight voices (2026-09-14). This one solves
// the feedback loop implicitly, so the linear part is exact at any sample
// rate with no oversampling, and a single saturation per sample - applied to
// the solved loop input, the usual "cheap" nonlinear ZDF - gives it a
// resonance that self-oscillates cleanly and never grows without bound
// (the clip only ever lowers the loop gain). About 40 flops and one divide
// per sample: the same order as the 24 dB SVF.
//
// Structure per sample, G = g / (1 + g) with g = tan(pi fc / fs):
//   S  = (1 - G) (G^3 s1 + G^2 s2 + G s3 + s4)     the states' contribution
//   u  = clip((x_comp - k S) / (1 + k G^4))        the loop input, solved,
//                                                   then the one nonlinearity
//   y_i = G (u - s_i) + s_i ; s_i = 2 y_i - s_i     four TPT one-poles
// Resonance k is 0..4 (self-oscillation at 4), and the input is raised by
// 1 + 0.5 k against the passband loss a ladder's global feedback causes,
// the same compensation DaisySP's model applies.
//
// HAL-free: plain float arithmetic, host-testable. Lives inside Voice, so it
// inherits VoiceManager's DTCM placement.

#include "fast_tan.hpp"
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

class ZdfLadder {
   public:
    enum class Mode : uint8_t { LP24, LP12, BP24, BP12, HP24, HP12 };

    static constexpr float kMaxResonance = 1.4f;  // loop gain 5.6

    void Init(float sample_rate) {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        mode_ = Mode::LP24;
        Reset();
        SetInputDrive(1.0f);
        SetRes(0.0f);
        SetFreq(5000.0f);
    }

    void Reset() {
        for (int i = 0; i < 4; ++i)
            s_[i] = 0.0f;
    }

    // Hz, clamped to 5 .. 0.45 * sample rate.
    void SetFreq(float hz) {
        const float top = sample_rate_ * 0.45f;
        hz = hz < 5.0f ? 5.0f : (hz > top ? top : hz);
        const float g = TanPi(hz / sample_rate_);
        G_ = g / (1.0f + g);
        G2_ = G_ * G_;
        G3_ = G2_ * G_;
        one_minus_G_ = 1.0f - G_;
        UpdateLoop();
    }

    // k = 4 * res. At res 1 the loop gain is exactly marginal and the clip
    // damps it, so the range runs to 1.4 (k = 5.6) for a clean, bounded,
    // decidedly aggressive self-oscillation; the clip is what bounds it.
    void SetRes(float res) {
        res = res < 0.0f ? 0.0f : (res > kMaxResonance ? kMaxResonance : res);
        k_ = 4.0f * res;
        input_gain_ = 1.0f + kPassbandCompensation * k_;
        UpdateLoop();
    }

    // DaisySP's ladder drive law: a gain of 0..4 on the input ahead of the
    // feedback sum and the clip, weighted by the passband compensation
    // above 1, and NO make-up afterwards - the SVF's input stage uses the
    // same law, so the two topologies sit at the same level for the same
    // drive setting. (A level-neutral drive, divided back out, made this
    // ladder audibly quieter than the others on the bench, 2026-09-14.)
    void SetInputDrive(float drive) {
        drive = drive < 0.0f ? 0.0f : (drive > 4.0f ? 4.0f : drive);
        drive_scaled_ =
            drive > 1.0f ? 1.0f + (drive - 1.0f) * (1.0f - kPassbandCompensation) : drive;
    }

    void SetFilterMode(Mode mode) { mode_ = mode; }
    Mode GetFilterMode() const { return mode_; }

    float Process(float in) {
        const float S = one_minus_G_ * (G3_ * s_[0] + G2_ * s_[1] + G_ * s_[2] + s_[3]);
        const float x = in * drive_scaled_ * input_gain_;
        const float u = Clip((x - k_ * S) * loop_inv_);
        const float y1 = Stage(u, s_[0]);
        const float y2 = Stage(y1, s_[1]);
        const float y3 = Stage(y2, s_[2]);
        const float y4 = Stage(y3, s_[3]);
        switch (mode_) {
            case Mode::LP24:
                return y4;
            case Mode::LP12:
                return y2;
            case Mode::BP24:
                return (y2 + y4) * 4.0f - y3 * 8.0f;
            case Mode::BP12:
                return (y1 - y2) * 2.0f;
            case Mode::HP24:
                return u + y4 - (y1 + y3) * 4.0f + y2 * 6.0f;
            case Mode::HP12:
                return u + y2 - y1 * 2.0f;
            default:
                return 0.0f;
        }
    }

   private:
    static constexpr float kPassbandCompensation = 0.5f;

    void UpdateLoop() { loop_inv_ = 1.0f / (1.0f + k_ * G2_ * G2_); }

    // The rational tanh of the DaisySP/Teensy ladder: one divide, unit
    // slope at the origin, hard limit at |x| >= 3.
    static float Clip(float x) {
        if (x > 3.0f)
            return 1.0f;
        if (x < -3.0f)
            return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    float Stage(float x, float& s) const {
        const float v = (x - s) * G_;
        const float y = v + s;
        s = y + v;
        return y;
    }

    float sample_rate_ = 48000.0f;
    float G_ = 0.0f, G2_ = 0.0f, G3_ = 0.0f, one_minus_G_ = 1.0f;
    float k_ = 0.0f, loop_inv_ = 1.0f, input_gain_ = 1.0f;
    float drive_scaled_ = 1.0f;
    float s_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Mode mode_ = Mode::LP24;
};

}  // namespace AudioEngine
}  // namespace WaveX
