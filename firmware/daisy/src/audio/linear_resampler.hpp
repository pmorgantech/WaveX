#pragma once

// Linear sample-rate conversion for streamed WAV playback, extracted from
// audio_engine.cpp so it can be host-tested (it had no coverage while being
// the subject of three separate playback-stall fixes in 2026-08).
//
// HAL-free and allocation-free: plain int16_t arithmetic, caller-provided
// buffers, no daisy_seed.h and no CMSIS include - same arrangement as
// voice_manager.hpp / output_sink.hpp, so tests compile it directly.
//
// The per-sample math is bit-identical to the CMSIS-DSP
// arm_linear_interp_q15() call this replaces: a 20-bit fraction, the
// y0*(0xFFFFF-fract) + y1*fract product accumulated in 64 bits, shifted back
// down by 20. What is NOT carried over is that function's packed 12.20 index
// word, whose sign bit capped a usable table at 2047 entries - past that the
// index went negative and it silently returned the FIRST sample for every
// remaining position (a DC level, no error). Today's callers bound chunks
// well under that by ring-buffer capacity, so this is a latent limit being
// retired, not a live bug being fixed. Index and fraction are split into
// separate 32-bit values here (detail::SplitPosition), so source length is
// limited only by the buffers.
//
// Channel handling: ResampleChannel() is the single-channel core and reads
// through an arbitrary stride, so it maps onto one channel of an interleaved
// buffer with no de-interleave copy (the CMSIS version needed a contiguous
// scratch buffer per channel and paid a full copy for it).
// ResampleInterleaved() is the thin multi-channel wrapper over it and is
// channel-count agnostic: mono, stereo, and the Stage B 8-channel voice
// layout all work, and a caller wanting per-channel dispatch can drive
// ResampleChannel() directly.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

namespace detail {

// Splits a position in frames into an integer frame index and a 20-bit
// fraction - the 12.20 layout of the CMSIS routine this file replaced, minus
// its 12-bit index limit. Every step is a 32-bit hardware operation: scaling
// by 2^20 is exact (power of two), and subtracting a float's own integer part
// is exact. The previous version converted position * 2^20 to int64, and a
// float-to-int64 conversion has no Cortex-M7 instruction - it is a libgcc
// call, paid once per output sample of every streamed WAV.
inline void SplitPosition(float position, int32_t& index, int32_t& fract) {
    if (position < 0.0f) {
        // Only the streaming path comes here, and only in (-1, 0): the gap
        // between the carried history frame and the chunk's frame 0.
        // Truncate toward zero and let the arithmetic shift floor, exactly
        // as the 64-bit version did, so the boundary is bit-identical.
        const int32_t fixed = static_cast<int32_t>(position * 1048576.0f);
        index = fixed >> 20;
        fract = fixed & 0xFFFFF;
        return;
    }
    index = static_cast<int32_t>(position);  // truncation is floor for >= 0
    const float remainder = position - static_cast<float>(index);
    fract = static_cast<int32_t>(remainder * 1048576.0f);
}

}  // namespace detail

// Number of output frames a ratio yields for a given input length. Callers
// size their destination with this before committing to a pass; it matches
// exactly what Resample*() will produce for the same arguments.
inline uint32_t LinearResampleOutputFrames(uint32_t src_frames, float ratio) {
    if (ratio <= 0.0f || src_frames < 2) {
        return 0;
    }
    // Upper bound the position walk, mirroring the caller-side allocation.
    const uint32_t max_output_frames =
        static_cast<uint32_t>(std::ceil(static_cast<float>(src_frames - 1) * ratio)) + 1u;
    const float step = 1.0f / ratio;

    // Counted with the same float accumulation the sample loop uses, so the
    // two cannot disagree - every channel must walk identical positions or
    // they would drift apart within a single frame.
    uint32_t output_frames = 0;
    float position = 0.0f;
    while ((position + 1.0f) < static_cast<float>(src_frames) &&
           output_frames < max_output_frames) {
        output_frames++;
        position += step;
    }
    return output_frames;
}

// Resamples ONE channel. `src`/`dst` point at that channel's first sample;
// `src_stride`/`dst_stride` are 1 for contiguous data or the channel count
// for interleaved. Returns frames written.
inline uint32_t ResampleChannel(const int16_t* src,
                                uint32_t src_frames,
                                size_t src_stride,
                                int16_t* dst,
                                size_t dst_stride,
                                float ratio) {
    if (src == nullptr || dst == nullptr || src_stride == 0 || dst_stride == 0) {
        return 0;
    }
    const uint32_t output_frames = LinearResampleOutputFrames(src_frames, ratio);
    if (output_frames == 0) {
        return 0;
    }

    const float step = 1.0f / ratio;
    float position = 0.0f;
    for (uint32_t out = 0; out < output_frames; ++out) {
        // Position is bounded by the count above, so this holds; kept as a
        // hard guard because reading src[index + 1] below depends on it.
        if ((position + 1.0f) >= static_cast<float>(src_frames)) {
            break;
        }

        // 12.20-equivalent split with a full 32-bit index, so long sources
        // cannot overflow it the way the packed CMSIS word did.
        int32_t index, fract;
        detail::SplitPosition(position, index, fract);

        const int16_t y0 = src[static_cast<size_t>(index) * src_stride];
        const int16_t y1 = src[(static_cast<size_t>(index) + 1u) * src_stride];

        int64_t y = static_cast<int64_t>(y0) * (0xFFFFF - fract);
        y += static_cast<int64_t>(y1) * fract;
        dst[static_cast<size_t>(out) * dst_stride] = static_cast<int16_t>(y >> 20);

        position += step;
    }
    return output_frames;
}

