# Feature Expansion — Index & Sequencing

**Status**: Index over the 2026-07-05 feature-design suite. Each feature has its own design doc (linked below) written to hand off directly to implementation; this file only carries the map, the shared wire-contract reservations, and the sequencing rationale. Roadmap integration lives in `../roadmap.md` (Phase 2 refs, new Phase 2.5, Phase 4/5 additions).

**Design north star**: E-mu Emax / Emulator III lineage. WaveX's Stage B path (sample → SSI2144 VCF → SSI2164 VCA per voice) is the Emax voice architecture; these designs supply the instrument/preset layer, sampling workflow, and realtime-control routing that made those machines instruments rather than sample players.

## Design docs

| Doc | What it covers | Roadmap slot |
|---|---|---|
| [`instrument-model.md`](instrument-model.md) | Presets/zones, multisample keymaps, velocity layers & crossfade, choke, WXCF file container, kit unification | Phase 2.5 (load-bearing; most others depend on it) |
| [`midi-sync-tempo-follower.md`](midi-sync-tempo-follower.md) | MIDI clock in (PLL follower, clock-domain-safe), clock out, transport/SPP | Phase 2 (gate requirement) |
| [`melodic-sequencing.md`](melodic-sequencing.md) | Melodic track type, chords, ties, step-record, live record/overdub/erase | Phase 2.5 |
| [`param-locks-and-modulation.md`](param-locks-and-modulation.md) | P-lock application path, mod matrix (8 slots/instrument), filter envelope, LFOs, param id space | Phase 2 (p-locks) + 2.5 (matrix/LFO) |
| [`sampling-and-recording.md`](sampling-and-recording.md) | Threshold-armed capture w/ pre-roll, resample/bounce, audition-before-save, non-destructive auto-trim, assign-to-zone | Phase 2.5 |
| [`arpeggiator.md`](arpeggiator.md) | Per-slot arp (modes, latch, clock-synced), feeds live record | Phase 2.5 |
| [`scenes-and-performance.md`](scenes-and-performance.md) | 8 scene snapshots w/ morph, param slew engine, 4 macros via mod matrix | Phase 5 (slew engine early, with 2.5) |
| [`tuning-and-scales.md`](tuning-and-scales.md) | Master tune, 12-degree tuning tables, scale masks for input surfaces | Phase 5 (any time after instrument model) |
| [`output-routing-and-mixer.md`](output-routing-and-mixer.md) | 16-track mixer (gain/pan/mute/solo/meters), Stage B bus routing, send-FX reserve | Phase 2.5 (mixer v1); Phase 3/5 (routing/sends) |

## Message-ID reservations (avoid collisions; final truth is `protocol.h` when implemented)

| Range | Block | IDs assigned by these designs |
|---|---|---|
| 0x50–0x5F | Sequencer / clock / arp | 0x50 SEQ_TRANSPORT · 0x51 SEQ_PATTERN_OP · 0x52 SEQ_PATTERN_SYNC · 0x53 SEQ_PLAYHEAD · 0x54 (reserved-unused; KIT_OP subsumed by INST_OP) · 0x55 MIDI_CLOCK_EVENT · 0x56 MIDI_CC · 0x57 SEQ_CLOCK_OUT · 0x58 ARP_SET |
| 0x60–0x6F | Instrument / tuning | 0x60 INST_OP · 0x61 INST_STATUS · 0x62 INST_ZONE_SYNC · 0x68 TUNING_SET |
| 0x70–0x7F | Recording / mix / scenes | 0x70 REC_CTRL · 0x71 REC_STATUS · 0x78 MIX_OP · 0x79 MIX_METERS · 0x7A SCENE_APPLY |
| 0xA0–0xAF | Offline render jobs (Phase 4, `offline-sample-editing.md`) | 0xA0 RENDER_SUBMIT · 0xA1 RENDER_PROGRESS · 0xA2 RENDER_CANCEL · 0xA3 RENDER_DONE/FAILED |

`ControlParameter` extensions (0x0B–0x15) are specified in `param-locks-and-modulation.md` §1. Every new message follows `inter-mcu-protocol.md` §4 (packed, named ctors, round-trip test + catalog row in the same commit).

## Shared infrastructure introduced by the suite

- **WXCF chunk container** (`instrument-model.md` §5): one versioned TLV file format for instruments (`.wxi`), tunings (`.wxt`), scenes/mixer (project chunks), patterns/projects (`.wxp`). Atomic temp+rename writes. Implement once in `firmware/shared/wxcf/`.
- **Param slew engine** (`scenes-and-performance.md` §3): control-tick ramp table used by scene morphs, mute ramps, and any zipper-prone param step.
- **Pending note-off queue** (`melodic-sequencing.md` §1): shared by melodic tracks and the arpeggiator.
- **`VoiceTriggerParams` extensions** (`instrument-model.md` §3): `gain_mul`, `pitch_ratio_mul`, `Voice::{slot, choke_group}`, `VoiceManager::{Choke, StopSlot}` — land once, in the instrument-model stages.

## Why this order

1. **MIDI sync** first — it's inside the Phase 2 gate already, and the tempo follower is pure host-testable code that doesn't touch the instrument layer.
2. **Instrument model** immediately after the Phase 2 gate — it deletes the item-8 stopgap note mapping and everything else (melodic tracks, arp, recording assign, p-lock resolution) plugs into it.
3. **Melodic sequencing + p-locks/modulation + mixer v1** make it a full groovebox; **recording** closes the sampler loop (and must precede the Phase 4 gate, whose first verb is "record").
4. **Arp, scenes, tuning** are leaf features — schedulable whenever their dependencies exist, sized for gap-filling.
