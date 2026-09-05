// The Daisy's Sample Pool: the shared registry instantiated over the
// engine's per-sample record (docs/features/track-and-patch-model.md §4).
//
// The engine owns the one instance (records in their SDRAM partition, the id
// index in SRAM) and hands it to the SFZ loader, so an import's samples are
// ordinary Pool entries: listable, editable, shared by path between imports
// and with samples the user loaded. Main-loop only.
#pragma once

#include "config/hardware_config.h"
#include "memory.h"
#include "spi_protocol/protocol.h"

#include "audio/sample_registry.hpp"
#include "sample_load_info.hpp"
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

struct LoadedSampleInfo {
    wxsamp_t handle = {};
    uint16_t sample_id = 0;
    uint32_t allocated_bytes = 0;
    uint32_t loaded_bytes = 0;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t bit_depth = 0;
    // The authoritative record. Every playback and display path reads its
    // markers, gain and channel mode from here, so streaming audition, RAM
    // voices and the preview generator cannot disagree about the same sample.
    WaveX::Protocol::SampleMetadata meta = {};
};

using SamplePool = WaveX::Audio::SampleRegistry<LoadedSampleInfo, WAVEX_SAMPLE_POOL_CAPACITY>;

// "Playable" means resident PCM16, mono or stereo - the voice manager reads
// int16 interleaved data directly; the load boundary rejects formats that do
// not satisfy that contract.
inline bool SampleIsPlayable(const LoadedSampleInfo& e) {
    return e.bit_depth == 16 && (e.channels == 1 || e.channels == 2);
}

// Fills a Pool record once its audio is resident. Markers default to the
// whole sample and gain to unity, so an unedited sample behaves as it always
// has; every later change goes through SetEditParams, which re-pushes.
// generation (the frontend's envelope-cache key) starts at 0: a Pool id
// names one file for its whole life, so content never changes under an id.
inline void FillLoadedSample(LoadedSampleInfo& info,
                             uint16_t sample_id,
                             const char* path,
                             const ResidentSampleInfo& resident,
                             const wxsamp_t& handle) {
    info = LoadedSampleInfo{};
    info.sample_id = sample_id;
    info.handle = handle;
    info.allocated_bytes = handle.len ? handle.len : resident.data_size;
    info.loaded_bytes = resident.data_size;
    info.sample_rate = resident.sample_rate;
    info.channels = resident.channels;
    info.bit_depth = resident.bit_depth;

    info.meta = WaveX::Protocol::SampleMetadata();
    info.meta.sample_id = sample_id;
    info.meta.sample_rate = resident.sample_rate;
    info.meta.total_frames = resident.total_frames;
    info.meta.end_frame = info.meta.total_frames;
    info.meta.loop_end = info.meta.total_frames;
    info.meta.channels = resident.channels;
    info.meta.bits_per_sample = resident.bit_depth;
    info.meta.channel_mode = WaveX::Protocol::SAMPLE_CH_AS_RECORDED;
    info.meta.flags = WaveX::Protocol::SAMPLE_META_RESIDENT;
    WaveX::Protocol::detail::CopyWireString(info.meta.name, sizeof(info.meta.name), path);
}

}  // namespace AudioEngine
}  // namespace WaveX
