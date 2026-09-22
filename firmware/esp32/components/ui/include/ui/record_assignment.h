#pragma once
#include "spi_protocol/protocol.h"

namespace wavex_ui {
// UI-owned navigation intent; never authorizes a map mutation. The recorder
// must acknowledge releasing this saved take before its sample can be assigned.
struct RecordAssignment {
    uint32_t request = 0;
    uint16_t sample = 0;
    bool keyboard = false;
    bool Begin(const WaveX::Protocol::RecordStatusMessage& take, uint32_t id, bool keys) {
        using namespace WaveX::Protocol;
        if (!id || take.state != REC_READY || !take.path[0] || !take.sample_id ||
            take.active_request_id)
            return false;
        request = id;
        sample = take.sample_id;
        keyboard = keys;
        return true;
    }
    bool Complete(const WaveX::Protocol::RecordStatusMessage& status) const {
        using namespace WaveX::Protocol;
        return request && status.completed_request_id == request &&
               status.completed_op == REC_DISCARD && status.error == REC_OK &&
               status.state == REC_IDLE && !status.active_request_id;
    }
};
}  // namespace wavex_ui
