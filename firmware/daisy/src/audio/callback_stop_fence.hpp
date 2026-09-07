#pragma once

#include "snapshot_mailbox.hpp"
#include <cstdint>

namespace WaveX::AudioEngine {

// Main loop owns stop requests; the callback owns voice state and completion.
// Coalesced requests retain every unacknowledged Track bit. A generation
// identifies the stop being awaited, so an earlier stop cannot release memory
// for a newer request. At most 2^31 generations may be outstanding.
class CallbackStopFence {
   public:
    void Init() {
        generation_ = 0;
        pending_tracks_ = 0;
        __atomic_store_n(&completed_, 0u, __ATOMIC_RELAXED);
        mailbox_.Init(Request{});
    }

    uint32_t RequestStop(uint16_t tracks) {
        if (tracks == 0) {
            return 0;
        }
        if (Complete(generation_)) {
            pending_tracks_ = 0;
        }
        pending_tracks_ |= tracks;
        if (++generation_ == 0) {
            ++generation_;
        }
        mailbox_.Publish(Request{generation_, pending_tracks_});
        return generation_;
    }

    bool Complete(uint32_t generation) const {
        if (generation == 0) {
            return true;
        }
        const uint32_t completed = __atomic_load_n(&completed_, __ATOMIC_ACQUIRE);
        return static_cast<int32_t>(completed - generation) >= 0;
    }

    // Callback only, after queued notes and stale trigger-map references have
    // been drained. The acknowledgement is published only after stop returns.
    template <typename Stop>
    bool ConsumeAndStop(Stop stop) {
        Request request;
        if (!mailbox_.ConsumeLatest(request)) {
            return false;
        }
        stop(request.tracks);
        __atomic_store_n(&completed_, request.generation, __ATOMIC_RELEASE);
        return true;
    }

   private:
    struct Request {
        uint32_t generation = 0;
        uint16_t tracks = 0;
    };
    SnapshotMailbox<Request> mailbox_;
    uint32_t generation_ = 0;
    uint16_t pending_tracks_ = 0;
    uint32_t completed_ = 0;
};

}  // namespace WaveX::AudioEngine
