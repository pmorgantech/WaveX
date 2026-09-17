#pragma once
#include "sequencer_transport.hpp"
#include <atomic>

namespace WaveX {
namespace Sequencer {
// One buffer, exclusive ownership: foreground fills an install or reads a
// completed capture; the callback owns it only between request and completion.
// Capture copies one row per block and retries if any intervening edit occurs.
class PatternExchange {
   public:
    enum class State : uint32_t {
        Idle,
        Capture,
        Captured,
        Install,
        Installed,
        Failed,
        Pause,
        Paused
    };
    State state() const { return state_.load(std::memory_order_acquire); }
    Pattern& foreground() { return pattern_; }  // only Idle / Captured
    bool Capture() {
        if (state() != State::Idle)
            return false;
        rows_ = 0;
        ticks_ = 0;
        state_.store(State::Capture, std::memory_order_release);
        return true;
    }
    const Protocol::SeqTransportMessage& capturedSettings() const { return settings_; }
    bool Pause() {
        if (state() != State::Idle)
            return false;
        state_.store(State::Pause, std::memory_order_release);
        return true;
    }
    bool InstallSession(const Protocol::SeqTransportMessage& settings) {
        if (state() != State::Idle)
            return false;
        settings_ = settings;
        settings_.command = Protocol::SEQ_TRANSPORT_STOP;
        install_settings_ = true;
        state_.store(State::Install, std::memory_order_release);
        return true;
    }
    bool Install() {
        if (state() != State::Idle)
            return false;
        install_settings_ = false;
        state_.store(State::Install, std::memory_order_release);
        return true;
    }
    void Retire() {  // only completed states; never cancel callback ownership
        const auto s = state();
        if (s == State::Captured || s == State::Installed || s == State::Failed ||
            s == State::Paused)
            state_.store(State::Idle, std::memory_order_release);
    }
    // Called after scheduling. A successful install discards that block's old
    // pattern events. Save rows wait for a block without scheduled triggers.
    bool Process(SequencerTransport& transport, bool allow_capture = true) {
        const auto s = state_.load(std::memory_order_acquire);
        if (s == State::Pause) {
            transport.StopForProject();
            state_.store(State::Paused, std::memory_order_release);
            return true;
        }
        if (s == State::Install) {
            transport.ReplacePattern(pattern_);
            if (install_settings_)
                transport.ApplyTransport(settings_);
            state_.store(State::Installed, std::memory_order_release);
            return true;
        } else if (s == State::Capture) {
            if (++ticks_ > 500) {
                state_.store(State::Failed, std::memory_order_release);
                return false;
            }
            if (!allow_capture)
                return false;
            const auto revision = transport.PatternRevision();
            if (!rows_ || revision != revision_) {
                rows_ = 0;
                revision_ = revision;
                pattern_.length = transport.pattern().length;
                pattern_.scale = transport.pattern().scale;
                pattern_.swing = transport.pattern().swing;
            }
            pattern_.tracks[rows_] = transport.pattern().tracks[rows_];
            if (++rows_ == kMaxTracks) {
                settings_ = transport.SessionSettings();
                state_.store(State::Captured, std::memory_order_release);
            }
        }
        return false;
    }

   private:
    Pattern pattern_;
    Protocol::SeqTransportMessage settings_;
    bool install_settings_ = false;
    std::atomic<State> state_{State::Idle};
    uint32_t revision_ = 0;
    uint16_t ticks_ = 0;
    uint8_t rows_ = 0;
};
static_assert(std::atomic<PatternExchange::State>::is_always_lock_free,
              "callback exchange must be lock-free");
}  // namespace Sequencer
}  // namespace WaveX
