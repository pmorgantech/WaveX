#pragma once

// Playback-time region fades and de-click (roadmap 1.5.6 item 3).
//
// A region that starts mid-waveform starts on a step from silence to whatever
// the sample happened to be doing at that frame, and a step is a click. The
// fix is a ramp of a few milliseconds - short enough to be inaudible as a
// fade, long enough that the step becomes a slope.
//
// Shape: raised cosine, g(t) = (1 - cos(pi*t)) / 2, NOT linear. Roadmap item 4
// flags shape as mattering more than it looks, and the reason applies here as
// much as to a crossfade: a linear ramp has a corner at each end, and a corner
// in amplitude is a discontinuity in the first derivative - which is audible
// as a faint thump on exactly the material where a click was the problem. The
// raised cosine leaves and arrives with zero slope, so both ends are smooth.
//
// This is deliberately not the equal-power (sin/cos) pair item 4 recommends
// for crossfades: equal power is right when two signals are summing and the
// sum must hold level. A fade to or from SILENCE has nothing to hold level
// against, and an equal-power fade-in would start at -3 dB rather than at
// zero - which is a step, i.e. the thing being fixed.
//
// HAL-free and header-only so both playback paths and the host tests share one
// definition. The curve is tabulated rather than computed: VoiceManager
// evaluates it per sample in the audio callback, where a cosf call is a
// library call with no worst-case guarantee. See detail::FadeTable below.
//
// The per-sample evaluation is a multiply and a table lerp, nothing else. The
// only division - frames to table steps - happens once per region in
// RegionFade::Prepare(), which VoiceManager calls per voice per BLOCK. An
// earlier version divided per sample, and as a 64-bit integer divide at that:
// on Cortex-M7 that is a __aeabi_uldivmod library call of 100+ cycles, twice
// per sample for a region with both fades, which cost more than the rest of
// the voice's per-sample work put together.

#include <cmath>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

/// Shortest fade that is worth applying. Below this the ramp is a handful of
/// frames and does not reliably remove the step it exists for.
static constexpr uint32_t kMinFadeFrames = 4;

namespace detail {

// The curve, tabulated. VoiceManager::Render() evaluates this per sample in
// the AUDIO CALLBACK, and cosf there is a library call of 50-150 cycles with
// no worst-case guarantee - which is precisely the shape of thing the
// real-time rules say must not go in the callback. A table lookup plus one
// lerp is a handful of cycles and, more importantly, the SAME handful every
// time. 257 entries (256 intervals + the endpoint) is ~1 KB and holds the
// interpolation error near 1e-5, far below the q15 floor either caller
// quantises to.
//
// Built at static-init time rather than on first use: a function-local static
// would put a __cxa_guard acquire on the callback's fast path, and its first
// execution would be the initialisation itself - inside the callback. This
// target is C++14, so the header-only global is a static member of a class
// template rather than an inline variable; either way it is dynamically
// initialised before main(), and audio starts well after that.
//
// Built in float on purpose: this is the only cos() in the firmware, and a
// double cos() links ~5 KB of double-precision libm (rem_pio2 and friends) for
// a 257-entry table whose endpoints are pinned below anyway. Single precision
// is accurate to an ulp here, far inside the 1e-5 interpolation error.
struct FadeTable {
    static constexpr uint32_t kSteps = 256;
    float g[kSteps + 1];

    FadeTable() {
        constexpr float kPi = 3.14159265358979323846f;
        for (uint32_t i = 0; i <= kSteps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            g[i] = 0.5f * (1.0f - std::cos(kPi * t));
        }
        // Pin the endpoints exactly. cos() is within an ulp of +/-1 here, but
        // "within an ulp" of zero is not zero, and a fade that starts at 1e-8
        // instead of 0 still starts on a step - a much smaller one, but the
        // property being relied on is exactness at the boundary.
        g[0] = 0.0f;
        g[kSteps] = 1.0f;
    }
};

template <typename Tag = void>
struct FadeTableHolder {
    static const FadeTable instance;
};
template <typename Tag>
const FadeTable FadeTableHolder<Tag>::instance;

inline const FadeTable& Table() {
    return FadeTableHolder<>::instance;
}

// Curve value at `steps` table steps in, 0 <= steps. At or past the last
// entry this is exactly 1.0. The caller has already scaled frames to steps.
inline float Lookup(float steps) {
    const FadeTable& t = Table();
    const uint32_t index = static_cast<uint32_t>(steps);
    if (index >= FadeTable::kSteps) {
        return t.g[FadeTable::kSteps];
    }
    const float frac = steps - static_cast<float>(index);
    return t.g[index] + (t.g[index + 1] - t.g[index]) * frac;
}

// Table steps per frame for a `length`-frame ramp. The one divide in this
// file; length is > 0 by the callers' contract.
inline float StepsPerFrame(uint32_t length) {
    return static_cast<float>(FadeTable::kSteps) / static_cast<float>(length);
}

}  // namespace detail

