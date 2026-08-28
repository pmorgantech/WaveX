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
// retired, not a live bug being fixed. Index and fraction are computed in
// int64_t here, so source length is limited only by the buffers.
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

        // 12.20-equivalent split, in 64 bits so long sources cannot overflow
        // the index the way the packed CMSIS word did.
        const int64_t fixed = static_cast<int64_t>(position * 1048576.0);
        const int64_t index = fixed >> 20;
        const int64_t fract = fixed & 0xFFFFF;

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

}  // namespace AudioEngine
}  // namespace WaveX
