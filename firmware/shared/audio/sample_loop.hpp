#pragma once

#include "spi_protocol/protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace WaveX::SampleLoop {
constexpr uint32_t kMaxCrossfadeFrames = 2048;
constexpr uint32_t kMinLoopFrames = 256;
// The overlap consumes the loop head once. Repeating playback resumes after
// that head, making the period loop length minus overlap. Both channels use
// the same frames and ramp. No PCM is changed and no voice is allocated.
inline uint32_t CrossfadeFrames(uint8_t ms, uint32_t rate, uint32_t length) {
    if (length <= kMinLoopFrames || ms == 0)
        return 0;
    const uint32_t requested =
        static_cast<uint32_t>(uint64_t(std::min(ms, Protocol::kMaxLoopCrossfadeMs)) * rate / 1000);
    const uint32_t n =
        std::min({requested, kMaxCrossfadeFrames, length / 2, length - kMinLoopFrames});
    return n >= 2 ? n : 0;
}
// A Q15, position-dependent two-input ramp has no equivalent libDaisy,
// DaisySP or CMSIS-DSP kernel. DaisySP CrossFade is float and per-sample state.
// This fixed-point convex sum cannot boost correlated input or overflow.
inline int16_t Mix(int16_t tail, int16_t head, uint32_t weight) {
    return static_cast<int16_t>(
        (int32_t(tail) * int32_t(32768 - weight) + int32_t(head) * int32_t(weight)) >> 15);
}
inline uint32_t RampStep(uint32_t frames) {
    return frames >= 2 ? (1u << 24) / (frames - 1) : 0;
}
inline uint32_t Weight(uint32_t offset, uint32_t frames, uint32_t step) {
    return offset + 1 >= frames ? 32768 : (offset * step) >> 9;
}
// Converted interleaved foreground block, before gain, fades and resampling.
// Absolute source frames make split SD chunks produce exactly the same PCM.
inline void ApplyBlock(int16_t* pcm,
                       uint32_t frames,
                       uint32_t first,
                       const int16_t* head,
                       uint32_t end,
                       uint32_t overlap,
                       uint8_t channels,
                       uint32_t step) {
    if (!overlap || first >= end || uint64_t(first) + frames <= end - overlap)
        return;
    const uint32_t begin = end - overlap;
    const uint32_t from = first < begin ? begin - first : 0;
    const uint32_t count = std::min(frames, end - first);
    for (uint32_t i = from; i < count; ++i) {
        const uint32_t offset = first + i - begin;
        const auto weight = Weight(offset, overlap, step);
        for (uint8_t c = 0; c < channels; ++c)
            pcm[i * channels + c] = Mix(pcm[i * channels + c], head[offset * channels + c], weight);
    }
}
inline Protocol::SampleEditMessage Edit(const Protocol::SampleMetadata& m) {
    Protocol::SampleEditMessage e(m.sample_id,
                                  m.loop_enabled,
                                  m.gain_db_x10,
                                  m.start_frame,
                                  m.end_frame,
                                  m.loop_start,
                                  m.loop_end,
                                  m.fade_in_ms,
                                  m.fade_out_ms);
    e.loop_crossfade_ms = m.loop_crossfade_ms;
    e.channel_mode = m.channel_mode;
    return e;
}
inline bool Matches(const Protocol::SampleMetadata& m, const Protocol::SampleSeamRequest& r) {
    const auto e = Edit(m);
    return m.generation == r.generation && std::memcmp(&e, &r.expected, sizeof(e)) == 0;
}
inline uint32_t Marker(const Protocol::SampleMetadata& m, uint8_t marker) {
    switch (marker) {
        case Protocol::SAMPLE_MARK_START:
            return m.start_frame;
        case Protocol::SAMPLE_MARK_END:
            return m.end_frame;
        case Protocol::SAMPLE_MARK_LOOP_START:
            return m.loop_start;
        default:
            return m.loop_end;
    }
}
inline void SetMarker(Protocol::SampleMetadata& m, uint8_t marker, uint32_t frame) {
    switch (marker) {
        case Protocol::SAMPLE_MARK_START:
            m.start_frame = frame;
            break;
        case Protocol::SAMPLE_MARK_END:
            m.end_frame = frame;
            break;
        case Protocol::SAMPLE_MARK_LOOP_START:
            m.loop_start = frame;
            break;
        default:
            m.loop_end = frame;
            break;
    }
}
inline int32_t Abs(int32_t v) {
    return v < 0 ? -v : v;
}
// Foreground only, <=4097 candidates in native interleaved PCM16. A candidate
// must cross zero in EVERY native channel (zero endpoints count). Otherwise
// leave the marker alone: cancellation in a mono sum is not a stereo crossing.
// Minimize the worst channel's cut amplitude, then distance, then earlier frame.
inline bool Snap(const int16_t* pcm,
                 const Protocol::SampleMetadata& m,
                 uint8_t marker,
                 uint16_t radius,
                 uint32_t& result) {
    if (!pcm || !m.total_frames || (m.channels != 1 && m.channels != 2) ||
        marker > Protocol::SAMPLE_MARK_LOOP_END || radius > 2048)
        return false;
    const uint32_t original = Marker(m, marker);
    uint32_t low = 1, high = m.total_frames - 1;
    switch (marker) {
        case Protocol::SAMPLE_MARK_START:
            high = std::min(high, m.loop_enabled ? m.loop_start : m.end_frame - 1);
            break;
        case Protocol::SAMPLE_MARK_END:
            low = std::max(low, m.loop_enabled ? m.loop_end : m.start_frame + 1);
            break;
        case Protocol::SAMPLE_MARK_LOOP_START:
            low = std::max(low, m.start_frame);
            if (m.loop_end < kMinLoopFrames)
                return false;
            high = std::min(high, m.loop_end - kMinLoopFrames);
            break;
        case Protocol::SAMPLE_MARK_LOOP_END:
            low = std::max(low, m.loop_start + kMinLoopFrames);
            high = std::min(high, m.end_frame);
            break;
    }
    low = std::max(low, original > radius ? original - radius : 0);
    high = std::min<uint64_t>(high, uint64_t(original) + radius);
    bool found = false;
    int32_t best = INT32_MAX;
    uint32_t distance = UINT32_MAX;
    for (uint32_t f = low; f <= high; ++f) {
        int32_t score = 0;
        bool crossing = true;
        for (uint8_t c = 0; c < m.channels; ++c) {
            const int32_t a = pcm[(f - 1) * m.channels + c], b = pcm[f * m.channels + c];
            crossing &= a == 0 || b == 0 || (a < 0) != (b < 0);
            score = std::max(score, std::max(Abs(a), Abs(b)));
        }
        const uint32_t d = f > original ? f - original : original - f;
        if (crossing && (!found || score < best || (score == best && d < distance))) {
            found = true;
            best = score;
            distance = d;
            result = f;
        }
    }
    return found;
}
inline void Measure(const int16_t* pcm,
                    const Protocol::SampleMetadata& m,
                    Protocol::SampleSeamStatus& out) {
    if (!pcm || m.loop_end <= m.loop_start || m.loop_end > m.total_frames)
        return;
    const uint32_t n =
        m.loop_enabled
            ? CrossfadeFrames(m.loop_crossfade_ms, m.sample_rate, m.loop_end - m.loop_start)
            : 0;
    int32_t raw[2]{}, effective[2]{};
    for (uint8_t c = 0; c < m.channels && c < 2; ++c) {
        raw[c] =
            int32_t(pcm[m.loop_start * m.channels + c]) - pcm[(m.loop_end - 1) * m.channels + c];
        effective[c] = n ? int32_t(pcm[(m.loop_start + n) * m.channels + c]) -
                               pcm[(m.loop_start + n - 1) * m.channels + c]
                         : raw[c];
    }
    out.raw_left = raw[0];
    out.raw_right = raw[m.channels == 2 ? 1 : 0];
    out.left = effective[0];
    out.right = effective[m.channels == 2 ? 1 : 0];
}
}  // namespace WaveX::SampleLoop
