# Arpeggiator

**Status:** implemented in the Instrument Arp tab, with host and selected
board checks. Physical MIDI timing, listening, persistence combinations and
full-load acceptance remain open in [HV-031](../hardware-validation.md#hv-031--arpeggiator).
Implementation was authorized ahead of callback-capacity remediation; it does
not close the existing 85.3713% finding or a phase gate.

## Contents

- [Model and lifetime](#model-and-lifetime)
- [Editing and persistence](#editing-and-persistence)
- [Related](#related)

## Model and lifetime

Each Track's prepared Instrument snapshot includes one `Arp::Config`, defined
in [arp_config.hpp](../../firmware/shared/audio/arp_config.hpp). The eight controls
are enable, mode, division, octaves, gate, latch, velocity mode and fixed velocity.
Six modes provide up, down, inclusive/exclusive up-down, as-played and seeded
random order. Up to sixteen held press identities expand over one to four
octaves; overflow refuses another held entry without corrupting existing keys.
Latch keeps the released chord until the first new press after all keys are up.

The callback intercepts enabled Track note input before prepared zone
resolution. Generated notes use the existing voice budget and group identity;
gate expiry releases only the admitted arp group. A stale physical release
cannot stop a newer press with the same pitch. Disable, rebind and stop fences
retire arp-owned groups through the same voice lifetime rules.

The HAL-free generator lives in `sequencer/arpeggiator.hpp`; `audio/arp_runtime.hpp`
adds bounded Track state, integer frame scheduling and voice-group gates. Arp
notes join the chronologically rendered sequencer events. Running transport uses
the sequencer's straight 96-PPQN timebase, excluding swing. Stopped transport uses
an integer free-running grid at the current tempo. Generated note-on/off pairs
feed the melodic live recorder with their own identities.

## Editing and persistence

Instrument → Arp exposes all eight fields. Changes preview automatically and
share the Instrument sound revision and Apply/Revert lifetime. The optional WXI
arp chunk preserves settings; older files default to disabled. The request/sync
pair is defined in [protocol.h](../../firmware/shared/spi_protocol/protocol.h) and
mirrored in the [protocol reference](inter-mcu-protocol.md#recording-arpeggiator-and-performance-controls).
Retained completion IDs support read recovery without replaying mutations.

Host tests cover mode endpoints, octave ordering, latch/press identity, owned
release groups, persistence, revisions and ten-minute exact frame grids for
free-run and transport. The 2026-09-21 HIL check exercised Arp edits,
Apply/Revert and generated voice admission. Host grids do not prove ten-minute
physical MIDI-clock alignment; hearing every mode, recording generated notes,
save/reload combinations and sustained callback pressure remain in HV-031.

## Related

- [Roadmap](../roadmap.md)
- [Melodic sequencing](melodic-sequencing.md)
- [Track and Instrument model](track-and-patch-model.md)
- [Callback measurements](../callback-performance-log.md)
