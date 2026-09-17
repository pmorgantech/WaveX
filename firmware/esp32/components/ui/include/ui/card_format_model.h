#pragma once
#include "spi_protocol/protocol.h"
namespace wavex_ui {
class CardFormatModel {
   public:
    bool CanConfirm() const {
        return !pending_ && prepared_ && state_.state == WaveX::Protocol::CARD_CONFIRMATION &&
               state_.token == prepared_;
    }
    WaveX::Protocol::CardOpMessage Request(uint8_t op, uint32_t id) {
        using namespace WaveX::Protocol;
        CardOpMessage request{id, 0, op, {}};
        if (op == CARD_CONFIRM_FORMAT) {
            if (!CanConfirm())
                return {};  // not a valid command
            request.token = state_.token;
            confirmed_ = id;
            prepared_ = 0;
        } else if (op == CARD_PREPARE_FORMAT) {
            prepared_ = id;
            confirmed_ = 0;
        } else if (op == CARD_CANCEL) {
            prepared_ = 0;
        }
        expected_ = id;
        // GET polling retains an already displayed confirmation. Reads cannot
        // create one: only this page's explicit Prepare can authorize it.
        if (op != CARD_GET)
            pending_ = true;
        return request;
    }
    bool Accept(const WaveX::Protocol::CardStateMessage& state) {
        if (!WaveX::Protocol::IsValidCardState(state) || state.request_id != expected_ ||
            state.request_id == accepted_)
            return false;
        accepted_ = state.request_id;
        state_ = state;
        pending_ = false;
        if (state.state != WaveX::Protocol::CARD_CONFIRMATION)
            prepared_ = 0;
        return true;
    }
    void SendFailed() {
        pending_ = false;
        prepared_ = 0;
    }
    const WaveX::Protocol::CardStateMessage& State() const { return state_; }
    bool Pending() const { return pending_; }
    bool Armed() const { return prepared_ != 0; }
    bool UnknownResult() const {
        return confirmed_ && state_.completed_request_id != confirmed_ &&
               state_.state != WaveX::Protocol::CARD_FORMATTING;
    }

   private:
    WaveX::Protocol::CardStateMessage state_{};
    uint32_t expected_ = 0, prepared_ = 0, accepted_ = 0, confirmed_ = 0;
    bool pending_ = false;
};
}  // namespace wavex_ui
