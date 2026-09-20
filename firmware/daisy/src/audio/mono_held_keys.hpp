#pragma once
#include "audio/live_note_id.hpp"
#include <cstdint>

namespace WaveX::AudioEngine {
// Callback-owned physical presses, independent of render slots and group stealing.
// A full ledger refuses the new Mono request rather than discarding an older key.
class MonoHeldKeys {
   public:
    static constexpr uint8_t kCapacity = 64, kNone = 0xff;
    struct Key {
        LiveNoteId id;
        uint64_t age = 0;
        uint8_t track = 0, velocity = 0;
    };
    void Init() {
        for (auto& key: keys_)
            key = {};
        for (auto& top: top_)
            top = kNone;
        age_ = 0;
        pending_ = 0;
    }
    bool Available() const {
        for (const auto& key: keys_)
            if (!key.velocity)
                return true;
        return false;
    }
    void Add(uint8_t track, LiveNoteId id, uint8_t velocity) {
        for (uint8_t i = 0; i < kCapacity; ++i) {
            auto& key = keys_[i];
            if (!key.velocity) {
                key = {id, ++age_, track, velocity};
                top_[track] = i;
                pending_ &= static_cast<uint16_t>(~(1u << track));
                return;
            }
        }
    }
    template <class Released>
    void Remove(Released released) {
        uint16_t retop = 0;
        for (uint8_t i = 0; i < kCapacity; ++i) {
            auto& key = keys_[i];
            if (key.velocity && released(key.id)) {
                if (top_[key.track] == i)
                    retop |= static_cast<uint16_t>(1u << key.track);
                key.velocity = 0;
            }
        }
        for (uint8_t t = 0; t < 16; ++t)
            if (retop & (1u << t))
                top_[t] = kNone;
        for (uint8_t i = 0; i < kCapacity; ++i) {
            const auto& key = keys_[i];
            if (key.velocity && (retop & (1u << key.track)) &&
                (top_[key.track] == kNone || key.age > keys_[top_[key.track]].age))
                top_[key.track] = i;
        }
        pending_ |= retop;
    }
    void Clear(uint16_t tracks) {
        for (auto& key: keys_)
            if (tracks & (1u << key.track))
                key.velocity = 0;
        for (uint8_t t = 0; t < 16; ++t)
            if (tracks & (1u << t))
                top_[t] = kNone;
        pending_ &= static_cast<uint16_t>(~tracks);
    }
    const Key* Fallback(uint8_t track) const {
        return (pending_ & (1u << track)) && top_[track] != kNone ? &keys_[top_[track]] : nullptr;
    }
    void FinishFallbacks() { pending_ = 0; }

   private:
    Key keys_[kCapacity]{};
    uint64_t age_ = 0;
    uint8_t top_[16]{};
    uint16_t pending_ = 0;
};
}  // namespace WaveX::AudioEngine
