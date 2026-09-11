#pragma once
#include "instrument.hpp"
#include <cmath>
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
// Effective readback is quantized for controls only; an untouched float
// field keeps its exact stored value when a different field is edited.
inline uint16_t SoundWire(float value, float scale, uint16_t maximum) {
    if (!std::isfinite(value) || value <= 0.0f)
        return 0;
    const float scaled = value * scale;
    return scaled >= maximum ? maximum : static_cast<uint16_t>(scaled + 0.5f);
}
inline void ReadSound(const Instrument& ins, uint8_t pad, Protocol::InstPadSoundSyncMessage& out) {
    if (pad >= Protocol::INST_PAD_COUNT || !Editable(ins) || !ins.zones[pad].in_use)
        return;
    const auto& z = ins.zones[pad];
    out.valid = 1;
    out.sample_id = z.sample_id;
    out.own = (z.flags & ZONE_FLAG_OWN_FILTER_ENV) != 0;
    out.cutoff_hz = SoundWire(out.own ? z.cutoff_hz : ins.filter.cutoff_hz, 1, 20000);
    out.attack_ms = SoundWire(out.own ? z.attack_s : ins.env.attack_s, 1000, 10000);
    out.decay_ms = SoundWire(out.own ? z.decay_s : ins.env.decay_s, 1000, 10000);
    out.sustain = SoundWire(out.own ? z.sustain : ins.env.sustain, 1000, 1000);
}
inline bool SetSound(Instrument& ins, const Protocol::InstPadSoundOpMessage& m) {
    using namespace Protocol;
    if (!IsValidPadSoundOp(m) || m.op == PAD_SOUND_GET || !Editable(ins))
        return false;
    auto& z = ins.zones[m.pad];
    if (!z.in_use || z.sample_id != m.sample_id)
        return false;
    if (m.op == PAD_SOUND_INHERIT) {
        z.flags &= static_cast<uint8_t>(~ZONE_FLAG_OWN_FILTER_ENV);
        return true;
    }
    if (!(z.flags & ZONE_FLAG_OWN_FILTER_ENV)) {
        z.cutoff_hz = ins.filter.cutoff_hz;
        z.attack_s = ins.env.attack_s;
        z.decay_s = ins.env.decay_s;
        z.sustain = ins.env.sustain;
        z.release_s = ins.env.release_s;
        z.flags |= ZONE_FLAG_OWN_FILTER_ENV;
    }
    switch (m.op) {
        case PAD_SOUND_CUTOFF:
            z.cutoff_hz = m.value;
            break;
        case PAD_SOUND_ATTACK:
            z.attack_s = m.value / 1000.0f;
            break;
        case PAD_SOUND_DECAY:
            z.decay_s = m.value / 1000.0f;
            break;
        case PAD_SOUND_SUSTAIN:
            z.sustain = m.value / 1000.0f;
            break;
        default:
            return false;
    }
    return true;
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
