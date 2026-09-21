#pragma once
#include "sample_pool.hpp"
#include "wxcf/project_file.hpp"

namespace WaveX::AudioEngine {
// Foreground-only adapters. The caller freezes edits during capture and applies
// records only to its private candidate Pool, before preparing any voice maps.
inline bool CaptureProjectSample(const LoadedSampleInfo& info, Sequencer::ProjectSample& out) {
    const auto& m = info.meta;
    std::memcpy(out.path, info.path, sizeof(out.path));
    out.sample_rate = m.sample_rate;
    out.total_frames = m.total_frames;
    out.start_frame = m.start_frame;
    out.end_frame = m.end_frame;
    out.loop_start = m.loop_start;
    out.loop_end = m.loop_end;
    out.gain_db_x10 = m.gain_db_x10;
    out.fade_in_ms = m.fade_in_ms;
    out.fade_out_ms = m.fade_out_ms;
    out.loop_crossfade_ms = m.loop_crossfade_ms;
    out.channels = m.channels;
    out.bits_per_sample = m.bits_per_sample;
    out.loop_enabled = m.loop_enabled != 0;
    out.channel_mode = m.channel_mode;
    return ProjectFile::detail::Sample(out);
}
inline bool ApplyProjectSample(const Sequencer::ProjectSample& saved, LoadedSampleInfo& info) {
    auto& m = info.meta;
    if (!ProjectFile::detail::Sample(saved) || std::strcmp(saved.path, info.path) != 0 ||
        saved.sample_rate != m.sample_rate || saved.total_frames != m.total_frames ||
        saved.channels != m.channels || saved.bits_per_sample != m.bits_per_sample)
        return false;
    // Preserve runtime identity; changing the channel mapping advances waveform revision.
    m.start_frame = saved.start_frame;
    m.end_frame = saved.end_frame;
    m.loop_start = saved.loop_start;
    m.loop_end = saved.loop_end;
    m.gain_db_x10 = saved.gain_db_x10;
    m.fade_in_ms = saved.fade_in_ms;
    m.fade_out_ms = saved.fade_out_ms;
    m.loop_crossfade_ms = saved.loop_crossfade_ms;
    m.loop_enabled = saved.loop_enabled;
    if (m.channel_mode != saved.channel_mode) {
        m.channel_mode = saved.channel_mode;
        ++m.generation;
    }
    m.Resolve();
    return true;
}
}  // namespace WaveX::AudioEngine
