#pragma once

#include "spi_protocol/protocol.h"

#include "audio/track_mix.hpp"
#include "sequencer/pattern_data.hpp"

namespace WaveX {
namespace Sequencer {
constexpr uint8_t kMaxPatterns = 128;
constexpr uint8_t kMaxSongs = 16;
constexpr uint16_t kMaxSongEntries = 128;
constexpr uint8_t kNoSong = 0xff;

struct ProjectTrack {
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
// A file transaction's private workspace, NOT an audio-domain object or an
// always-resident device global. Device adapters must reserve foreground
// scratch explicitly; never instantiate this (~3 MiB) on either MCU's stack.
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
    ProjectTrack tracks[kMaxTracks]{};
    ProjectPattern patterns[kMaxPatterns]{};
    Song songs[kMaxSongs]{};
};
}  // namespace Sequencer
}  // namespace WaveX
