#include "project_session.hpp"

#include "audio/project_sample_edits.hpp"
#include "bss_static.hpp"
#include "card_space.hpp"
#include "pattern_store.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace WaveX::Storage {
using namespace Protocol;
using namespace AudioEngine;
using Sequencer::Project;
using Exchange = Sequencer::PatternExchange;
ProjectSession::ProjectSession(SampleMemMgr& memory,
                               SamplePool& pool,
                               Exchange& exchange,
                               MixerControlHandoff& mixer,
                               uint8_t* io,
                               uint32_t bytes,
                               Boundary boundary)
    : memory_(memory),
      pool_(pool),
      exchange_(exchange),
      mixer_(mixer),
      io_(io),
      io_bytes_(bytes),
      boundary_(boundary) {}
ProjectSession::~ProjectSession() {
    file_.Cancel();
    ReleaseScratch();
    if (current_)
        current_->~Project();
    memory_.release(&current_mem_);
}
bool ProjectSession::Allocate(uint32_t bytes, wxsamp_t& handle, void** out) {
    if (memory_.alloc(bytes, &handle) && memory_.ptr(handle, out))
        return true;
    memory_.release(&handle);
    return false;
}
bool ProjectSession::ReadBank(void* ctx, void* out, size_t bytes) {
    auto& self = *static_cast<ProjectSession*>(ctx);
    UINT read = 0;
    return bytes < 512 && f_read(&self.bank_file_, out, static_cast<UINT>(bytes), &read) == FR_OK &&
           read == bytes;
}
bool ProjectSession::BankEof(void* ctx) {
    return f_eof(&static_cast<ProjectSession*>(ctx)->bank_file_);
}
void ProjectSession::ReleaseScratch() {
    bank_decoder_.reset();
    if (bank_open_) {
        f_close(&bank_file_);
        bank_open_ = false;
    }
    if (candidate_) {
        if (SfzLoader::ProjectLoadActive()) {
            SfzLoader::CancelProjectTrack(candidate_->pool, memory_);
            SfzLoader::FinishProjectLoad(false);
        }
        pool_stage_.reset();  // releases only candidate-owned PCM on abort
        candidate_->~Candidate();
        candidate_ = nullptr;
    }
    memory_.release(&candidate_mem_);
    if (scratch_) {
        scratch_->~Project();
        scratch_ = nullptr;
    }
    memory_.release(&scratch_mem_);
}
bool ProjectSession::Request(const ProjectOpMessage& request, bool external_busy) {
    if (!IsValidProjectOp(request))
        return false;
    status_.request_id = request.request_id;
    reply_ = true;
    if (request.op == PROJECT_GET || request.request_id == status_.active_request_id ||
        request.request_id == status_.completed_request_id)
        return false;
    if (Busy() || external_busy || exchange_.state() != Exchange::State::Idle) {
        status_.completed_request_id = request.request_id;
        status_.completed_op = request.op;
        status_.error = PROJECT_BUSY;
        return false;
    }
    request_ = request;
    if (request.op == PROJECT_NEW)
        request_.name[0] = 0;
    if (request.op != PROJECT_NEW && !PatternFile::ValidName(request.name)) {
        status_.completed_request_id = request.request_id;
        status_.completed_op = request.op;
        status_.error = PROJECT_BAD_NAME;
        return false;
    }
    status_.busy = 1;
    status_.active_request_id = request.request_id;
    status_.active_op = request.op;
    status_.progress = 0;
    status_.failed_track = 0xff;
    status_.error = PROJECT_OK;
    owned_directory_ = paused_ = track_started_ = false;
    owned_snapshots_ = sample_ = track_ = 0;
    phase_ = Phase::Allocate;
    return true;
}
uint8_t ProjectSession::FileError() const {
    switch (file_.Status()) {
        case ProjectFileJob::Result::NoSpace:
            return PROJECT_NO_SPACE;
        case ProjectFileJob::Result::Exists:
            return PROJECT_EXISTS;
        case ProjectFileJob::Result::NotFound:
            return PROJECT_NOT_FOUND;
        case ProjectFileJob::Result::BadName:
            return PROJECT_BAD_NAME;
        case ProjectFileJob::Result::Invalid:
            return PROJECT_BAD_FILE;
        default:
            return PROJECT_IO;
    }
}
uint8_t ProjectSession::InstrumentError(uint8_t error) const {
    switch (error) {
        case INST_ERROR_NO_MEMORY:
            return PROJECT_NO_MEMORY;
        case INST_ERROR_NO_SPACE:
            return PROJECT_NO_SPACE;
        case INST_ERROR_IO:
            return PROJECT_IO;
        case INST_ERROR_EXISTS:
            return PROJECT_EXISTS;
        default:
            return PROJECT_DEPENDENCY;
    }
}
void ProjectSession::SnapshotPath(uint8_t track, char* out, size_t size) const {
    std::snprintf(out, size, "%s/track%02u.wxi", directory_, track + 1);
}
void ProjectSession::Finish(uint8_t error) {
    result_ = error;
    file_.Cancel();
    ReleaseScratch();
    exchange_.Retire();
    if (paused_)
        boundary_.publish();  // old session on failure, installed session on success
    paused_ = false;
    phase_ = Phase::Cleanup;
    track_ = 0;
}
void ProjectSession::Complete() {
    status_.busy = 0;
    status_.active_request_id = 0;
    status_.active_op = PROJECT_GET;
    status_.completed_request_id = request_.request_id;
    status_.completed_op = request_.op;
    status_.error = result_;
    if (result_ == PROJECT_OK) {
        Protocol::detail::CopyWireString(status_.name, sizeof(status_.name), request_.name);
        status_.progress = 100;
    }
    reply_ = true;
    phase_ = Phase::Idle;
}
void ProjectSession::Promote() {
    if (current_)
        current_->~Project();
    memory_.release(&current_mem_);
    current_ = scratch_;
    current_mem_ = scratch_mem_;
    scratch_ = nullptr;
    scratch_mem_ = {};
}
void ProjectSession::CaptureSession() {
    const auto& settings = exchange_.capturedSettings();
    auto& slot = scratch_->patterns[scratch_->active_pattern];
    slot.used = true;
    slot.pattern = exchange_.foreground();
    const char* name = PatternStore::CurrentName();
    Protocol::detail::CopyWireString(slot.name, sizeof(slot.name), name[0] ? name : "Pattern 1");
    scratch_->tempo_bpm_x100 = settings.tempo_bpm_x100;
    scratch_->clock_source = settings.clock_source;
    scratch_->input_mode = settings.input_mode;
    scratch_->quantize = settings.quantize != 0;
    exchange_.Retire();
    Protocol::detail::CopyWireString(scratch_->name, sizeof(scratch_->name), request_.name);
    const auto& mix = mixer_.Pending();
    scratch_->master_gain = mix.master_gain;
    std::snprintf(directory_, sizeof(directory_), "0:/wavex/projects/%s", request_.name);
    for (uint8_t t = 0; t < kNumTracks; ++t) {
        auto& target = scratch_->tracks[t];
        target.midi_in = SfzLoader::TrackMidiIn(t);
        target.poly_limit = SfzLoader::TrackPolyLimit(t);
        target.priority = SfzLoader::TrackPriority(t);
        target.program_change = SfzLoader::TrackProgramChange(t);
        target.mix = mix.tracks[t];
        target.instrument_path[0] = 0;
        if (SfzLoader::TrackLoaded(t))
            SnapshotPath(t, target.instrument_path, sizeof(target.instrument_path));
    }
    scratch_->sample_edits_present = true;
    scratch_->sample_count = 0;
    bool valid = true;
    pool_.ForEach([&](const SamplePool::Record& record) {
        if (!record.used_by || !valid)
            return;
        if (scratch_->sample_count == Sequencer::kMaxProjectSamples ||
            !CaptureProjectSample(record.payload, scratch_->samples[scratch_->sample_count])) {
            valid = false;
            return;
        }
        ++scratch_->sample_count;
    });
    if (!valid)
        Finish(PROJECT_DEPENDENCY);
    else
        phase_ = Phase::Assets;
}
void ProjectSession::Pump() {
    if (!Busy())
        return;
    switch (phase_) {
        case Phase::Idle:
            break;
        case Phase::Allocate: {
            void* bytes = nullptr;
            if (!Allocate(sizeof(Project), scratch_mem_, &bytes)) {
                Finish(PROJECT_NO_MEMORY);
                break;
            }
            scratch_ = new (bytes) Project();  // foreground placement construction; no stack copy
            if (request_.op == PROJECT_SAVE_COPY) {
                clone_offset_ = 0;
                phase_ = Phase::Clone;
            } else {
                if (!exchange_.Pause()) {
                    Finish(PROJECT_CAPTURE_BUSY);
                    break;
                }
                phase_ = Phase::Pause;
            }
            break;
        }
        case Phase::Clone: {
            static_assert(std::is_trivially_copyable<Project>::value, "bounded Project copy");
            if (current_ && clone_offset_ < sizeof(Project)) {
                const auto n = std::min<size_t>(4096, sizeof(Project) - clone_offset_);
                std::memcpy(reinterpret_cast<uint8_t*>(scratch_) + clone_offset_,
                            reinterpret_cast<const uint8_t*>(current_) + clone_offset_,
                            n);
                clone_offset_ += n;
                break;
            }
            if (!exchange_.Capture())
                Finish(PROJECT_CAPTURE_BUSY);
            else
                phase_ = Phase::Capture;
            break;
        }
        case Phase::Capture:
            if (exchange_.state() == Exchange::State::Captured)
                CaptureSession();
            else if (exchange_.state() == Exchange::State::Failed)
                Finish(PROJECT_CAPTURE_BUSY);
            break;
        case Phase::Assets: {
            const auto space = CheckSaveSpace(0);
            if (space != SaveSpace::Ready) {
                Finish(space == SaveSpace::Full ? PROJECT_NO_SPACE : PROJECT_IO);
                break;
            }
            for (const char* parent: {"0:/wavex", "0:/wavex/projects"}) {
                const auto r = f_mkdir(parent);
                if (r != FR_OK && r != FR_EXIST) {
                    Finish(PROJECT_IO);
                    return;
                }
            }
            char destination[112];
            std::snprintf(destination, sizeof(destination), "%s.wxp", directory_);
            FILINFO info{};
            const auto existing = f_stat(destination, &info);
            if (existing != FR_NO_FILE) {
                Finish(existing == FR_OK ? PROJECT_EXISTS : PROJECT_IO);
                break;
            }
            const auto created = f_mkdir(directory_);
            if (created != FR_OK) {
                Finish(created == FR_EXIST ? PROJECT_EXISTS : PROJECT_IO);
                break;
            }
            owned_directory_ = true;
            track_ = 0;
            phase_ = Phase::Snapshot;
            break;
        }
        case Phase::Snapshot:
            if (track_started_) {
                SfzLoader::Pump(pool_, memory_, io_, io_bytes_);
                if (SfzLoader::Busy())
                    break;
                track_started_ = false;
                if (SfzLoader::ProjectSnapshotError() != INST_ERROR_NONE) {
                    status_.failed_track = track_;
                    Finish(InstrumentError(SfzLoader::ProjectSnapshotError()));
                    break;
                }
                owned_snapshots_ |= static_cast<uint16_t>(1u << track_);
                ++track_;
            }
            while (track_ < kNumTracks && !scratch_->tracks[track_].instrument_path[0])
                ++track_;
            status_.progress = static_cast<uint8_t>(5 + track_ * 3);
            if (track_ == kNumTracks) {
                if (!file_.SaveCopy(*scratch_, request_.request_id))
                    Finish(FileError());
                else
                    phase_ = Phase::Save;
            } else if (!SfzLoader::BeginProjectSnapshot(track_,
                                                        scratch_->tracks[track_].instrument_path)) {
                status_.failed_track = track_;
                Finish(InstrumentError(SfzLoader::ProjectSnapshotError()));
            } else
                track_started_ = true;
            break;
        case Phase::Save:
            file_.Pump();
            if (file_.Busy())
                break;
            if (file_.Status() != ProjectFileJob::Result::Saved) {
                Finish(FileError());
                break;
            }
            owned_directory_ = false;
            owned_snapshots_ = 0;  // published .wxp now owns its Instrument copies
            Promote();
            Finish(PROJECT_OK);
            break;
        case Phase::Pause:
            if (exchange_.state() != Exchange::State::Paused)
                break;
            exchange_.Retire();
            paused_ = true;
            if (!boundary_.stop_voices()) {
                Finish(PROJECT_AUDIO_BUSY);
                break;
            }
            if (request_.op == PROJECT_LOAD) {
                if (!file_.Load(request_.name, *scratch_))
                    Finish(FileError());
                else
                    phase_ = Phase::Read;
            } else {
                std::strcpy(scratch_->name, "Untitled");
                scratch_->patterns[0].used = true;
                std::strcpy(scratch_->patterns[0].name, "Pattern 1");
                for (uint8_t t = 0; t < kNumTracks; ++t)
                    scratch_->tracks[t].midi_in = t + 1;
                phase_ = Phase::Stage;
            }
            break;
        case Phase::Read:
            file_.Pump();
            if (file_.Busy())
                break;
            if (file_.Status() != ProjectFileJob::Result::Loaded)
                Finish(FileError());
            else
                phase_ = Phase::Stage;
            break;
        case Phase::Stage: {
            if (scratch_->bank_path[0]) {
                FILINFO info{};
                if (f_stat(scratch_->bank_path, &info) != FR_OK || (info.fattrib & AM_DIR)) {
                    Finish(PROJECT_DEPENDENCY);
                    break;
                }
            }
            void* bytes = nullptr;
            if (!Allocate(sizeof(Candidate), candidate_mem_, &bytes)) {
                Finish(PROJECT_NO_MEMORY);
                break;
            }
            candidate_ = new (bytes) Candidate();
            pool_stage_.emplace(pool_, candidate_->pool, memory_);
            if (!pool_stage_->Begin() || !SfzLoader::BeginProjectLoad(candidate_->tracks)) {
                Finish(PROJECT_BUSY);
                break;
            }
            track_ = 0;
            phase_ = Phase::LoadTrack;
            if (scratch_->bank_path[0]) {
                if (f_open(&bank_file_, scratch_->bank_path, FA_READ) != FR_OK) {
                    Finish(PROJECT_DEPENDENCY);
                    break;
                }
                bank_open_ = true;
                bank_decoder_.emplace(Wxcf::IoContext{this, ReadBank, nullptr, BankEof},
                                      candidate_->bank);
                phase_ = Phase::Bank;
            }
            break;
        }
        case Phase::Bank: {
            auto result = BankFile::Result::More;
            for (unsigned i = 0; i < 8 && result == BankFile::Result::More; ++i)
                result = bank_decoder_->Advance();
            if (result == BankFile::Result::More)
                break;
            bank_decoder_.reset();
            const auto closed = f_close(&bank_file_);
            bank_open_ = false;
            if (closed != FR_OK || result != BankFile::Result::Done)
                Finish(PROJECT_DEPENDENCY);
            else
                phase_ = Phase::LoadTrack;
            break;
        }
        case Phase::LoadTrack:
            if (track_started_) {
                SfzLoader::PumpProjectLoad(candidate_->pool, memory_, io_, io_bytes_);
                if (SfzLoader::ProjectTrackBusy())
                    break;
                track_started_ = false;
                if (SfzLoader::ProjectTrackError() != INST_ERROR_NONE) {
                    status_.failed_track = track_;
                    Finish(InstrumentError(SfzLoader::ProjectTrackError()));
                    break;
                }
                ++track_;
            }
            while (track_ < kNumTracks && !scratch_->tracks[track_].instrument_path[0])
                ++track_;
            status_.progress = static_cast<uint8_t>(10 + track_ * 4);
            if (track_ == kNumTracks) {
                sample_ = 0;
                phase_ = Phase::SampleEdits;
            } else if (!SfzLoader::BeginProjectTrack(track_,
                                                     scratch_->tracks[track_].instrument_path)) {
                status_.failed_track = track_;
                Finish(InstrumentError(SfzLoader::ProjectTrackError()));
            } else
                track_started_ = true;
            break;
        case Phase::SampleEdits:
            if (sample_ < scratch_->sample_count) {
                const auto& saved = scratch_->samples[sample_++];
                auto* record = candidate_->pool.FindByPath(saved.path);
                if (!record || !record->used_by || !ApplyProjectSample(saved, record->payload))
                    Finish(PROJECT_DEPENDENCY);
                break;
            }
            {
                uint16_t referenced = 0;
                candidate_->pool.ForEach([&](SamplePool::Record& r) {
                    if (!r.used_by)
                        return;
                    ++referenced;
                    if (!scratch_
                             ->sample_edits_present) {  // legacy 1.0 file: default sample playback
                        auto& m = r.payload.meta;
                        m.start_frame = m.loop_start = 0;
                        m.end_frame = m.loop_end = m.total_frames;
                        m.gain_db_x10 = 0;
                        m.fade_in_ms = m.fade_out_ms = kDefaultDeclickMs;
                        m.loop_enabled = 0;
                        m.channel_mode = SAMPLE_CH_AS_RECORDED;
                    }
                });
                if (scratch_->sample_edits_present && referenced != scratch_->sample_count)
                    Finish(PROJECT_DEPENDENCY);
                else
                    phase_ = Phase::Commit;
            }
            break;
        case Phase::Commit: {
            for (uint8_t t = 0; t < kNumTracks; ++t) {
                auto& target = candidate_->tracks.At(t);
                const auto& source = scratch_->tracks[t];
                target.midi_in = source.midi_in;
                target.poly_limit = source.poly_limit;
                target.priority = source.priority;
                target.program_change = source.program_change;
            }
            // No fallible I/O/allocation after this point. Playback stays gated
            // until the callback installs the Pattern/settings and acknowledges.
            pool_stage_->Commit();
            SfzLoader::FinishProjectLoad(true);
            MixerControlHandoff::Controls mix;
            mix.master_gain = scratch_->master_gain;
            for (uint8_t t = 0; t < kNumTracks; ++t)
                mix.tracks[t] = scratch_->tracks[t].mix;
            mixer_.Restore(mix);
            const auto& pattern = scratch_->patterns[scratch_->active_pattern];
            exchange_.foreground() = pattern.pattern;
            PatternStore::SetProjectPatternName(pattern.name);
            boundary_.publish();
            SeqTransportMessage settings{SEQ_TRANSPORT_STOP,
                                         scratch_->clock_source,
                                         scratch_->input_mode,
                                         static_cast<uint8_t>(scratch_->quantize),
                                         scratch_->tempo_bpm_x100,
                                         0};
            exchange_.InstallSession(settings);
            phase_ = Phase::Install;
            status_.progress = 95;
            break;
        }
        case Phase::Install:
            if (exchange_.state() == Exchange::State::Installed) {
                exchange_.Retire();
                Promote();
                Finish(PROJECT_OK);
            }
            break;
        case Phase::Cleanup:
            while (track_ < kNumTracks && !(owned_snapshots_ & (1u << track_)))
                ++track_;
            if (track_ < kNumTracks) {
                char path[128];
                SnapshotPath(track_++, path, sizeof(path));
                if (f_unlink(path) != FR_OK)
                    result_ = PROJECT_IO;
                break;
            }
            if (owned_directory_) {
                if (f_unlink(directory_) != FR_OK)
                    result_ = PROJECT_IO;
                owned_directory_ = false;
            }
            Complete();
            break;
    }
}
}  // namespace WaveX::Storage
