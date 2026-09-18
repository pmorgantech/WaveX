#pragma once
#include "spi_protocol/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wavex_ui {
// Desired preview and acknowledged settings are separate; the backend owns undo.
class LfoModel {
    using State = WaveX::Protocol::InstLfoSyncMessage;
    using Settings = WaveX::Protocol::InstLfoSettings;

   public:
    void Reset(uint8_t track) {
        *this = LfoModel{};
        state_.track = track;
    }
    void AdoptRevision(uint32_t revision) {
        if (valid_)
            state_.revision = revision;
    }
    void Expect(uint32_t id) { expected_ = id; }
    void Sent(uint32_t id) {
        pending_ = expected_ = id;
        sent_ = desired_;
    }
    bool Accept(const State& s) {
        if (!expected_ || s.request_id != expected_ || s.track != state_.track || !s.revision ||
            s.valid > 1 || s.busy > 1 ||
            (valid_ && static_cast<int32_t>(s.revision - state_.revision) < 0))
            return false;
        for (const auto& v: s.values) {
            // Retain unknown selectors from a newer file while displaying them
            // explicitly; writes still require supported settings.
            auto checked = v;
            checked.wave = checked.sync_div = 0;
            if (!WaveX::Protocol::IsValidInstLfoSettings(checked))
                return false;
        }
        State previous = state_;
        previous.request_id = s.request_id;
        if (valid_ && std::memcmp(&previous, &s, sizeof(s)) == 0)
            return false;
        const bool completed = pending_ && s.completed_request_id == pending_;
        const bool replaced = valid_ && s.revision != state_.revision;
        const bool queued = pending_ && std::memcmp(&desired_, &sent_, sizeof(desired_));
        state_ = s;
        valid_ = true;
        if (completed)
            pending_ = 0;
        if (completed && queued && !s.error)
            UpdateDirty();
        else if ((!dirty_ && !pending_) || replaced || completed)
            Load();
        return true;
    }
    bool Select(uint8_t index) {
        if (index >= 2 || dirty_ || pending_)
            return false;
        index_ = index;
        Load();
        return true;
    }
    const State& Snapshot() const { return state_; }
    uint8_t Index() const { return index_; }
    bool Valid() const { return valid_; }
    bool Ready() const { return valid_ && !state_.busy && !pending_; }
    bool Editable() const { return valid_ && state_.valid && !state_.busy; }
    bool Dirty() const { return dirty_; }
    bool Pending() const { return pending_ != 0; }
    int Value(uint8_t field) const {
        switch (field) {
            case 0:
                return desired_.wave;
            case 1:
                return static_cast<int>(desired_.rate_hz * 1000 + .5f);
            case 2:
                return desired_.sync_div;
            case 3:
                return desired_.retrigger;
            case 4:
                return static_cast<int>(desired_.delay_s * 1000 + .5f);
            case 5:
                return static_cast<int>(desired_.fade_s * 1000 + .5f);
            case 6:
                return desired_.pitch_follow;
            default:
                return 0;
        }
    }
    static int Maximum(uint8_t field) {
        return field == 0                 ? 4
               : field == 1               ? 100000
               : field == 2               ? WaveX::LfoControl::kDivisionCount - 1
               : field == 4 || field == 5 ? 600000
                                          : 1;
    }
    bool Set(uint8_t field, int value) {
        if (!Editable() || field > 6 || value < (field == 1 ? 10 : 0) || value > Maximum(field))
            return false;
        switch (field) {
            case 0:
                desired_.wave = static_cast<uint8_t>(value);
                break;
            case 1:
                desired_.rate_hz = static_cast<float>(value) / 1000;
                break;
            case 2:
                desired_.sync_div = static_cast<uint8_t>(value);
                break;
            case 3:
                desired_.retrigger = static_cast<uint8_t>(value);
                break;
            case 4:
                desired_.delay_s = static_cast<float>(value) / 1000;
                break;
            case 5:
                desired_.fade_s = static_cast<float>(value) / 1000;
                break;
            case 6:
                desired_.pitch_follow = static_cast<uint8_t>(value);
                break;
        }
        UpdateDirty();
        return true;
    }
    bool Synced() const { return desired_.sync_div != 0; }
    float RateHz() const { return desired_.rate_hz; }
    bool SetSync(bool enabled) {
        // Hz is independent and stays in every preview/save snapshot. Returning
        // to sync defaults to a quarter note; an existing division is retained.
        return Set(2, enabled ? (Synced() ? desired_.sync_div : 3) : 0);
    }
    int DurationIndex() const {
        for (int i = 0; i < WaveX::LfoControl::kDivisionCount - 1; ++i)
            if (WaveX::LfoControl::kDurationOrder[i] == desired_.sync_div)
                return i;
        return 0;
    }
    bool AdjustRate(int steps, int divisor = 1) {
        if (!Editable() || !steps)
            return false;
        if (Synced()) {
            const auto index = std::clamp<int64_t>(
                int64_t{DurationIndex()} + steps, 0, WaveX::LfoControl::kDivisionCount - 2);
            return Set(2, WaveX::LfoControl::kDurationOrder[index]);
        }
        // Logarithmic movement across four decades. Keep float Hz here so small
        // adjustments near 0.01 are not rounded away by the console's mHz units.
        const float base = std::clamp(
            desired_.rate_hz, WaveX::LfoControl::kMinRateHz, WaveX::LfoControl::kMaxRateHz);
        const double movement =
            std::clamp(static_cast<double>(steps) / (100.0 * std::max(divisor, 1)), -4.0, 4.0);
        desired_.rate_hz =
            static_cast<float>(std::clamp(base * std::pow(10.0, movement),
                                          static_cast<double>(WaveX::LfoControl::kMinRateHz),
                                          static_cast<double>(WaveX::LfoControl::kMaxRateHz)));
        UpdateDirty();
        return true;
    }
    float RateFill() const {
        if (Synced())
            return static_cast<float>(DurationIndex()) / (WaveX::LfoControl::kDivisionCount - 2);
        return std::clamp(std::log10(std::clamp(desired_.rate_hz,
                                                WaveX::LfoControl::kMinRateHz,
                                                WaveX::LfoControl::kMaxRateHz) /
                                     WaveX::LfoControl::kMinRateHz) /
                              4.f,
                          0.f,
                          1.f);
    }
    WaveX::Protocol::InstLfoOpMessage Request(uint32_t id, bool get = false) const {
        WaveX::Protocol::InstLfoOpMessage r;
        r.request_id = id;
        r.revision = state_.revision;
        r.track = state_.track;
        r.index = index_;
        r.op = get ? WaveX::Protocol::INST_LFO_GET : WaveX::Protocol::INST_LFO_SET;
        r.value = desired_;
        return r;
    }

   private:
    void Load() {
        desired_ = state_.values[index_];
        dirty_ = false;
    }
    void UpdateDirty() {
        dirty_ = std::memcmp(&desired_, &state_.values[index_], sizeof(desired_)) != 0;
    }
    State state_{};
    Settings desired_{}, sent_{};
    uint32_t expected_ = 0, pending_ = 0;
    uint8_t index_ = 0;
    bool valid_ = false, dirty_ = false;
};
}  // namespace wavex_ui
