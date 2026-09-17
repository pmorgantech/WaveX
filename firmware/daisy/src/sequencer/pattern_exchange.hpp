#pragma once
#include "sequencer_transport.hpp"
#include <atomic>

namespace WaveX {
namespace Sequencer {
// One exchange-owned buffer at a time: foreground fills an install or reads a
// completed capture; the callback owns it only between request and completion.
// Capture copies one row per block and retries if any intervening edit occurs.
// Launch swaps this buffer with the transport working buffer. Both objects and
// their original storage must have engine lifetime; neither may be moved.
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
        Paused,
        Running,
        Launch,
        Launching,
        Launched,
        Cancelled,
        SongStart,
        SongPlaying,
        SongEnded
    };
    State state() const { return state_.load(std::memory_order_acquire); }
    Pattern& foreground() { return *buffer_; }  // only Idle / Captured
    bool Capture(bool stopped_only = false) {
        if (state() != State::Idle)
            return false;
        stopped_only_ = stopped_only;
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
    bool InstallSession(const Protocol::SeqTransportMessage& settings, uint8_t slot = 0xff) {
        if (state() != State::Idle)
            return false;
        slot_ = slot;
        settings_ = settings;
        settings_.command = Protocol::SEQ_TRANSPORT_STOP;
        install_settings_ = true;
        state_.store(State::Install, std::memory_order_release);
        return true;
    }
    bool Install(uint8_t slot = 0xff) {
        if (state() != State::Idle)
            return false;
        slot_ = slot;
        install_settings_ = false;
        state_.store(State::Install, std::memory_order_release);
        return true;
    }
    bool Launch(uint8_t slot) {
        if (state() != State::Idle)
            return false;
        slot_ = slot;
        state_.store(State::Launch, std::memory_order_release);
        return true;
    }
    bool PlaySong(const Project* project, uint8_t song, uint8_t entry, bool loop) {
        if (state() != State::Idle)
            return false;
        song_project_ = project;
        song_slot_ = song;
        song_entry_ = entry;
        song_loop_ = loop;
        song_stop_.store(false, std::memory_order_relaxed);
        song_position_.store(0, std::memory_order_relaxed);
        state_.store(State::SongStart, std::memory_order_release);
        return true;
    }
    void StopSong() { song_stop_.store(true, std::memory_order_release); }
    uint32_t SongPosition() const { return song_position_.load(std::memory_order_acquire); }
    void Retire() {  // only completed states; never cancel callback ownership
        const auto s = state();
        if (s == State::Captured || s == State::Installed || s == State::Failed ||
            s == State::Paused || s == State::Running || s == State::Launched ||
            s == State::Cancelled || s == State::SongEnded)
            state_.store(State::Idle, std::memory_order_release);
    }
    // Called after scheduling. A successful install discards that block's old
    // pattern events. Save rows wait for a block without scheduled triggers.
    bool Process(SequencerTransport& transport, bool allow_capture = true) {
        const auto s = state_.load(std::memory_order_acquire);
        if (s == State::SongStart) {
            const bool started =
                transport.StartSong(song_project_, song_slot_, song_entry_, song_loop_);
            song_position_.store(transport.SongPosition(), std::memory_order_release);
            state_.store(started ? State::SongPlaying : State::Running, std::memory_order_release);
            return started;
        }
        if (s == State::SongPlaying) {
            const bool stop = song_stop_.load(std::memory_order_acquire);
            if (stop)
                transport.StopSong();
            song_position_.store(transport.SongPosition(), std::memory_order_release);
            if (!transport.SongActive())
                state_.store(State::SongEnded, std::memory_order_release);
            return stop;
        }
        if (s == State::Launch) {
            if (!transport.LaunchPattern(buffer_, slot_))
                state_.store(State::Running, std::memory_order_release);
            else
                state_.store(transport.LaunchPending() ? State::Launching : State::Launched,
                             std::memory_order_release);
            return false;
        }
        if (s == State::Launching) {
            if (!transport.LaunchPending())
                state_.store(transport.LaunchCancelled() ? State::Cancelled : State::Launched,
                             std::memory_order_release);
            return false;
        }
        if (s == State::Pause) {
            transport.StopForProject();
            state_.store(State::Paused, std::memory_order_release);
            return true;
        }
        if (s == State::Install) {
            transport.ReplacePattern(*buffer_);
            if (slot_ != 0xff)
                transport.SetPatternSlot(slot_);
            if (install_settings_)
                transport.ApplyTransport(settings_);
            state_.store(State::Installed, std::memory_order_release);
            return true;
        } else if (s == State::Capture) {
            if (stopped_only_ && (transport.IsPlaying() || transport.IsArmed())) {
                state_.store(State::Running, std::memory_order_release);
                return false;
            }
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
                buffer_->length = transport.pattern().length;
                buffer_->scale = transport.pattern().scale;
                buffer_->swing = transport.pattern().swing;
            }
            buffer_->tracks[rows_] = transport.pattern().tracks[rows_];
            if (++rows_ == kMaxTracks) {
                settings_ = transport.SessionSettings();
                state_.store(State::Captured, std::memory_order_release);
            }
        }
        return false;
    }

   private:
    const Project* song_project_ = nullptr;
    uint8_t song_slot_ = 0, song_entry_ = 0;
    bool song_loop_ = false;
    std::atomic<bool> song_stop_{false};
    std::atomic<uint32_t> song_position_{0};
    Pattern pattern_;
    Pattern* buffer_ = &pattern_;
    uint8_t slot_ = 0xff;
    Protocol::SeqTransportMessage settings_;
    bool install_settings_ = false, stopped_only_ = false;
    std::atomic<State> state_{State::Idle};
    uint32_t revision_ = 0;
    uint16_t ticks_ = 0;
    uint8_t rows_ = 0;
};
static_assert(std::atomic<PatternExchange::State>::is_always_lock_free,
              "callback exchange must be lock-free");
}  // namespace Sequencer
}  // namespace WaveX
