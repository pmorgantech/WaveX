#pragma once

// Stage A paraphonic envelope law (roadmap Phase 1 item 5;
// docs/features/analog-voice-board.md §0): the ONE shared analog VCF/VCA
// pair needs one shared envelope, and the classic paraphonic behavior is:
//
//   - retrigger on EVERY note-on (each new note re-articulates the shared
//     filter/amp, ramping from the current level - no click), and
//   - enter release only when the LAST held voice releases.
//
// Runs at the 1 kHz control tick, not the audio rate: its output becomes
// CV values (cutoff modulation, VCA level) staged for the MCP4728, which
// updates once per tick anyway. Per-voice digital envelopes in
// voice_manager.hpp still shape each voice's level before the sum, which
// keeps the mix articulate even with one analog VCA.
//
// "Held" comes from the voice manager (active voices not yet releasing),
// so a non-looping sample reaching its end counts as released - the shared
// envelope follows what is actually sounding, not the raw MIDI key state.
//
// HAL-free (wraps the linear Envelope at tick rate), host-testable:
// tests/unit/audio/paraphonic_envelope_test.cpp.

#include "envelope.hpp"
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

class ParaphonicEnvelope {
   public:
    // `tick_rate_hz` is the control-tick rate (1000 for the 1 ms tick).
    void Init(uint32_t tick_rate_hz) { env_.Init(tick_rate_hz); }

    // Attack/decay/release in seconds, sustain 0..1 (same contract as
    // Envelope::SetParams, interpreted at the tick rate).
    void SetParams(float attack_s, float decay_s, float sustain_level, float release_s) {
        env_.SetParams(attack_s, decay_s, sustain_level, release_s);
    }

    // Advances one control tick and returns the envelope level (0..1).
    //  - note_on_edge: a note was triggered since the last tick (drained
    //    from the note queue in the same audio-callback context).
    //  - any_held: the voice manager still has at least one active,
    //    not-yet-releasing voice.
    float Tick(bool note_on_edge, bool any_held) {
        if (note_on_edge) {
            env_.Retrigger();
        } else if (!any_held && !env_.IsIdle() && !env_.IsReleasing()) {
            env_.Release();
        }
        return env_.Process();
    }

    float Level() const { return env_.Level(); }
    bool IsIdle() const { return env_.IsIdle(); }
    bool IsReleasing() const { return env_.IsReleasing(); }

   private:
    Envelope env_;
};

}  // namespace AudioEngine
}  // namespace WaveX
