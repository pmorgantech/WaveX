#pragma once
#include "instrument.hpp"
#include <cstring>

namespace WaveX {
namespace AudioEngine {
namespace KitEdit {
// File-safe, nonempty ASCII names; one source for UI/wire validation is in
// protocol.h. This helper tests whether a map can be edited without hiding
// imported ranges/layers outside the sixteen fixed pads.
inline bool Editable(const Instrument& ins) {
    if (ins.origin == InstrumentOrigin::None || ins.mode != InstrumentMode::Drum)
        return false;
    for (uint8_t i = 0; i < kMaxZones; ++i) {
        const auto& z = ins.zones[i];
        if (!z.in_use)
            continue;
        if (i >= Protocol::INST_PAD_COUNT || z.key_lo != Protocol::INST_PAD_FIRST_NOTE + i ||
            z.key_hi != z.key_lo || z.vel_lo != 1 || z.vel_hi != 127)
            return false;
    }
    return true;
}
inline void Assign(Instrument& ins, uint8_t pad, uint16_t sample, uint8_t choke) {
    auto& z = ins.zones[pad];
    // Sample assignment preserves that pad's edits; removal resets its zone.
    if (!sample) {
        z = Zone{};
        return;
    }
    if (!z.in_use)
        z.loop_mode = ZONE_LOOP_OFF;
    z.in_use = true;
    z.sample_id = sample;
    z.key_lo = z.key_hi = static_cast<uint8_t>(Protocol::INST_PAD_FIRST_NOTE + pad);
    z.root_note = 60;
    z.vel_lo = 1;
    z.vel_hi = 127;
    z.choke_group = choke;
    z.flags |= ZONE_FLAG_ONE_SHOT;
}
inline bool Uses(const Instrument& ins, uint16_t sample) {
    for (const auto& z: ins.zones)
        if (z.in_use && z.sample_id == sample)
            return true;
    return false;
}
}  // namespace KitEdit
}  // namespace AudioEngine
}  // namespace WaveX
