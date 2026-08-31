#pragma once

// HAL-free resident-sample admission and geometry extraction. OnSampleLoad()
// owns FatFS/SDRAM operations; this helper owns the validation policy and is
// host-tested so malformed files are rejected before eviction or allocation.

#include "spi_protocol/protocol.h"

#include "wav/resident_sample_policy.hpp"
#include "wav/wav_header_parser.hpp"
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

struct ResidentSampleInfo {
    uint16_t sample_id = 0;
    uint32_t data_size = 0;
    uint32_t sample_rate = 0;
    uint32_t total_frames = 0;
    uint8_t channels = 0;
    uint8_t bit_depth = 0;
};

inline bool ValidatePcmWavPayload(const WaveX::Wav::WavInfo& wav,
                                  uint32_t file_size,
                                  uint32_t max_data_size) {
    if (wav.audio_format != 1 || (wav.bits_per_sample != 16 && wav.bits_per_sample != 24) ||
        (wav.num_channels != 1 && wav.num_channels != 2) || wav.sample_rate == 0 ||
        wav.data_size == 0 || wav.data_size > max_data_size || wav.data_offset > file_size ||
        wav.data_size > file_size - wav.data_offset) {
        return false;
    }

    const uint32_t bytes_per_frame =
        (static_cast<uint32_t>(wav.bits_per_sample) / 8u) * wav.num_channels;
    return bytes_per_frame != 0 && (wav.data_size % bytes_per_frame) == 0;
}

inline bool BuildResidentSampleInfo(const WaveX::Protocol::SampleLoadMessage& request,
                                    const WaveX::Wav::WavInfo& wav,
                                    uint32_t file_size,
                                    uint32_t arena_capacity,
                                    ResidentSampleInfo& out) {
    out = ResidentSampleInfo{};
    if (!ValidatePcmWavPayload(wav, file_size, arena_capacity)) {
        return false;
    }
    if (!WaveX::Wav::IsResidentSampleFormatSupported(wav.bits_per_sample, wav.num_channels)) {
        return false;
    }

    const uint32_t bytes_per_frame =
        (static_cast<uint32_t>(wav.bits_per_sample) / 8u) * wav.num_channels;
    out.sample_id = request.sample_id;
    out.data_size = wav.data_size;
    // The wire fields are explicitly optional hints. The file parsed by the
    // Daisy is authoritative (and can carry rates such as 96 kHz that do not
    // fit SampleLoadMessage::sample_rate).
    out.sample_rate = wav.sample_rate;
    out.channels = static_cast<uint8_t>(wav.num_channels);
    out.bit_depth = static_cast<uint8_t>(wav.bits_per_sample);
    out.total_frames = out.data_size / bytes_per_frame;
    return true;
}

}  // namespace AudioEngine
}  // namespace WaveX
