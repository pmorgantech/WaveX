#include "recording_session.hpp"

#include "arm_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace WaveX::AudioEngine {
using namespace Protocol;
void RecordingSession::Init(SamplePool* pool,
                            SampleMemMgr* memory,
                            void (*publish)(const LoadedSampleInfo&),
                            int (*send)(uint16_t, const void*, uint16_t)) {
    pool_ = pool;
    memory_ = memory;
    publish_ = publish;
    send_packet_ = send;
    commands_.Init(Command{});
    meters_.Init(Meter{});
}
bool RecordingSession::Busy() const {
    return status_.active_request_id || status_.state != REC_IDLE;
}
void RecordingSession::Complete(uint32_t request, uint8_t op, uint8_t error) {
    status_.completed_request_id = request;
    status_.completed_op = op;
    status_.error = error;
    send_ = true;
}
void RecordingSession::FreeScratch() {
    if (history_.len)
        memory_->release(&history_);
    if (ring_.len)
        memory_->release(&ring_);
}
bool RecordingSession::Prepare(const RecordOpMessage& request) {
    if (!pool_ || !memory_)
        return false;
    const uint8_t channels = RecordChannels(request.source);
    const uint32_t history_frames = request.preroll_ms * 48u;
    void *take = nullptr, *history = nullptr, *ring = nullptr;
    const bool allocated =
        memory_->alloc(request.max_frames * channels * 2u, &take_) && memory_->ptr(take_, &take) &&
        (!history_frames || (memory_->alloc(history_frames * channels * 2u, &history_) &&
                             memory_->ptr(history_, &history))) &&
        memory_->alloc(16384u * channels * 2u, &ring_) && memory_->ptr(ring_, &ring);
    if (!allocated ||
        !capture_.Configure({request.max_frames, history_frames, request.threshold, channels},
                            {static_cast<int16_t*>(history),
                             static_cast<int16_t*>(ring),
                             static_cast<int16_t*>(take),
                             16384,
                             request.max_frames,
                             history_frames})) {
        FreeScratch();
        if (take_.len)
            memory_->release(&take_);
        return false;
    }
    pcm_ = static_cast<int16_t*>(take);
    saved_ = false;
    status_.take_id = request.request_id;
    status_.source = request.source;
    status_.max_frames = request.max_frames;
    status_.frames = 0;
    status_.sample_id = 0;
    status_.threshold = request.threshold;
    status_.preroll_ms = request.preroll_ms;
    status_.monitor = request.monitor;
    status_.capture_error = REC_OK;
    status_.path[0] = 0;
    status_.progress = 0;
    next_admit_ms_ = 0;
    return true;
}
void RecordingSession::Discard() {
    if (!saved_) {
        if (status_.sample_id) {
            SampleMetadata removed;
            removed.sample_id = status_.sample_id;
            removed.flags = 0;
            send_packet_(MSG_SAMPLE_META, &removed, sizeof(removed));
            pool_->Remove(status_.sample_id);
        }
        if (take_.len)
            memory_->release(&take_);
    }
    take_ = {};
    pcm_ = nullptr;
    FreeScratch();
    status_.state = REC_IDLE;
    status_.take_id = 0;
    status_.sample_id = 0;
    status_.frames = status_.max_frames = 0;
    status_.capture_error = REC_OK;
    status_.path[0] = 0;
    status_.peak_l = status_.peak_r = 0;
    status_.rms_l = status_.rms_r = 0;
    status_.clip_count = 0;
}
void RecordingSession::Request(const RecordOpMessage& request, bool storage_busy) {
    if (!IsValidRecordOp(request))
        return;
    status_.request_id = request.request_id;
    send_ = true;
    if (request.op == REC_GET || request.request_id == status_.active_request_id ||
        request.request_id == status_.completed_request_id)
        return;
    auto fail = [&](uint8_t error) { Complete(request.request_id, request.op, error); };
    if (status_.active_request_id || save_.Busy()) {
        fail(REC_BUSY);
        return;
    }
    if (request.op != REC_ARM && request.take_id != status_.take_id) {
        fail(REC_STALE);
        return;
    }
    pending_ = {};
    pending_.id = request.request_id;
    pending_.op = request.op;
    switch (request.op) {
        case REC_ARM:
            if (status_.state != REC_IDLE) {
                fail(REC_BAD_STATE);
                return;
            }
            if (storage_busy) {
                fail(REC_BUSY);
                return;
            }
            if (!Prepare(request)) {
                fail(REC_NO_MEMORY);
                return;
            }
            pending_.source = request.source;
            pending_.monitor = request.monitor;
            break;
        case REC_START:
            if (status_.state != REC_ARMED) {
                fail(REC_BAD_STATE);
                return;
            }
            break;
        case REC_STOP:
            if (status_.state != REC_ARMED && status_.state != REC_CAPTURING &&
                status_.state != REC_READY) {
                fail(REC_BAD_STATE);
                return;
            }
            break;
        case REC_SAVE:
            if (status_.state != REC_READY || !status_.sample_id || saved_) {
                fail(REC_BAD_STATE);
                return;
            }
            if (storage_busy) {
                fail(REC_BUSY);
                return;
            }
            if (!save_.Begin(request.request_id,
                             request.name,
                             pcm_,
                             status_.frames,
                             RecordChannels(status_.source))) {
                fail(save_.Error());
                return;
            }
            if (pool_->FindByPath(save_.Path())) {
                save_.Cancel();
                fail(REC_EXISTS);
                return;
            }
            status_.active_request_id = request.request_id;
            status_.state = REC_SAVING;
            return;
        case REC_AUDITION:
            if (status_.state != REC_READY || status_.frames < 2) {
                fail(REC_BAD_STATE);
                return;
            }
            pending_.preview.sample = pcm_;
            pending_.preview.sample_frames = status_.frames;
            pending_.preview.sample_rate_hz = 48000;
            pending_.preview.channels = RecordChannels(status_.source);
            pending_.preview.preview = true;
            pending_.preview.one_shot = true;
            pending_.preview.sustain_level = 1;
            pending_.preview.release_s = .005f;
            break;
        case REC_DISCARD:
            if (status_.state != REC_READY) {
                fail(REC_BAD_STATE);
                return;
            }
            if (!saved_ && status_.sample_id && pool_->Find(status_.sample_id)->used_by) {
                fail(REC_BUSY);
                return;
            }
            break;
        default:
            return;
    }
    status_.active_request_id = request.request_id;
    commands_.Publish(pending_);
}
void RecordingSession::Commands(VoiceManager& voices) {
    if (!commands_.AcquireLatest())
        return;
    const auto& command = commands_.ConsumerValue();
    command_error_ = REC_OK;
    switch (command.op) {
        case REC_ARM:
            meter_ = {};
            source_ = command.source;
            monitor_ = command.monitor;
            capture_.Arm();
            capturing_ = true;
            detached_.store(false, std::memory_order_release);
            break;
        case REC_START:
            capture_.Start();
            break;
        case REC_STOP:
            voices.StopGroup(preview_group_);
            preview_group_ = 0;
            capture_.Stop();
            capturing_ = false;
            detached_.store(true, std::memory_order_release);
            break;
        case REC_AUDITION:
            voices.StopGroup(preview_group_);
            preview_group_ = voices.TriggerGroup(&command.preview, 1);
            if (!preview_group_)
                command_error_ = REC_BUSY;
            break;
        case REC_DISCARD:
            voices.StopGroup(preview_group_);
            preview_group_ = 0;
            break;
        default:
            break;
    }
    acknowledged_.store(command.id, std::memory_order_release);
}
void RecordingSession::Monitor(
    const float* left, const float* right, float* out_l, float* out_r, uint32_t frames) {
    if (!capturing_ || !monitor_ || source_ == REC_INTERNAL_MIX || frames > 48)
        return;
    const float* a = source_ == REC_CODEC_RIGHT ? right : left;
    const float* b = source_ == REC_CODEC_STEREO ? right : a;
    for (uint32_t i = 0; i < frames; ++i) {
        out_l[i] += a[i];
        out_r[i] += b[i];
    }
}
void RecordingSession::Process(
    const float* left, const float* right, float* out_l, float* out_r, uint32_t frames) {
    if (!capturing_ || !frames || frames > 48)
        return;
    const float* a =
        source_ == REC_INTERNAL_MIX ? out_l : (source_ == REC_CODEC_RIGHT ? right : left);
    const float* b = source_ == REC_INTERNAL_MIX ? out_r : right;
    alignas(4) int16_t l[48], r[48], interleaved[96];
    const uint8_t channels = RecordChannels(source_);
    arm_float_to_q15(a, l, frames);
    if (channels == 2)
        arm_float_to_q15(b, r, frames);
    uint64_t square_l = 0, square_r = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const int32_t x = l[i], y = channels == 2 ? r[i] : l[i];
        meter_.left = static_cast<uint16_t>(std::max<int32_t>(meter_.left, x < 0 ? -x : x));
        meter_.right = static_cast<uint16_t>(std::max<int32_t>(meter_.right, y < 0 ? -y : y));
        square_l += static_cast<uint64_t>(x * x);
        square_r += static_cast<uint64_t>(y * y);
        if ((std::fabs(a[i]) >= 1.0f || (channels == 2 && std::fabs(b[i]) >= 1.0f)) &&
            meter_.clips != UINT32_MAX)
            ++meter_.clips;
        if (channels == 2) {
            interleaved[2 * i] = l[i];
            interleaved[2 * i + 1] = r[i];
        }
    }
    capture_.Process(channels == 2 ? interleaved : l, frames);
    meter_.square_l = static_cast<uint32_t>(square_l / frames);
    meter_.square_r = static_cast<uint32_t>(square_r / frames);
    meters_.Publish(meter_);
    if (capture_.State() == Recording::Capture::Phase::Stopped) {
        capturing_ = false;
        detached_.store(true, std::memory_order_release);
    }
}
void RecordingSession::Pump(uint32_t now) {
    if (status_.active_request_id && !save_.Busy() &&
        acknowledged_.load(std::memory_order_acquire) == status_.active_request_id) {
        if (pending_.op == REC_ARM)
            status_.state = REC_ARMED;
        if (pending_.op == REC_START)
            status_.state = REC_CAPTURING;
        if (pending_.op == REC_STOP)
            status_.state = REC_DRAINING;
        if (pending_.op == REC_DISCARD)
            Discard();
        Complete(status_.active_request_id, pending_.op, command_error_);
        status_.active_request_id = 0;
    }
    if (status_.state == REC_ARMED || status_.state == REC_CAPTURING ||
        status_.state == REC_DRAINING) {
        capture_.Drain(2048);
        status_.frames = capture_.Frames();
        if (capture_.State() == Recording::Capture::Phase::Capturing)
            status_.state = REC_CAPTURING;
        if (capture_.State() == Recording::Capture::Phase::Stopped)
            status_.state = REC_DRAINING;
        if (capture_.Complete() && detached_.load(std::memory_order_acquire)) {
            FreeScratch();
            status_.state = REC_READY;
            status_.capture_error =
                capture_.Reason() == Recording::Capture::End::Overflow ? REC_OVERFLOW : REC_OK;
            if (!status_.frames) {
                Discard();
            }
            send_ = true;
        }
    }
    if (status_.state == REC_READY && !status_.sample_id && status_.frames &&
        static_cast<int32_t>(now - next_admit_ms_) >= 0) {
        next_admit_ms_ = now + 250;
        SamplePool::Record* record = nullptr;
        if (pool_->AdmitTransient(&record) == SamplePool::Admit::Ok) {
            ResidentSampleInfo resident{};
            resident.sample_rate = 48000;
            resident.total_frames = status_.frames;
            resident.channels = RecordChannels(status_.source);
            resident.bit_depth = 16;
            resident.data_size = status_.frames * resident.channels * 2u;
            FillLoadedSample(record->payload, record->sample_id, "", resident, take_);
            std::snprintf(
                record->payload.meta.name, sizeof(record->payload.meta.name), "Unsaved take");
            pool_->SetPinned(record->sample_id, true);
            status_.sample_id = record->sample_id;
            publish_(record->payload);
            status_.error = REC_OK;
            send_ = true;
        } else if (status_.error != REC_NO_MEMORY) {
            status_.error = REC_NO_MEMORY;
            send_ = true;
        }
    }
    if (save_.Busy()) {
        save_.Pump();
        status_.progress = save_.Progress();
        if (!save_.Busy()) {
            uint8_t error = save_.Error();
            if (error == REC_OK) {
                auto* record = pool_->Find(status_.sample_id);
                if (!record || !pool_->BindPath(status_.sample_id, save_.Path()))
                    error = REC_BAD_STATE;
                else {
                    saved_ = true;
                    SampleFile::Document document;
                    document.sample = save_.Metadata();
                    SampleFile::Apply(document, record->payload.meta);
                    const char* basename = std::strrchr(save_.Path(), '/');
                    Protocol::detail::CopyWireString(record->payload.meta.name,
                                                     sizeof(record->payload.meta.name),
                                                     basename ? basename + 1 : save_.Path());
                    Protocol::detail::CopyWireString(
                        status_.path, sizeof(status_.path), save_.Path());
                    pool_->NoteNewest(status_.sample_id);
                    publish_(record->payload);
                }
            }
            status_.state = REC_READY;
            Complete(status_.active_request_id, REC_SAVE, error);
            status_.active_request_id = 0;
        }
    }
    Meter meter;
    if (meters_.ConsumeLatest(meter) && status_.state != REC_IDLE) {
        status_.peak_l = meter.left;
        status_.peak_r = meter.right;
        status_.rms_l = static_cast<uint16_t>(std::sqrt(float(meter.square_l)));
        status_.rms_r = static_cast<uint16_t>(std::sqrt(float(meter.square_r)));
        status_.clip_count = meter.clips;
    }
    if (status_.request_id && (send_ || (status_.state != REC_IDLE && now - last_send_ >= 67))) {
        if (send_packet_(MSG_REC_STATUS, &status_, sizeof(status_)) >= 0) {
            send_ = false;
            last_send_ = now;
        }
    }
}
}  // namespace WaveX::AudioEngine
