#pragma once

#include "spi_protocol/protocol.h"

#include <array>
#include <cstdint>

namespace wavex_ui {

// UI-domain view, never an authority for backend pattern ownership. A page
// only becomes editable after a matching, validated backend reply.
class SequencerGridModel {
   public:
    static constexpr uint8_t kRows = 4;
    using Page = WaveX::Protocol::SeqPatternSyncMessage;
    using Request = WaveX::Protocol::SeqPatternRequestMessage;
    using Step = WaveX::Protocol::SeqStepState;

    bool SetWindow(uint8_t first_track, uint8_t first_step) {
        if (first_track > WaveX::Protocol::SEQ_TRACK_COUNT - kRows || first_track % kRows != 0 ||
            first_step >= WaveX::Protocol::SEQ_MAX_STEPS ||
            first_step % WaveX::Protocol::SEQ_PAGE_STEPS != 0)
            return false;
        first_track_ = first_track;
        first_step_ = first_step;
        Invalidate();
        return true;
    }
    void Invalidate() {
        read_only_ = false;
        epoch_ = 0;
        ready_.fill(false);
        snapshot_valid_.fill(false);
        DiscardPreview();
        request_ = Request{};
    }
    Request BeginRead(uint32_t id, uint8_t row) {
        request_ = Request{};
        if (row < kRows && id != 0) {
            request_.request_id = id;
            request_.track = first_track_ + row;
            request_.first_step = first_step_;
        }
        return request_;
    }
    bool Accept(const Page& page) {
        using namespace WaveX::Protocol;
        if (!request_.request_id || page.request_id != request_.request_id ||
            page.track != request_.track || page.first_step != request_.first_step ||
            page.valid != 1 || page.length < 1 || page.length > SEQ_MAX_STEPS || page.scale > 5 ||
            page.swing < 50 || page.swing > 75 || page.enabled > 1 ||
            page.clock_source > SEQ_CLOCK_MIDI || page.tempo_bpm_x100 < 100)
            return false;
        for (const auto& step: page.steps)
            if (step.on > 1 || step.note > 127 || step.velocity > 127 || step.probability > 100 ||
                step.retrig_count > 8)
                return false;
        const auto row = static_cast<uint8_t>(page.track - first_track_);
        pages_[row] = page;
        if (preview_valid_ && preview_row_ == row)
            DiscardPreview();
        ready_[row] = true;
        snapshot_valid_[row] = true;
        request_ = Request{};
        return true;
    }
    bool AcceptScoped(const WaveX::Protocol::SeqSlotPageMessage& snapshot) {
        if (!WaveX::Protocol::IsValidSeqSlotPage(snapshot) || !Accept(snapshot.page))
            return false;
        if (epoch_ != snapshot.epoch || pattern_ != snapshot.pattern) {
            ready_.fill(false);
            snapshot_valid_.fill(false);
            DiscardPreview();
            const auto row = static_cast<uint8_t>(snapshot.page.track - first_track_);
            ready_[row] = snapshot_valid_[row] = true;
        }
        read_only_ = snapshot.read_only;
        epoch_ = snapshot.epoch;
        pattern_ = snapshot.pattern;
        return true;
    }
    WaveX::Protocol::SeqSlotEditMessage ScopedEdit(
        const WaveX::Protocol::SeqPatternOpMessage& edit) const {
        WaveX::Protocol::SeqSlotEditMessage message;
        message.epoch = epoch_;
        message.pattern = pattern_;
        message.edit = edit;
        return message;
    }
    uint32_t Epoch() const { return epoch_; }
    uint8_t PatternSlot() const { return pattern_; }

    void InvalidateRow(uint8_t row) {
        if (row < kRows)
            ready_[row] = false;
        request_ = Request{};
    }
    // A successful outgoing edit can be the basis of another drag increment
    // while its row awaits readback. Confirmed pages remain untouched.
    bool CopyStepForEdit(uint8_t row, uint8_t column, Step& out) const {
        if (row >= kRows || column >= WaveX::Protocol::SEQ_PAGE_STEPS)
            return false;
        if (preview_valid_ && preview_row_ == row && preview_column_ == column) {
            out = preview_;
            return true;
        }
        if (!Ready(row))
            return false;
        out = pages_[row].steps[column];
        return true;
    }
    void PreviewStep(uint8_t row, uint8_t column, const Step& step) {
        if (row >= kRows || column >= WaveX::Protocol::SEQ_PAGE_STEPS)
            return;
        preview_ = step;
        preview_row_ = row;
        preview_column_ = column;
        preview_valid_ = true;
    }
    void DiscardPreview() { preview_valid_ = false; }
    // Readback pending does not erase the last confirmed picture. It still
    // authorizes no new edits; only Accept() can make that row ready again.
    bool HasSnapshot(uint8_t row) const { return row < kRows && snapshot_valid_[row]; }
    bool ReadOnly() const { return read_only_; }
    bool Ready(uint8_t row) const { return !read_only_ && row < kRows && ready_[row]; }
    bool AllReady() const {
        if (read_only_)
            return false;
        for (bool ready: ready_)
            if (!ready)
                return false;
        return true;
    }
    bool Waiting() const { return request_.request_id != 0; }
    uint8_t FirstTrack() const { return first_track_; }
    uint8_t FirstStep() const { return first_step_; }
    const Page& Row(uint8_t row) const { return pages_[row]; }

   private:
    std::array<Page, kRows> pages_{};
    std::array<bool, kRows> ready_{};
    std::array<bool, kRows> snapshot_valid_{};
    Request request_{};
    Step preview_{};
    uint8_t preview_row_ = 0, preview_column_ = 0;
    bool preview_valid_ = false, read_only_ = false;
    uint32_t epoch_ = 0;
    uint8_t pattern_ = 0;
    uint8_t first_track_ = 0;
    uint8_t first_step_ = 0;
};
}  // namespace wavex_ui
