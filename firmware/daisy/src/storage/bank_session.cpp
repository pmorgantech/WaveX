#include "bank_session.hpp"

#include <cstdio>
#include <new>

namespace WaveX::Storage {
using namespace Protocol;
using namespace AudioEngine;
using R = BankFileJob::Result;
BankSession::BankSession(
    SampleMemMgr& memory, SamplePool& pool, uint8_t* io, uint32_t bytes, Boundary boundary)
    : memory_(memory), pool_(pool), io_(io), io_bytes_(bytes), boundary_(boundary) {}
BankSession::~BankSession() {
    file_.Cancel();
    Cleanup();
}
void BankSession::Cleanup() {
    if (candidate_) {
        if (SfzLoader::ProjectLoadActive()) {
            SfzLoader::CancelProjectTrack(candidate_->pool, memory_);
            SfzLoader::FinishProjectLoad(false);
        }
        stage_.reset();
        candidate_->~Candidate();
        candidate_ = nullptr;
    }
    memory_.release(&candidate_mem_);
}
void BankSession::RefreshSlot() {
    const auto& slot = index_.slots[status_.slot];
    status_.occupied = status_.loaded && slot.used();
    Protocol::detail::CopyWireString(
        status_.instrument, sizeof(status_.instrument), status_.occupied ? slot.name : "");
}
bool BankSession::Request(const BankOpMessage& request, bool external_busy) {
    if (!IsValidBankOp(request))
        return false;
    status_.request_id = request.request_id;
    status_.slot = request.slot;
    status_.blocked = external_busy;
    RefreshSlot();
    reply_ = true;
    if (request.op == BANK_GET || request.request_id == status_.active_request_id ||
        request.request_id == status_.completed_request_id)
        return false;
    const auto reject = [&](uint8_t error) {
        status_.completed_request_id = request.request_id;
        status_.completed_op = request.op;
        status_.error = error;
        return false;
    };
    if (Busy() || external_busy)
        return reject(BANK_BUSY);
    if (request.revision != status_.revision)
        return reject(BANK_STALE);
    if (request.op != BANK_RECALL && request.op != BANK_PRELOAD &&
        !BankFile::ValidName(request.name))
        return reject(BANK_BAD_NAME);
    if (request.op >= BANK_SAVE_COPY && !status_.loaded)
        return reject(BANK_NO_BANK);
    if ((request.op == BANK_RECALL || request.op == BANK_CLEAR_COPY) && !status_.occupied)
        return reject(BANK_EMPTY_SLOT);
    if ((request.op == BANK_RECALL || request.op == BANK_CLEAR_COPY ||
         (request.op == BANK_STORE_COPY && status_.occupied)) &&
        !(request.flags & BANK_CONFIRM_REPLACE))
        return reject(BANK_CONFIRM_REQUIRED);
    request_ = request;
    status_.busy = 1;
    status_.active_request_id = request.request_id;
    status_.active_op = request.op;
    phase_ = Phase::Begin;
    return true;
}
uint8_t BankSession::FileError() const {
    switch (file_.Status()) {
        case R::BadName:
            return BANK_BAD_NAME;
        case R::Exists:
            return BANK_EXISTS;
        case R::NoSpace:
            return BANK_NO_SPACE;
        case R::NotFound:
            return BANK_NOT_FOUND;
        case R::EmptySlot:
            return BANK_EMPTY_SLOT;
        case R::Invalid:
            return BANK_BAD_FILE;
        default:
            return BANK_IO;
    }
}
uint8_t BankSession::InstrumentError(uint8_t error) const {
    if (error == INST_ERROR_NO_MEMORY || error == INST_ERROR_TOO_LARGE)
        return BANK_NO_MEMORY;
    if (error == INST_ERROR_IO)
        return BANK_IO;
    return BANK_DEPENDENCY;
}
void BankSession::Finish(uint8_t error) {
    file_.Cancel();
    Cleanup();
    status_.busy = 0;
    status_.active_request_id = 0;
    status_.active_op = BANK_GET;
    status_.completed_request_id = request_.request_id;
    status_.completed_op = request_.op;
    status_.error = error;
    RefreshSlot();
    phase_ = Phase::Idle;
    reply_ = true;
}
void BankSession::Pump() {
    if (!Busy())
        return;
    switch (phase_) {
        case Phase::Begin:
            if (request_.op == BANK_STORE_COPY) {
                if (!SfzLoader::BeginBankSnapshot(request_.track)) {
                    Finish(BANK_DEPENDENCY);
                    break;
                }
                phase_ = Phase::Snapshot;
            } else if (request_.op == BANK_RECALL || request_.op == BANK_PRELOAD) {
                void* storage = nullptr;
                if (!memory_.alloc(sizeof(Candidate), &candidate_mem_) ||
                    !memory_.ptr(candidate_mem_, &storage)) {
                    Finish(BANK_NO_MEMORY);
                    break;
                }
                candidate_ = new (storage) Candidate{};
                if (request_.op == BANK_PRELOAD) {
                    stage_.emplace(pool_, candidate_->pool, memory_);
                    if (!stage_->BeginAdditions() ||
                        !SfzLoader::BeginProjectLoad(candidate_->tracks)) {
                        Finish(BANK_DEPENDENCY);
                        break;
                    }
                    preload_slot_ = 0;
                    phase_ = Phase::PreloadNext;
                    break;
                }
                if (!file_.ReadInstrument(status_.name, request_.slot, candidate_->document)) {
                    Finish(FileError());
                    break;
                }
                phase_ = Phase::File;
            } else {
                bool accepted =
                    request_.op == BANK_OPEN
                        ? file_.LoadIndex(request_.name)
                        : file_.SaveCopy(request_.name,
                                         request_.request_id,
                                         request_.op == BANK_NEW ? nullptr : status_.name,
                                         request_.op == BANK_CLEAR_COPY ? request_.slot : -1);
                if (!accepted) {
                    Finish(FileError());
                    break;
                }
                phase_ = Phase::File;
            }
            break;
        case Phase::Snapshot:
            SfzLoader::Pump(pool_, memory_, io_, io_bytes_);
            if (SfzLoader::Busy())
                break;
            if (SfzLoader::ProjectSnapshotError() != INST_ERROR_NONE) {
                Finish(InstrumentError(SfzLoader::ProjectSnapshotError()));
                break;
            }
            if (!file_.SaveCopy(request_.name,
                                request_.request_id,
                                status_.name,
                                request_.slot,
                                &SfzLoader::BankSnapshot())) {
                Finish(FileError());
                break;
            }
            phase_ = Phase::File;
            break;
        case Phase::File:
            file_.Pump();
            if (file_.Busy())
                break;
            if (file_.Status() == R::Saved) {
                if (!file_.LoadIndex(request_.name)) {
                    Finish(FileError());
                    break;
                }
                phase_ = Phase::Index;
            } else if (file_.Status() == R::Indexed) {
                index_ = file_.Index();
                status_.loaded = 1;
                Protocol::detail::CopyWireString(status_.name, sizeof(status_.name), request_.name);
                if (++status_.revision == 0)
                    ++status_.revision;
                Finish(BANK_OK);
            } else if (file_.Status() == R::InstrumentRead)
                phase_ = Phase::Stage;
            else
                Finish(FileError());
            break;
        case Phase::Index:
            file_.Pump();
            if (file_.Busy())
                break;
            if (file_.Status() != R::Indexed) {
                Finish(FileError());
                break;
            }
            index_ = file_.Index();
            status_.loaded = 1;
            Protocol::detail::CopyWireString(status_.name, sizeof(status_.name), request_.name);
            if (++status_.revision == 0)
                ++status_.revision;
            Finish(BANK_OK);
            break;
        case Phase::Stage:
            if (request_.op == BANK_PRELOAD) {
                if (!SfzLoader::BeginProjectPreload(candidate_->document)) {
                    Finish(BANK_DEPENDENCY);
                    break;
                }
                phase_ = Phase::Load;
                break;
            }
            stage_.emplace(pool_, candidate_->pool, memory_);
            if (!stage_->Begin(static_cast<uint16_t>(~(1u << request_.track))) ||
                !SfzLoader::BeginProjectLoad(candidate_->tracks) ||
                !SfzLoader::BeginProjectDocument(request_.track, candidate_->document)) {
                Finish(BANK_DEPENDENCY);
                break;
            }
            phase_ = Phase::Load;
            break;
        case Phase::Load:
            SfzLoader::PumpProjectLoad(candidate_->pool, memory_, io_, io_bytes_);
            if (SfzLoader::ProjectTrackBusy())
                break;
            if (SfzLoader::ProjectTrackError() != INST_ERROR_NONE) {
                Finish(InstrumentError(SfzLoader::ProjectTrackError()));
                break;
            }
            phase_ = request_.op == BANK_PRELOAD ? Phase::PreloadNext : Phase::Commit;
            break;
        case Phase::PreloadNext:
            // At most 128 index entries; documents and PCM load cooperatively.
            while (preload_slot_ < BankFile::kSlots && !index_.slots[preload_slot_].used())
                ++preload_slot_;
            if (preload_slot_ == BankFile::kSlots) {
                SfzLoader::FinishProjectLoad(false);
                stage_->Commit();  // additive: never retires live PCM or changes Tracks
                Finish(BANK_OK);
            } else if (!file_.ReadInstrument(status_.name, preload_slot_++, candidate_->document))
                Finish(FileError());
            else
                phase_ = Phase::File;
            break;
        case Phase::Commit:
            if (!boundary_.stop_track || !boundary_.stop_track(request_.track)) {
                if (boundary_.publish)
                    boundary_.publish();
                Finish(BANK_AUDIO_BUSY);
                break;
            }
            // No fallible I/O/allocation after the stop fence. Only this
            // Instrument is installed; Track routing and mix are independent.
            if (!SfzLoader::FinishProjectLoad(true, request_.track)) {
                if (boundary_.publish)
                    boundary_.publish();
                Finish(BANK_DEPENDENCY);
                break;
            }
            stage_->Commit();
            if (boundary_.publish)
                boundary_.publish();
            Finish(BANK_OK);
            break;
        case Phase::Idle:
            break;
    }
}
}  // namespace WaveX::Storage
