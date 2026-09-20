#pragma once

#include "spi_protocol/protocol.h"

#include "audio/note_policy.hpp"
#include "audio/track_mix.hpp"
#include "sequencer/pattern_data.hpp"

namespace WaveX {
namespace Sequencer {
constexpr uint8_t kMaxPatterns = 128;
constexpr uint8_t kMaxSongs = 16;
constexpr uint16_t kMaxSongEntries = 128;
constexpr uint8_t kNoSong = 0xff;

// Saved edits are keyed by full card path, never a boot-local Pool id. File
// dimensions detect replaced dependencies before applying absolute markers.
constexpr uint16_t kMaxProjectSamples = 1024;
struct ProjectSample {
    char path[Protocol::BROWSE_PATH_MAX]{};
    uint32_t sample_rate = 0, total_frames = 0;
    uint32_t start_frame = 0, end_frame = 0, loop_start = 0, loop_end = 0;
    int16_t gain_db_x10 = 0;
    uint16_t fade_in_ms = Protocol::kDefaultDeclickMs;
    uint16_t fade_out_ms = Protocol::kDefaultDeclickMs;
    uint8_t channels = 0, bits_per_sample = 0;
    bool loop_enabled = false;
    uint8_t channel_mode = Protocol::SAMPLE_CH_AS_RECORDED;
};
struct ProjectTrack {
    Allocation::Override allocation;
    char instrument_path[Protocol::BROWSE_PATH_MAX]{};
    uint8_t midi_in = Protocol::TRACK_MIDI_IN_OMNI;
    uint8_t poly_limit = 0, priority = 0;
    bool program_change = true;
    Mix::TrackMix mix;
};
struct SongEntry {
    uint8_t pattern = 0;
    uint8_t repeats = 1;
};
struct Song {
    bool used = false;
    char name[24]{};
    uint16_t tempo_bpm_x100 = 12000;
    uint16_t length = 0;
    SongEntry entries[kMaxSongEntries]{};
};
struct ProjectPattern {
    bool used = false;
    char name[24]{};
    Pattern pattern;
};
// Foreground document, NOT an audio-domain object. The session owner reserves
// retained and candidate documents through the sample allocator; never
// instantiate this (~3.5 MiB) on either MCU's stack.
// Playback retains one prepared Pattern. Song execution is a separate owner.
struct Project {
    char name[24]{};
    char bank_path[Protocol::BROWSE_PATH_MAX]{};
    uint16_t tempo_bpm_x100 = 12000;
    uint8_t active_pattern = 0;
    uint8_t selected_song = kNoSong;
    uint8_t clock_source = Protocol::SEQ_CLOCK_INTERNAL;
    uint8_t input_mode = Protocol::SEQ_INPUT_PLAY;
    bool quantize = false;
    float master_gain = 1;
    bool sample_edits_present = true;  // decoder provenance; not a native-layout file field
    uint16_t sample_count = 0;
    ProjectSample samples[kMaxProjectSamples]{};
    ProjectTrack tracks[kMaxTracks]{};
    ProjectPattern patterns[kMaxPatterns]{};
    Song songs[kMaxSongs]{};
};
}  // namespace Sequencer
}  // namespace WaveX
