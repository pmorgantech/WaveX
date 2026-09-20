#pragma once
#include "sequencer/pattern_data.hpp"
#include <algorithm>
#include <cmath>

namespace WaveX::Sequencer {
// Callback-owned key identities. A replacement invalidates its old capture,
// so a delayed release cannot overwrite the gate of the replacement lane.
class NoteRecorder {
   public:
    void Reset() {
        for (auto& k: keys_)
            k.active = false;
        for (auto& row: ages_)
            for (auto& age: row)
                age = 0;
        counter_ = 0;
    }
    void Target(uint8_t track, uint8_t step) {
        Reset();
        track_ = track;
        step_ = step;
    }
    uint8_t Track() const { return track_; }
    uint8_t StepIndex() const { return step_; }
    void InvalidateLane(uint8_t track, uint8_t step, uint8_t lane) {
        if (track != track_)
            return;
        for (auto& k: keys_)
            if (k.active && k.step == step && k.lane == lane)
                k.captured = false;
    }
    bool Input(Pattern& pattern,
               uint8_t mode,
               uint8_t quantize,
               bool playing,
               double tick,
               uint8_t source,
               uint8_t note,
               uint32_t serial,
               uint8_t velocity,
               uint16_t tracks) {
        if (!mode || track_ >= kMaxTracks)
            return false;
        if (!velocity) {
            bool changed = false, released = false;
            for (auto& k: keys_)
                if (k.active && k.source == source && k.note == note && k.serial == serial) {
                    if (k.captured && mode == 2) {
                        pattern.tracks[track_].steps[k.step].notes[k.lane].gate_ticks =
                            static_cast<uint16_t>(
                                std::clamp(std::floor(tick - k.tick + .5), 1.0, 32767.0));
                        changed = true;
                    }
                    k.active = false;
                    released = true;
                }
            bool held = false;
            for (const auto& k: keys_)
                held |= k.active;
            if (released && !held && mode == 1) {
                step_ = static_cast<uint8_t>((step_ + 1) % pattern.length);
            }
            return changed;
        }
        if (!(tracks & (1u << track_)) || ((mode == 2 || mode == 3) && !playing))
            return false;
        Key* key = nullptr;
        for (auto& k: keys_)
            if (!k.active) {
                key = &k;
                break;
            }
        if (!key)
            return false;  // fixed capacity: live monitoring still proceeds
        *key = {true, false, source, note, 0, 0, serial, tick};
        if (mode == 3)
            return false;
        auto& row = pattern.tracks[track_];
        row.melodic = true;
        const auto interval = StepIntervalTicks(pattern.scale);
        double capture_tick = std::max(0.0, tick);
        if (mode == 2) {
            if (quantize) {
                const double quantum = quantize == 2 ? interval * .5 : interval;
                capture_tick = std::floor(capture_tick / quantum + .5) * quantum;
            }
            const double grid = std::floor(capture_tick / interval);
            step_ = static_cast<uint8_t>(static_cast<uint64_t>(grid) % pattern.length);
        }
        auto& step = row.steps[step_];
        uint8_t lane = kNoteLanes;
        bool empty = true;
        for (uint8_t i = 0; i < kNoteLanes; ++i) {
            empty &= step.notes[i].velocity == 0;
            if (!step.notes[i].velocity && lane == kNoteLanes)
                lane = i;
        }
        if (lane == kNoteLanes) {
            lane = 0;
            for (uint8_t i = 1; i < kNoteLanes; ++i)
                if (ages_[step_][i] < ages_[step_][lane])
                    lane = i;
        }
        ages_[step_][lane] = ++counter_;
        for (auto& k: keys_)
            if (k.active && k.captured && k.step == step_ && k.lane == lane)
                k.captured = false;
        if (empty && mode == 2)
            step.micro_offset = static_cast<int16_t>(std::lround(
                std::fmod(capture_tick, interval) -
                (!quantize && step_ % 2 ? interval * (pattern.swing - 50) / 50.0 : 0.0)));
        step.on = true;
        step.notes[lane] = {note, velocity, interval};
        key->captured = true;
        key->step = step_;
        key->lane = lane;
        return true;
    }
    template <typename Released>
    void Prune(Released released) {
        for (auto& k: keys_)
            if (k.active && released(k.source, k.note, k.serial))
                k.active = false;
    }
    bool Erase(Pattern& pattern, uint8_t step) {
        bool changed = false;
        for (auto& n: pattern.tracks[track_].steps[step].notes)
            for (const auto& k: keys_)
                if (k.active && n.velocity && n.note == k.note) {
                    n.velocity = 0;
                    changed = true;
                    break;
                }
        return changed;
    }

   private:
    struct Key {
        bool active = false, captured = false;
        uint8_t source = 0, note = 0, step = 0, lane = 0;
        uint32_t serial = 0;
        double tick = 0;
    };
    Key keys_[64]{};
    uint8_t track_ = 0, step_ = 0;
    uint32_t counter_ = 0, ages_[kMaxSteps][kNoteLanes]{};
};
}  // namespace WaveX::Sequencer
