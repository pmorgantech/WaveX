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
struct FadeTable {
    static constexpr uint32_t kSteps = 256;
    float g[kSteps + 1];

    FadeTable() {
        for (uint32_t i = 0; i <= kSteps; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(kSteps);
            g[i] = static_cast<float>(0.5 * (1.0 - std::cos(3.14159265358979323846 * t)));
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

}  // namespace detail

/**
 * @brief Raised-cosine fade gain at `position` of a `length`-frame ramp.
 *
 * Returns exactly 0.0 at position 0 and exactly 1.0 at position >= length.
 * `length` 0 means no fade, which returns 1.0 - an absent fade must be unity,
 * never silence.
 *
 * Callback-safe: a table lookup and one interpolation, no transcendental and
 * no branch whose cost depends on the input.
 */
inline float FadeGain(uint32_t position, uint32_t length) {
    if (length == 0 || position >= length) {
        return 1.0f;
    }
    if (position == 0) {
        return 0.0f;
    }
    const detail::FadeTable& t = detail::Table();
    // Index in table steps, with the fractional part kept for the lerp.
    const uint64_t scaled =
        (static_cast<uint64_t>(position) * detail::FadeTable::kSteps * 256u) / length;
    const uint32_t index = static_cast<uint32_t>(scaled >> 8);
    const float frac = static_cast<float>(scaled & 0xFFu) * (1.0f / 256.0f);
    if (index >= detail::FadeTable::kSteps) {
        return t.g[detail::FadeTable::kSteps];
    }
    return t.g[index] + (t.g[index + 1] - t.g[index]) * frac;
}

/**
 * @brief Gain for a frame at `frame` within the region [start, end).
 *
 * Combines the fade-in at the head and the fade-out at the tail. Both are
 * clamped so they cannot overlap: a fade-in and fade-out that together exceed
 * the region would otherwise multiply into a region that never reaches unity,
 * which reads as "the sample got quieter" rather than as a fade.
 *
 * Frames outside [start, end) return 1.0 rather than 0.0 - this shapes audio
 * that is already inside the region, and silencing anything else here would
 * hide a region-clamping bug rather than expose it.
 */
inline float RegionFadeGain(uint32_t frame,
                            uint32_t start,
                            uint32_t end,
                            uint32_t fade_in_frames,
                            uint32_t fade_out_frames) {
    if (end <= start || frame < start || frame >= end) {
        return 1.0f;
    }
    const uint32_t span = end - start;

    uint32_t fade_in = (fade_in_frames >= kMinFadeFrames) ? fade_in_frames : 0;
    uint32_t fade_out = (fade_out_frames >= kMinFadeFrames) ? fade_out_frames : 0;
    if (fade_in + fade_out > span) {
        // Share the region proportionally rather than letting the head win and
        // the tail vanish; a request for a 1 s fade each way on a 100 ms region
        // is a UI that has not been clamped, and half each is the least
        // surprising reading of it.
        if (fade_in >= fade_out) {
            fade_in = (span * fade_in) / (fade_in + fade_out);
            fade_out = span - fade_in;
        } else {
            fade_out = (span * fade_out) / (fade_in + fade_out);
            fade_in = span - fade_out;
        }
    }

    float g = 1.0f;
    if (fade_in > 0) {
        g *= FadeGain(frame - start, fade_in);
    }
    if (fade_out > 0) {
        const uint32_t from_end = end - 1 - frame;
        g *= FadeGain(from_end, fade_out);
    }
    return g;
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
