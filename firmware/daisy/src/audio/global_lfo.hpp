#pragma once
#include "spi_protocol/protocol.h"

#include "lfo.hpp"
#include "snapshot_mailbox.hpp"
#include <cmath>

namespace WaveX::AudioEngine {
// Session-owned performance control. Foreground owns requests/replies; the
// callback owns phase and acquires an immutable settings/reset snapshot.
class GlobalLfo {
   public:
    void Init() {
        mailbox_.Init(published_);
        lfo_.Init(1000.f);
    }
    void Request(const Protocol::GlobalLfoOpMessage& request) {
        using namespace Protocol;
        if (!IsValidGlobalLfoOp(request))
            return;
        reply_.request_id = request.request_id;
        pending_ = true;
        if (request.op == GLOBAL_LFO_GET || request.request_id == reply_.completed_request_id)
            return;
        reply_.completed_request_id = request.request_id;
        reply_.error = request.revision != reply_.revision;
        if (reply_.error)
            return;
        if (request.op == GLOBAL_LFO_SET)
            reply_.value = request.value;
        else
            ++published_.reset;
        if (!++reply_.revision)
            ++reply_.revision;
        published_.value = reply_.value;
        mailbox_.Publish(published_);
    }
    template <class Send>
    void Pump(Send send) {
        if (pending_ && send(Protocol::MSG_GLOBAL_LFO_SYNC, &reply_, sizeof(reply_)) >= 0)
            pending_ = false;
    }
    float Tick(float tempo, bool playing, uint32_t run, bool note) {
        mailbox_.ConsumeLatest(active_);
        const auto& v = active_.value;
        lfo_.SetWave(static_cast<LfoWave>(v.wave));
        float hz = v.rate_hz;
        if (v.sync_div) {
            const auto& d = LfoControl::kDivisions[v.sync_div];
            hz = std::ldexp(tempo / 60.f, d.shift) / d.divisor;
        }
        lfo_.SetRateHz(hz);
        if (reset_ != active_.reset || (v.restart == 1 && playing && (!playing_ || run != run_)) ||
            (v.restart == 2 && note))
            lfo_.Retrigger();
        reset_ = active_.reset;
        playing_ = playing;
        run_ = run;
        return lfo_.Tick();
    }
    const Protocol::GlobalLfoSyncMessage& State() const { return reply_; }
    float Phase() const { return lfo_.Phase(); }

   private:
    struct Settings {
        Protocol::GlobalLfoSettings value;
        uint32_t reset = 0;
    };
    Protocol::GlobalLfoSyncMessage reply_;
    SnapshotMailbox<Settings> mailbox_;
    Settings published_, active_;
    Lfo lfo_;
    uint32_t reset_ = 0, run_ = 0;
    bool pending_ = false, playing_ = false;
};
}  // namespace WaveX::AudioEngine
