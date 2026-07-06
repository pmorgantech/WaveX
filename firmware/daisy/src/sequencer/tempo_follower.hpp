#pragma once

// MIDI clock PLL tempo follower (design:
// docs/features/midi-sync-tempo-follower.md §3-4). HAL-free: operates on
// caller-supplied deltas and its own internal frame counter, so it is
// host-testable without any Daisy hardware or cross compiler - same
// pattern as sequencer_scheduler.hpp.
//
// Clock-domain contract (midi-sync-tempo-follower.md §2 - read this before
// changing anything here): the ESP32's per-byte MIDI ingest timestamps and
// the Daisy's own audio-frame counter are different, unrelated clock
// domains. This class enforces the rule that keeps that safe:
//   - `esp_delta_us` (time between consecutive MIDI clock bytes, measured
//     entirely on the ESP32 side) is used ONLY to estimate the master's
//     *period* - a difference of two same-domain timestamps, so it is not
//     subject to cross-domain bias, only to that domain's own jitter.
//   - This class's own frame counter (`frame_counter_`, advanced by
//     `block_size_` every Tick() call - the same exact-integer-accumulation
//     technique as SequencerScheduler) is the *only* thing ever compared
//     to the Daisy audio timeline (freewheel-timeout detection). The two
//     domains are never subtracted from each other.
//
// Call contract:
//   - OnMidiClock() represents the interval between two consecutive
//     incoming MIDI clock bytes (0xF8), NOT the very first byte received -
//     there is no delta to report for a first byte with nothing before it.
//     A caller that just enabled MIDI sync should simply not call
//     OnMidiClock() for the first byte, then start calling it from the
//     second byte onward.
//   - Tick() must be called exactly once per 1 kHz control tick (one audio
//     block), matching SequencerScheduler's Process() cadence, so the two
//     stay phase-locked to the same audio timeline.
//
// Real-time-safety: every method is allocation-free, branch-bounded, plain
// arithmetic - safe to call from the audio callback's control-tick context
// (architecture.md §1) and from the main-loop message-dispatch path.

#include <cmath>
#include <cstdint>

namespace WaveX {
namespace Sequencer {

// MIDI clock is 24 PPQN; the sequencer's internal resolution is 96 PPQN
// (pattern.hpp's kInternalPpqn) - exactly 4 internal ticks per MIDI clock.
static constexpr double kInternalTicksPerMidiClock = 4.0;

enum class SyncLockState : uint8_t {
    Unlocked,   // MIDI sync not active, or no clock received yet since enabling it
    Acquiring,  // collecting clock deltas, not yet stable enough to trust
    Locked,     // phase servo active, tracking the external clock
    Freewheel,  // clock stopped arriving; keep playing at the last known rate
};

class TempoFollower {
   public:
    void Init(uint32_t sample_rate, uint16_t block_size) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        block_size_ = block_size > 0 ? block_size : 48;
    }

    void SetNominalBpm(float bpm) {
        nominal_bpm_ = bpm > 1.0f ? bpm : 1.0f;
        if (state_ == SyncLockState::Unlocked)
            effective_bpm_ = nominal_bpm_;
    }

    // Enabling MIDI sync (false->true) resets all acquisition state -
    // starting sync fresh always re-acquires rather than trusting stale
    // history from a previous session. Disabling it drops straight back
    // to Unlocked/free-run at the nominal tempo (docs' "any -> INTERNAL"
    // transition).
    void SetSyncSource(bool use_midi) {
        if (use_midi == use_midi_)
            return;
        use_midi_ = use_midi;
        if (use_midi_) {
            ResetAcquisition();
        } else {
            state_ = SyncLockState::Unlocked;
            effective_bpm_ = nominal_bpm_;
        }
    }

    bool UsingMidiSync() const { return use_midi_; }

    // MIDI Start: establishes Φ=0 as the downbeat. Simplification versus a
    // literal reading of the MIDI spec (where the *following* clock byte
    // is the true downbeat): this class resets phase immediately rather
    // than deferring the reset to the next OnMidiClock() call. The servo's
    // own error-correction pulls in any resulting sub-tick offset within
    // the first few clocks, which is what the servo exists to do anyway.
    void OnStart() {
        playing_ = true;
        phase_ = 0.0;
        anchor_phase_ = 0.0;
        clock_index_since_anchor_ = 0;
        phase_error_ = 0.0;
    }

    void OnStop() { playing_ = false; }

    // MIDI Continue with a Song Position Pointer, in MIDI beats (16th
    // notes). Internal ticks per 16th note = kInternalPpqn/4 = 24.
    void OnContinue(uint16_t spp_beats16) {
        playing_ = true;
        phase_ = static_cast<double>(spp_beats16) * 24.0;
        anchor_phase_ = phase_;
        clock_index_since_anchor_ = 0;
        phase_error_ = 0.0;
    }