// Resamples an interleaved buffer of `channels` channels. Every channel walks
// identical positions, so frames stay aligned. Returns frames written (not
// samples); `dst` must hold output_frames * channels.
inline uint32_t ResampleInterleaved(
    const int16_t* src, uint32_t src_frames, int16_t* dst, uint32_t channels, float ratio) {
    if (src == nullptr || dst == nullptr || channels == 0) {
        return 0;
    }
    uint32_t output_frames = 0;
    for (uint32_t ch = 0; ch < channels; ++ch) {
        output_frames = ResampleChannel(src + ch, src_frames, channels, dst + ch, channels, ratio);
        if (output_frames == 0) {
            return 0;
        }
    }
    return output_frames;
}

// ---------------------------------------------------------------------------
// Streaming resampler
//
// The stateless calls above restart at phase 0 and stop one frame short of the
// end of their input, which is correct for a self-contained buffer and WRONG
// for successive chunks of one continuous stream: each chunk boundary discards
// up to a frame of audio and jumps the fractional phase. At the ~1050-frame
// chunks the WAV pump uses, that is a discontinuity every ~24 ms - an audible
// ~42 Hz warble once dropouts stop masking it.
//
// This carries the phase across calls, and one history frame so the first
// output of a chunk can interpolate against the last input of the previous
// one. The whole chunk is consumed every call; continuity lives in the state,
// not in leftover input.
// ---------------------------------------------------------------------------

constexpr uint32_t kMaxResamplerChannels = 8;

struct StreamResamplerState {
    // Position for the next chunk, relative to that chunk's frame 0. Normally
    // negative (between the history frame and the next chunk's first frame);
    // may be positive when downsampling far enough to skip frames.
    float phase = 0.0f;
    bool has_history = false;
    int16_t history[kMaxResamplerChannels] = {};

    void Reset() {
        phase = 0.0f;
        has_history = false;
    }
};

// Resamples one chunk of a continuous stream. Consumes ALL src_frames; returns
// frames written, capped by dst_capacity_frames.
inline uint32_t ResampleStreamInterleaved(StreamResamplerState& st,
                                          const int16_t* src,
                                          uint32_t src_frames,
                                          int16_t* dst,
                                          uint32_t dst_capacity_frames,
                                          uint32_t channels,
                                          float ratio) {
    if (src == nullptr || dst == nullptr || channels == 0 || channels > kMaxResamplerChannels ||
        ratio <= 0.0f || src_frames == 0) {
        return 0;
    }

    const float step = 1.0f / ratio;
    // A chunk with no history has nothing to interpolate its first output
    // against, so it starts at frame 0 like the stateless path.
    float position = st.has_history ? st.phase : 0.0f;
    const float limit = static_cast<float>(src_frames) - 1.0f;

    uint32_t out = 0;
    while (out < dst_capacity_frames && position < limit) {
        // Strictly less than limit, so index+1 is always within the chunk.
        int32_t index, fract;
        detail::SplitPosition(position, index, fract);

        for (uint32_t ch = 0; ch < channels; ++ch) {
            // index < 0 only ever means -1: the sample carried over from the
            // previous chunk.
            const int16_t y0 =
                (index < 0) ? st.history[ch] : src[static_cast<size_t>(index) * channels + ch];
            const int16_t y1 = src[static_cast<size_t>(index + 1) * channels + ch];
            int64_t y = static_cast<int64_t>(y0) * (0xFFFFF - fract);
            y += static_cast<int64_t>(y1) * fract;
            dst[static_cast<size_t>(out) * channels + ch] = static_cast<int16_t>(y >> 20);
        }
        ++out;
        position += step;
    }

    // Rebase the phase onto the next chunk and keep its last frame, so the
    // boundary is interpolated across rather than jumped over.
    st.phase = position - static_cast<float>(src_frames);
    for (uint32_t ch = 0; ch < channels; ++ch) {
        st.history[ch] = src[static_cast<size_t>(src_frames - 1) * channels + ch];
    }
    st.has_history = true;
    return out;
}

}  // namespace AudioEngine
}  // namespace WaveX
