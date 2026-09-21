#pragma once
#include "sequencer/arpeggiator.hpp"
#include "sequencer_voice_map.hpp"
#include <cmath>

namespace WaveX::AudioEngine {
// Callback-owned. Instruments publish configurations through the existing
// immutable prepared map. Groups/deadlines belong only to generated arp notes.
class ArpRuntime {
   public:
    template <typename Record>
    void Sync(const SequencerVoiceMap* map, VoiceManager& voices, Record record) {
        for (uint8_t t = 0; t < kNumTracks; ++t) {
            const auto config = map ? map->tracks[t].arp : Arp::Config{};
            if (!Arp::Equal(config, tracks_[t].arp.Config())) {
                if (!config.enabled) {
                    End(t, 0, voices, record);
                    tracks_[t].scheduled = false;
                }
                tracks_[t].arp.Configure(config);
            }
        }
    }
    void Input(LiveNoteEvent& event) {
        for (uint8_t t = 0; t < kNumTracks; ++t) {
            auto& arp = tracks_[t].arp;
            if (!event.velocity)
                arp.Release(event.id);
            else if (arp.Config().enabled && (event.tracks & (1u << t))) {
                arp.Press(event.id, event.velocity);
                event.tracks &= static_cast<uint16_t>(~(1u << t));
            }
        }
    }
    template <typename Released>
    void Prune(Released released) {
        for (auto& t: tracks_)
            t.arp.Prune(released);
    }
    template <typename Record>
    void Stop(uint16_t mask, VoiceManager& voices, Record record) {
        for (uint8_t t = 0; t < kNumTracks; ++t)
            if (mask & (1u << t)) {
                End(t, 0, voices, record);
                tracks_[t].arp.Clear();
                tracks_[t].scheduled = false;
            }
    }
    // Called once per audio block after transport commands have been consumed.
    // Free-run derives phase from an integer frame clock, reanchored on tempo
    // changes. Running transport uses its own frame conversion without drift.
    template <typename Scheduler, typename Record>
    size_t Events(const Scheduler& scheduler,
                  uint64_t block_frame,
                  double start_tick,
                  uint16_t frames,
                  bool playing,
                  uint32_t epoch,
                  Sequencer::TriggerEvent* out,
                  VoiceManager& voices,
                  Record record) {
        const double fpt = 48000.0 * 60.0 / (scheduler.Tempo() * 96.0);
        if (playing != playing_ || epoch != epoch_) {
            for (uint8_t t = 0; t < kNumTracks; ++t) {
                End(t, 0, voices, record);
                tracks_[t].arp.Restart(t + 1);
                tracks_[t].scheduled = false;
            }
            free_frames_ = 0;
            free_anchor_ = 0;
            free_tick_ = 0;
            playing_ = playing;
            epoch_ = epoch;
        }
        if (fpt != free_fpt_) {
            free_tick_ += double(free_frames_ - free_anchor_) / free_fpt_;
            free_anchor_ = free_frames_;
            free_fpt_ = fpt;
        }
        start_ = playing ? start_tick : free_tick_ + double(free_frames_ - free_anchor_) / fpt;
        per_frame_ = 1.0 / fpt;
        const double end = start_ + frames * per_frame_;
        size_t n = 0;
        for (uint8_t t = 0; t < kNumTracks; ++t) {
            auto& state = tracks_[t];
            const auto& config = state.arp.Config();
            if (!config.enabled || !state.arp.Count()) {
                End(t, 0, voices, record);
                state.scheduled = false;
                continue;
            }
            const auto division = Arp::kTicks[config.division];
            if (!state.scheduled) {
                state.next = std::ceil((start_ - 1e-9) / division) * division;
                state.scheduled = true;
            }
            if (state.next < start_ - 1e-6)
                state.next = std::ceil(start_ / division) * division;
            const auto frame =
                playing ? scheduler.FrameAtTick(state.next)
                        : free_anchor_ +
                              static_cast<uint64_t>(std::llround((state.next - free_tick_) * fpt));
            const auto origin = playing ? block_frame : free_frames_;
            if (frame >= origin && frame - origin < frames && state.next < end + 1e-8) {
                const auto note = state.arp.Next();
                auto& event = out[n++];
                event = {};
                event.arp = true;
                event.track = t;
                event.frame = block_frame + frame - origin;
                event.tick = state.next;
                event.note = note.note;
                event.velocity = note.velocity;
                event.gate_ticks = static_cast<uint16_t>(
                    std::max(1u, unsigned(division) * config.gate_pct / 100u));
                state.next += division;
            }
        }
        free_frames_ += frames;
        return n;  // At most one per Track: minimum division is far above one block.
    }
    template <typename Record>
    void Admit(const Sequencer::TriggerEvent& event,
               uint64_t group,
               VoiceManager& voices,
               Record record) {
        End(event.track, 0, voices, record);
        auto& state = tracks_[event.track];
        state.group = group;
        state.gate = event.tick + event.gate_ticks;
        state.note = event.note;
        if (++serial_ == 0)
            ++serial_;
        state.serial = serial_;
        if (group)
            record(uint8_t(32 + event.track),
                   event.note,
                   state.serial,
                   event.velocity,
                   uint16_t(1u << event.track));
    }
    template <typename Record>
    void Gates(uint16_t offset, uint16_t frames, VoiceManager& voices, Record record) {
        const double start = start_ + offset * per_frame_;
        for (uint8_t t = 0; t < kNumTracks; ++t) {
            auto& state = tracks_[t];
            if (state.group && state.gate < start + frames * per_frame_)
                End(t,
                    static_cast<uint16_t>(
                        std::max(0.0, std::round((state.gate - start) / per_frame_))),
                    voices,
                    record);
        }
    }

   private:
    template <typename Record>
    void End(uint8_t track, uint16_t offset, VoiceManager& voices, Record record) {
        auto& state = tracks_[track];
        if (state.group) {
            voices.ReleaseGroupAt(state.group, offset);
            record(uint8_t(32 + track), state.note, state.serial, 0, uint16_t(1u << track));
            state.group = 0;
        }
    }
    struct Track {
        Sequencer::Arpeggiator arp;
        double next = 0, gate = 0;
        uint64_t group = 0;
        uint32_t serial = 0;
        uint8_t note = 0;
        bool scheduled = false;
    } tracks_[kNumTracks];
    bool playing_ = false;
    uint32_t epoch_ = 0, serial_ = 0;
    uint64_t free_frames_ = 0, free_anchor_ = 0;
    double free_tick_ = 0, free_fpt_ = 250, start_ = 0, per_frame_ = 1.0 / 250;
};
}  // namespace WaveX::AudioEngine
