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
        ready_.fill(false);
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
            if (step.on > 1 || step.velocity > 127 || step.probability > 100 ||
                step.retrig_count > 8)
                return false;
        const auto row = static_cast<uint8_t>(page.track - first_track_);
        pages_[row] = page;
        ready_[row] = true;
        request_ = Request{};
        return true;
    }
    void InvalidateRow(uint8_t row) {
        if (row < kRows)
            ready_[row] = false;
        request_ = Request{};
    }
    bool Ready(uint8_t row) const { return row < kRows && ready_[row]; }
    bool AllReady() const {
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
    Request request_{};
    uint8_t first_track_ = 0;
    uint8_t first_step_ = 0;
};
}  // namespace wavex_ui
