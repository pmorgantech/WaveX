#pragma once
#include "audio/arp_config.hpp"
#include "audio/live_note_queue.hpp"
#include <algorithm>
#include <cstdint>

namespace WaveX::Sequencer {
// Callback-owned, bounded chord model. Physical press identity survives repeated
// pitches; latch replaces the old chord only on the first new all-keys-up press.
class Arpeggiator {
   public:
    struct Note {
        uint8_t note = 0, velocity = 0;
    };
    void Configure(const Arp::Config& config) {
        if (!Arp::Valid(config))
            return;
        if (!config.enabled || config.latch != config_.latch)
            Clear();
        config_ = config;
    }
    const Arp::Config& Config() const { return config_; }
    void Clear() {
        count_ = 0;
        step_ = 0;
    }
    void Restart(uint32_t seed = 1) {
        step_ = 0;
        random_ = seed ? seed : 1;
    }
    bool Press(AudioEngine::LiveNoteId id, uint8_t velocity) {
        if (!config_.enabled || !velocity || id.note > 127)
            return false;
        bool any_down = false;
        for (uint8_t i = 0; i < count_; ++i) {
            if (keys_[i].id.Matches(id))
                return true;
            any_down |= keys_[i].down;
        }
        if (config_.latch && !any_down)
            Clear();
        if (count_ == 16)
            return false;
        keys_[count_++] = {id, velocity, true};
        return true;
    }
    void Release(AudioEngine::LiveNoteId id) {
        for (uint8_t i = 0; i < count_; ++i)
            if (keys_[i].id.Matches(id)) {
                if (config_.latch)
                    keys_[i].down = false;
                else {
                    for (uint8_t j = i + 1; j < count_; ++j)
                        keys_[j - 1] = keys_[j];
                    --count_;
                }
                return;
            }
    }
    template <typename Released>
    void Prune(Released released) {
        for (uint8_t i = 0; i < count_;) {
            if (keys_[i].down && released(keys_[i].id)) {
                const auto before = count_;
                Release(keys_[i].id);
                if (count_ != before)
                    continue;
            }
            ++i;
        }
    }
    uint8_t Count() const { return count_; }
    Note Next() {
        if (!config_.enabled || !count_)
            return {};
        uint8_t order[16];
        for (uint8_t i = 0; i < count_; ++i)
            order[i] = i;
        if (config_.mode != 4)
            std::sort(order, order + count_, [&](uint8_t a, uint8_t b) {
                return keys_[a].id.note < keys_[b].id.note ||
                       (keys_[a].id.note == keys_[b].id.note && a < b);
            });
        // Materialize only MIDI-representable pitches; octave overflow never wraps.
        Note notes[64];
        uint8_t n = 0;
        for (uint8_t octave = 0; octave < config_.octaves; ++octave)
            for (uint8_t i = 0; i < count_; ++i) {
                const auto& key = keys_[order[i]];
                const unsigned pitch = key.id.note + 12u * octave;
                if (pitch <= 127)
                    notes[n++] = {static_cast<uint8_t>(pitch), key.velocity};
            }
        uint32_t index = step_ % n;
        switch (config_.mode) {
            case 1:
                index = n - 1 - index;
                break;
            case 2:
                index = step_ % (2u * n);
                if (index >= n)
                    index = 2u * n - 1 - index;
                break;
            case 3:
                if (n > 1) {
                    index = step_ % (2u * n - 2);
                    if (index >= n)
                        index = 2u * n - 2 - index;
                }
                break;
            case 5:
                random_ ^= random_ << 13;
                random_ ^= random_ >> 17;
                random_ ^= random_ << 5;
                index = random_ % n;
                break;
            default:
                break;
        }
        auto note = notes[index];
        if (config_.vel_mode == 1)
            note.velocity = config_.vel_fixed;
        if (config_.vel_mode == 2)
            note.velocity = static_cast<uint8_t>(
                std::max<uint32_t>(1u, 127u - 126u * (step_ % n) / std::max(1u, unsigned(n) - 1u)));
        ++step_;
        return note;
    }

   private:
    struct Key {
        AudioEngine::LiveNoteId id;
        uint8_t velocity = 0;
        bool down = false;
    };
    Key keys_[16]{};
    Arp::Config config_;
    uint8_t count_ = 0;
    uint32_t step_ = 0, random_ = 1;
};
}  // namespace WaveX::Sequencer
