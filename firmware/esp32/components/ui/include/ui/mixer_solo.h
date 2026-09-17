#pragma once
#include <atomic>
#include <cstdint>

namespace wavex_ui {
// One UI-domain temporary selection shared by Sequencer and Mixer. Change it
// only after the complete mask has been accepted by the outgoing link queue.
// User mute targets belong to the engine and are never copied into this model.
class MixerSolo {
   public:
    // Comm task may publish reset intent; only UI accessors mutate selection.
    void RequestReset() { reset_.store(true, std::memory_order_release); }
    uint8_t Track() const {
        if (reset_.exchange(false, std::memory_order_acquire))
            track_ = 0xff;
        return track_;
    }
    bool Active() const { return Track() < 16; }
    bool Contains(uint8_t track) const { return Active() && track_ == track; }
    void Select(uint8_t track) {
        Track();
        track_ = track < 16 ? track : 0xff;
    }
    static uint16_t Mask(uint8_t track) {
        return track < 16 ? static_cast<uint16_t>(1u << track) : 0;
    }
    uint16_t Mask() const { return Mask(Track()); }

   private:
    mutable uint8_t track_ = 0xff;
    mutable std::atomic<bool> reset_{false};
};
inline MixerSolo mixerSolo;
}  // namespace wavex_ui
