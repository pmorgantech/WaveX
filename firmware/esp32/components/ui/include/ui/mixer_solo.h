#pragma once
#include <cstdint>

namespace wavex_ui {
// One UI-domain temporary selection shared by Sequencer and Mixer. Change it
// only after the complete mask has been accepted by the outgoing link queue.
// User mute targets belong to the engine and are never copied into this model.
class MixerSolo {
   public:
    uint8_t Track() const { return track_; }
    bool Active() const { return track_ < 16; }
    bool Contains(uint8_t track) const { return Active() && track_ == track; }
    void Select(uint8_t track) { track_ = track < 16 ? track : 0xff; }
    static uint16_t Mask(uint8_t track) {
        return track < 16 ? static_cast<uint16_t>(1u << track) : 0;
    }
    uint16_t Mask() const { return Mask(track_); }

   private:
    uint8_t track_ = 0xff;
};
inline MixerSolo mixerSolo;
}  // namespace wavex_ui