    // `esp_delta_us`: microseconds between this MIDI clock byte and the
    // previous one, measured on the ESP32 (see class comment). If more
    // than one MIDI clock's worth of time elapsed without an intervening
    // call (a dropped byte), pass the count in `clocks_elapsed` so the
    // period estimate divides out the gap correctly instead of reading a
    // sudden tempo change.
    void OnMidiClock(uint32_t esp_delta_us, uint16_t clocks_elapsed = 1) {
        if (!use_midi_)
            return;
        if (clocks_elapsed == 0)
            clocks_elapsed = 1;
        const double delta =
            static_cast<double>(esp_delta_us) / static_cast<double>(clocks_elapsed);
        last_clock_frame_ = frame_counter_;

        if (state_ == SyncLockState::Unlocked) {
            ResetAcquisition();
            PushDeltaHistory(delta);
            period_us_ = delta;
            UpdateEffectiveBpmFromPeriod();
            state_ = SyncLockState::Acquiring;
            return;
        }

        if (state_ == SyncLockState::Freewheel) {
            // Clock resumed: per the design doc's state machine, re-enter
            // ACQUIRING (re-anchoring happens on the transition back to
            // LOCKED, same as any other acquisition).
            ResetAcquisition();
            PushDeltaHistory(delta);
            period_us_ = delta;
            UpdateEffectiveBpmFromPeriod();
            state_ = SyncLockState::Acquiring;
            return;
        }

        if (state_ == SyncLockState::Acquiring) {
            PushDeltaHistory(delta);
            period_us_ = MedianOfHistory();
            UpdateEffectiveBpmFromPeriod();
            if (history_count_ >= kAcquireStableCount && IsHistoryStable(kStableToleranceFrac)) {
                EnterLocked();
            }
            return;
        }

        // Locked: outlier-reject against the current period estimate.
        const bool outlier = std::fabs(delta - period_us_) > kOutlierToleranceFrac * period_us_;
        if (outlier) {
            rejected_history_[rejected_count_ % kRejectedHistorySize] = delta;
            ++rejected_count_;
            if (rejected_count_ >= kRejectedHistorySize && RejectedHistoryMutuallyConsistent()) {
                // Tempo jump: the last 3 "outliers" actually agree with
                // each other, just not with the old period - treat as a
                // real tempo change and re-acquire at the new value.
                period_us_ = AverageOfRejectedHistory();
                UpdateEffectiveBpmFromPeriod();
                ResetAcquisition();
                for (uint8_t i = 0; i < kRejectedHistorySize; ++i)
                    PushDeltaHistory(rejected_history_[i]);
                state_ = SyncLockState::Acquiring;
            }
            // Otherwise: a single spurious outlier - ignore for period
            // estimation, but still update the phase-servo book-keeping
            // below (a glitched delta shouldn't desync the phase index).
        } else {
            rejected_count_ = 0;
            period_us_ = kPeriodEmaAlpha * delta + (1.0 - kPeriodEmaAlpha) * period_us_;
            UpdateEffectiveBpmFromPeriod();
        }

        // Phase servo: how far is our free-running phase from where this
        // clock "should" be, given the anchor established at lock time?
        ++clock_index_since_anchor_;
        const double expected_phase =
            anchor_phase_ +
            kInternalTicksPerMidiClock * static_cast<double>(clock_index_since_anchor_);
        const double raw_error = phase_ - expected_phase;
        phase_error_ = kPhaseErrorEmaAlpha * raw_error + (1.0 - kPhaseErrorEmaAlpha) * phase_error_;
    }

    // Advances the free-running phase by exactly one control tick. Must be
    // called once per 1 kHz tick regardless of sync state (INTERNAL mode
    // free-runs at nominal_bpm_; MIDI modes free-run at effective_bpm_,
    // trimmed by the phase servo while LOCKED).
    void Tick() {
        if (playing_) {
            double dphase = DphaseFromBpm(effective_bpm_);
            if (state_ == SyncLockState::Locked) {
                double trim = -kProportionalGain * phase_error_;
                // Manual clamp, not std::min/max: those take const T&, which
                // would odr-use the static constexpr member below and fail
                // to link without an out-of-class definition (this is a
                // header-only class with none).
                if (trim > kMaxTrimFraction)
                    trim = kMaxTrimFraction;
                if (trim < -kMaxTrimFraction)
                    trim = -kMaxTrimFraction;
                dphase *= (1.0 + trim);
            }
            phase_ += dphase;
        }

        // Freewheel-timeout detection: only meaningful once locked, using
        // this class's own frame counter against period_us_ (both derived
        // from the Daisy timeline - never mixed with esp_delta_us).
        if (state_ == SyncLockState::Locked) {
            const double elapsed_us = FramesToUs(frame_counter_ - last_clock_frame_);
            if (elapsed_us > kFreewheelTimeoutPeriods * period_us_) {
                state_ = SyncLockState::Freewheel;
            }
        }

        frame_counter_ += block_size_;
    }

