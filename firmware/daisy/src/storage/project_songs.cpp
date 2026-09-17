#include "project_songs.hpp"

#include "pattern_store.hpp"
#include "wxcf/pattern_file.hpp"
#include <cstdio>
#include <cstring>
namespace WaveX::Storage {
using namespace Protocol;
using namespace Sequencer;
using State = PatternExchange::State;
void ProjectSongs::Refresh(const Project* project) {
    status_.selected_song = project ? project->selected_song : kNoSong;
    status_.active_pattern = project ? project->active_pattern : 0;
    const Song* song = project ? &project->songs[status_.song] : nullptr;
    status_.used = song && song->used;
    status_.length = status_.used ? static_cast<uint8_t>(song->length) : 0;
    status_.tempo_bpm_x100 = status_.used ? song->tempo_bpm_x100 : 12000;
    detail::CopyWireString(status_.name, sizeof(status_.name), status_.used ? song->name : "");
    for (uint16_t i = 0; i < kMaxSongEntries; ++i) {
        status_.entries[i].pattern = i < status_.length ? song->entries[i].pattern : 0;
        status_.entries[i].repeats = i < status_.length ? song->entries[i].repeats : 1;
    }
    status_.playing_song = 0xff;
    if (phase_ == Phase::Playing || phase_ == Phase::Start) {
        const auto position = exchange_.SongPosition();
        status_.active_pattern = static_cast<uint8_t>(position & 0x7f);
        status_.playing_entry = static_cast<uint8_t>((position >> 8) & 0x7f);
        status_.playing_repeat = static_cast<uint8_t>(position >> 16);
        if (position & (1u << 24))
            status_.playing_song = playback_song_;
    }
}
bool ProjectSongs::Request(const SeqSongOpMessage& request, const Project* project, bool busy) {
    if (!IsValidSeqSongOp(request))
        return false;
    status_.request_id = request.request_id;
    status_.song = request.song;
    reply_ = true;
    Refresh(project);
    if (request.op == SEQ_SONG_GET || request.request_id == status_.active_request_id ||
        request.request_id == status_.completed_request_id)
        return false;
    if (request.op == SEQ_SONG_STOP && phase_ == Phase::Playing) {
        request_ = request;
        status_.active_request_id = request.request_id;
        exchange_.StopSong();
        return true;
    }
    uint8_t error = SEQ_SONG_OK;
    if (Busy() || busy || exchange_.state() != State::Idle)
        error = SEQ_SONG_BUSY;
    else if ((request.op == SEQ_SONG_CREATE || request.op == SEQ_SONG_RENAME) &&
             !PatternFile::ValidName(request.name))
        error = SEQ_SONG_BAD_NAME;
    else if (request.op == SEQ_SONG_CREATE && status_.used)
        error = SEQ_SONG_EXISTS;
    else if (request.op != SEQ_SONG_CREATE && request.op != SEQ_SONG_STOP && !status_.used)
        error = SEQ_SONG_EMPTY;
    else if (request.op == SEQ_SONG_INSERT || request.op == SEQ_SONG_SET_ENTRY) {
        if (!project->patterns[request.pattern].used ||
            (request.op == SEQ_SONG_INSERT ? status_.length == 128 || request.entry > status_.length
                                           : request.entry >= status_.length))
            error = SEQ_SONG_BAD_ENTRY;
    } else if (request.op == SEQ_SONG_REMOVE &&
               (status_.length <= 1 || request.entry >= status_.length))
        error = SEQ_SONG_BAD_ENTRY;
    else if (request.op == SEQ_SONG_MOVE &&
             (request.entry >= status_.length || request.destination >= status_.length))
        error = SEQ_SONG_BAD_ENTRY;
    else if (request.op == SEQ_SONG_PLAY && request.entry >= status_.length)
        error = SEQ_SONG_BAD_ENTRY;
    if (error != SEQ_SONG_OK || request.op == SEQ_SONG_STOP) {
        status_.completed_request_id = request.request_id;
        status_.completed_op = request.op;
        status_.error = error;
        return false;
    }
    request_ = request;
    status_.busy = 1;
    status_.active_request_id = request.request_id;
    status_.error = SEQ_SONG_OK;
    phase_ = Phase::Capture;
    exchange_.Capture(true);
    return true;
}
void ProjectSongs::Finish(uint8_t error) {
    exchange_.Retire();
    phase_ = Phase::Idle;
    status_.busy = 0;
    status_.active_request_id = 0;
    status_.completed_request_id = request_.request_id;
    status_.completed_op = request_.op;
    status_.error = error;
    status_.playing_song = 0xff;
    reply_ = true;
}
void ProjectSongs::Pump(Project* project) {
    if (!Busy())
        return;
    if (phase_ == Phase::Start || phase_ == Phase::Playing) {
        if (exchange_.state() == State::Running) {
            Finish(SEQ_SONG_STOP_FIRST);
            return;
        }
        if (exchange_.state() == State::SongPlaying && phase_ == Phase::Start) {
            phase_ = Phase::Playing;
            status_.completed_request_id = request_.request_id;
            status_.completed_op = SEQ_SONG_PLAY;
            reply_ = true;
        }
        if (exchange_.state() == State::SongEnded) {
            project->active_pattern = static_cast<uint8_t>(exchange_.SongPosition() & 0x7f);
            PatternStore::SetProjectPatternName(project->patterns[project->active_pattern].name);
            Finish(SEQ_SONG_OK);
        }
        Refresh(project);
        return;
    }
    if (exchange_.state() == State::Running) {
        Finish(SEQ_SONG_STOP_FIRST);
        return;
    }
    if (exchange_.state() == State::Failed) {
        Finish(SEQ_SONG_CAPTURE_BUSY);
        return;
    }
    if (exchange_.state() != State::Captured)
        return;
    if (!project) {
        Finish(SEQ_SONG_NO_MEMORY);
        return;
    }
    auto& active = project->patterns[project->active_pattern];
    active.used = true;
    active.pattern = exchange_.foreground();
    const auto* name = PatternStore::CurrentName();
    if (name[0])
        detail::CopyWireString(active.name, sizeof(active.name), name);
    if (!active.name[0])
        std::snprintf(active.name, sizeof(active.name), "Pattern %u", project->active_pattern + 1);
    exchange_.Retire();
    auto& song = project->songs[request_.song];
    const auto entry = request_.entry;
    switch (request_.op) {
        case SEQ_SONG_CREATE:
            song = Song{};
            song.used = true;
            song.length = 1;
            song.entries[0].pattern = project->active_pattern;
            song.tempo_bpm_x100 = exchange_.capturedSettings().tempo_bpm_x100;
            detail::CopyWireString(song.name, sizeof(song.name), request_.name);
            break;
        case SEQ_SONG_RENAME:
            detail::CopyWireString(song.name, sizeof(song.name), request_.name);
            break;
        case SEQ_SONG_INSERT:
            for (uint16_t i = song.length; i > entry; --i)
                song.entries[i] = song.entries[i - 1];
            ++song.length;
            [[fallthrough]];
        case SEQ_SONG_SET_ENTRY:
            song.entries[entry] = {request_.pattern, request_.repeats};
            break;
        case SEQ_SONG_REMOVE:
            for (uint16_t i = entry + 1; i < song.length; ++i)
                song.entries[i - 1] = song.entries[i];
            song.entries[--song.length] = {};
            break;
        case SEQ_SONG_MOVE: {
            const auto value = song.entries[entry];
            if (entry < request_.destination)
                for (uint16_t i = entry; i < request_.destination; ++i)
                    song.entries[i] = song.entries[i + 1];
            else
                for (uint16_t i = entry; i > request_.destination; --i)
                    song.entries[i] = song.entries[i - 1];
            song.entries[request_.destination] = value;
            break;
        }
        case SEQ_SONG_TEMPO:
            song.tempo_bpm_x100 = request_.tempo_bpm_x100;
            break;
        default:
            break;
    }
    project->selected_song = request_.song;
    if (request_.op == SEQ_SONG_PLAY) {
        playback_song_ = request_.song;
        status_.loop = request_.loop;
        exchange_.PlaySong(project, request_.song, request_.entry, request_.loop);
        phase_ = Phase::Start;
    } else
        Finish(SEQ_SONG_OK);
    Refresh(project);
}
}  // namespace WaveX::Storage