/**
 * @brief Raised-cosine fade gain at `position` of a `length`-frame ramp.
 *
 * Returns exactly 0.0 at position 0 and exactly 1.0 at position >= length.
 * `length` 0 means no fade, which returns 1.0 - an absent fade must be unity,
 * never silence.
 *
 * Callback-safe: one float divide, a table lookup and one interpolation - no
 * transcendental and no branch whose cost depends on the input. For a loop
 * over consecutive frames, RegionFade below hoists even the divide.
 */
inline float FadeGain(uint32_t position, uint32_t length) {
    if (length == 0 || position >= length) {
        return 1.0f;
    }
    if (position == 0) {
        return 0.0f;
    }
    return detail::Lookup(static_cast<float>(position) * detail::StepsPerFrame(length));
}

/**
 * @brief The fades of one region [start, end), prepared once, evaluated per
 * frame.
 *
 * Combines the fade-in at the head and the fade-out at the tail. Both are
 * clamped so they cannot overlap: a fade-in and fade-out that together exceed
 * the region would otherwise multiply into a region that never reaches unity,
 * which reads as "the sample got quieter" rather than as a fade.
 *
 * Frames outside [start, end) return 1.0 rather than 0.0 - this shapes audio
 * that is already inside the region, and silencing anything else here would
 * hide a region-clamping bug rather than expose it.
 *
 * Prepare() does the clamping and the two frames-to-steps divides; Gain() is
 * then a couple of compares, a multiply and a table lerp per fade. A
 * default-constructed RegionFade is inactive and returns 1.0 everywhere, so
 * a caller can prepare it conditionally and still call Gain() unconditionally.
 */
class RegionFade {
   public:
    static RegionFade Prepare(uint32_t start,
                              uint32_t end,
                              uint32_t fade_in_frames,
                              uint32_t fade_out_frames) {
        RegionFade f;
        if (end <= start) {
            return f;
        }
        const uint32_t span = end - start;

        uint32_t fade_in = (fade_in_frames >= kMinFadeFrames) ? fade_in_frames : 0;
        uint32_t fade_out = (fade_out_frames >= kMinFadeFrames) ? fade_out_frames : 0;
        const uint64_t requested_fade = static_cast<uint64_t>(fade_in) + fade_out;
        if (requested_fade > span) {
            // Share the region proportionally rather than letting the head
            // win and the tail vanish; a request for a 1 s fade each way on a
            // 100 ms region is a UI that has not been clamped, and half each
            // is the least surprising reading of it.
            if (fade_in >= fade_out) {
                fade_in =
                    static_cast<uint32_t>((static_cast<uint64_t>(span) * fade_in) / requested_fade);
                fade_out = span - fade_in;
            } else {
                fade_out = static_cast<uint32_t>((static_cast<uint64_t>(span) * fade_out) /
                                                 requested_fade);
                fade_in = span - fade_out;
            }
        }

        f.start_ = start;
        f.end_ = end;
        f.fade_in_ = fade_in;
        f.fade_out_ = fade_out;
        f.in_steps_per_frame_ = fade_in > 0 ? detail::StepsPerFrame(fade_in) : 0.0f;
        f.out_steps_per_frame_ = fade_out > 0 ? detail::StepsPerFrame(fade_out) : 0.0f;
        return f;
    }

    /// False when neither fade is applied, i.e. Gain() is 1.0 everywhere.
    bool Active() const { return fade_in_ != 0 || fade_out_ != 0; }

    float Gain(uint32_t frame) const {
        if (frame < start_ || frame >= end_) {
            return 1.0f;
        }
        float g = 1.0f;
        if (fade_in_ != 0) {
            const uint32_t position = frame - start_;
            if (position == 0) {
                return 0.0f;
            }
            if (position < fade_in_) {
                g = detail::Lookup(static_cast<float>(position) * in_steps_per_frame_);
            }
        }
        if (fade_out_ != 0) {
            const uint32_t from_end = end_ - 1 - frame;
            if (from_end == 0) {
                return 0.0f;
            }
            if (from_end < fade_out_) {
                g *= detail::Lookup(static_cast<float>(from_end) * out_steps_per_frame_);
            }
        }
        return g;
    }

   private:
    uint32_t start_ = 0;
    uint32_t end_ = 0;
    uint32_t fade_in_ = 0;
    uint32_t fade_out_ = 0;
    float in_steps_per_frame_ = 0.0f;
    float out_steps_per_frame_ = 0.0f;
};

/**
 * @brief Gain for a single frame at `frame` within the region [start, end).
 *
 * RegionFade::Prepare() + Gain() for one frame. Right for a one-off query;
 * a loop over frames should prepare once and call Gain() per frame.
 */
inline float RegionFadeGain(uint32_t frame,
                            uint32_t start,
                            uint32_t end,
                            uint32_t fade_in_frames,
                            uint32_t fade_out_frames) {
    return RegionFade::Prepare(start, end, fade_in_frames, fade_out_frames).Gain(frame);
}

/// Milliseconds to frames at `sample_rate`, saturating rather than wrapping.
inline uint32_t FadeFrames(uint16_t ms, uint32_t sample_rate) {
    if (ms == 0 || sample_rate == 0) {
        return 0;
    }
    const uint64_t frames = (static_cast<uint64_t>(ms) * sample_rate) / 1000u;
    return (frames > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(frames);
}

}  // namespace AudioEngine
}  // namespace WaveX