    SyncLockState State() const { return state_; }
    bool IsPlaying() const { return playing_; }
    float MeasuredBpm() const { return static_cast<float>(effective_bpm_); }
    double PhaseTicks() const { return phase_; }
    double PhaseErrorTicks() const { return phase_error_; }
    uint64_t CurrentFrame() const { return frame_counter_; }

   private:
    static constexpr uint8_t kHistorySize = 5;
    static constexpr uint8_t kAcquireStableCount = 5;
    static constexpr double kStableToleranceFrac = 0.02;
    static constexpr double kOutlierToleranceFrac = 0.25;
    static constexpr uint8_t kRejectedHistorySize = 3;
    static constexpr double kRejectedConsistencyFrac = 0.02;
    static constexpr double kPeriodEmaAlpha = 1.0 / 8.0;
    static constexpr double kPhaseErrorEmaAlpha = 1.0 / 8.0;
    static constexpr double kProportionalGain = 0.02;  // fractional trim per internal tick of error
    static constexpr double kMaxTrimFraction = 0.005;  // clamp: +-0.5% rate trim
    static constexpr double kFreewheelTimeoutPeriods = 2.0;

    void ResetAcquisition() {
        history_count_ = 0;
        history_next_ = 0;
        rejected_count_ = 0;
    }

    void EnterLocked() {
        state_ = SyncLockState::Locked;
        phase_error_ = 0.0;
        anchor_phase_ = phase_;
        clock_index_since_anchor_ = 0;
    }

    void PushDeltaHistory(double delta) {
        history_[history_next_] = delta;
        history_next_ = static_cast<uint8_t>((history_next_ + 1) % kHistorySize);
        if (history_count_ < kHistorySize)
            ++history_count_;
    }

    double MedianOfHistory() const {
        if (history_count_ == 0)
            return period_us_;
        double buf[kHistorySize];
        for (uint8_t i = 0; i < history_count_; ++i)
            buf[i] = history_[i];
        // Insertion sort - history_count_ <= 5, this is cheaper and
        // simpler than pulling in <algorithm>'s sort for such a tiny N.
        for (uint8_t i = 1; i < history_count_; ++i) {
            double key = buf[i];
            int j = i - 1;
            while (j >= 0 && buf[j] > key) {
                buf[j + 1] = buf[j];
                --j;
            }
            buf[j + 1] = key;
        }
        return buf[history_count_ / 2];
    }

    bool IsHistoryStable(double tol_frac) const {
        if (history_count_ < kAcquireStableCount)
            return false;
        const double med = MedianOfHistory();
        for (uint8_t i = 0; i < history_count_; ++i) {
            if (std::fabs(history_[i] - med) > tol_frac * med)
                return false;
        }
        return true;
    }

    bool RejectedHistoryMutuallyConsistent() const {
        const double avg = AverageOfRejectedHistory();
        for (uint8_t i = 0; i < kRejectedHistorySize; ++i) {
            if (std::fabs(rejected_history_[i] - avg) > kRejectedConsistencyFrac * avg)
                return false;
        }
        return true;
    }

    double AverageOfRejectedHistory() const {
        double sum = 0.0;
        for (uint8_t i = 0; i < kRejectedHistorySize; ++i)
            sum += rejected_history_[i];
        return sum / static_cast<double>(kRejectedHistorySize);
    }

    void UpdateEffectiveBpmFromPeriod() {
        if (period_us_ <= 0.0)
            return;
        // 24 MIDI clocks/quarter note; period_us_ is time between clocks.
        effective_bpm_ = 60.0 * 1.0e6 / (24.0 * period_us_);
    }

    double DphaseFromBpm(double bpm) const {
        // Internal ticks per control tick (one audio block).
        return static_cast<double>(kInternalPpqnConst) * bpm / 60.0 *
               (static_cast<double>(block_size_) / static_cast<double>(sample_rate_));
    }

    double FramesToUs(uint64_t frames) const {
        return static_cast<double>(frames) * 1.0e6 / static_cast<double>(sample_rate_);
    }

    // Duplicated numerically from pattern.hpp's kInternalPpqn (96) rather
    // than included from it - this class has no other dependency on the
    // pattern data model and shouldn't gain one just for a constant.
    static constexpr uint16_t kInternalPpqnConst = 96;

    uint32_t sample_rate_ = 48000;
    uint16_t block_size_ = 48;
    bool use_midi_ = false;
    bool playing_ = false;
    SyncLockState state_ = SyncLockState::Unlocked;

    double nominal_bpm_ = 120.0;
    double effective_bpm_ = 120.0;
    double period_us_ = 0.0;

    double phase_ = 0.0;
    double anchor_phase_ = 0.0;
    uint32_t clock_index_since_anchor_ = 0;
    double phase_error_ = 0.0;

    uint64_t frame_counter_ = 0;
    uint64_t last_clock_frame_ = 0;

    double history_[kHistorySize] = {};
    uint8_t history_count_ = 0;
    uint8_t history_next_ = 0;

    double rejected_history_[kRejectedHistorySize] = {};
    uint32_t rejected_count_ = 0;
};

}  // namespace Sequencer
}  // namespace WaveX
