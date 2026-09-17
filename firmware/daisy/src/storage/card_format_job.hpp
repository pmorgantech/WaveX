#pragma once
#include "spi_protocol/protocol.h"

namespace WaveX::Storage {
// Foreground-owned confirmation and retained result. A read never retries a
// format. Card changes, cancellation and a 60-second expiry disarm the token.
class CardFormatJob {
   public:
    using Request = Protocol::CardOpMessage;
    using Reply = Protocol::CardStateMessage;
    const Reply& State() const { return state_; }
    bool Busy() const { return state_.state == Protocol::CARD_FORMATTING; }
    bool Dirty() const { return dirty_; }
    void Sent() {
        dirty_ = false;
        status_sent_ = true;
    }
    void Handle(const Request& request,
                uint32_t now,
                uint32_t media,
                bool ready,
                bool storage_busy,
                bool mounted) {
        using namespace Protocol;
        if (!IsValidCardOp(request))
            return;
        state_.request_id = request.request_id;
        state_.mounted = mounted;
        dirty_ = true;
        if (request.op == CARD_GET || Busy() || request.request_id == state_.completed_request_id)
            return;
        if (request.op == CARD_CANCEL) {
            state_.token = 0;
            state_.state = CARD_IDLE;
            state_.error = CARD_OK;
            return;
        }
        if (request.op == CARD_PREPARE_FORMAT) {
            state_.token = 0;
            if (!ready || storage_busy) {
                state_.state = CARD_FAILED;
                state_.error = ready ? CARD_BUSY : CARD_NOT_READY;
                return;
            }
            state_.token = request.request_id;
            state_.state = CARD_CONFIRMATION;
            state_.error = CARD_OK;
            armed_at_ = now;
            media_ = media;
            return;
        }
        const bool confirmed = state_.state == CARD_CONFIRMATION && request.token == state_.token &&
                               media == media_ && now - armed_at_ < 60000u;
        state_.token = 0;
        state_.completed_request_id = request.request_id;
        if (!confirmed || !ready || storage_busy) {
            state_.state = CARD_FAILED;
            state_.error = !confirmed ? CARD_BAD_CONFIRMATION : !ready ? CARD_NOT_READY : CARD_BUSY;
            return;
        }
        state_.state = CARD_FORMATTING;
        state_.error = CARD_OK;
        start_at_ = now;
        ran_ = status_sent_ = false;
    }
    bool TakeRun(uint32_t now, uint32_t media) {
        if (!Busy() || ran_)
            return false;
        if (media != media_) {
            Complete(Protocol::CARD_BAD_CONFIRMATION, false);
            return false;
        }
        if (!status_sent_ || now - start_at_ < 100u)
            return false;
        ran_ = true;
        return true;
    }
    void Complete(uint8_t error, bool mounted) {
        state_.state = error ? Protocol::CARD_FAILED : Protocol::CARD_DONE;
        state_.error = error;
        state_.mounted = mounted;
        state_.token = 0;
        dirty_ = true;
    }

   private:
    Reply state_{};
    uint32_t armed_at_ = 0, start_at_ = 0, media_ = 0;
    bool dirty_ = false, ran_ = false, status_sent_ = false;
};
}  // namespace WaveX::Storage
