# Arpeggiator — Design

**Status**: Target design (unimplemented). Phase 2.5 in `roadmap.md`.
**Dependencies**: sequencer clock (Phase 2 — the arp is clock-synced; a free-run Hz fallback lets it ship against the bare control tick if it lands first), `instrument-model.md` (arp output feeds slot instruments).
**Lineage**: the Emax shipped a latch-able arpeggiator over its preset engine; same placement here — between note input and the instrument's zone resolution, invisible to everything downstream.

---

## 1. Placement

Per Track, on the **Daisy**, in the control tick. The arp intercepts the Track's note stream *after* wire dispatch, *before* `Tracks::ResolveNote`:

```
MSG_NOTE_ON/OFF → [slot arp enabled?] ──no──► instrument resolution (unchanged)
                        │yes
                        ▼
                  held-note buffer ──(clock-driven pattern generator)──► instrument resolution
```

Downstream (zones, p-locks, mod matrix, choke, analog path) needs zero changes — an arp note is indistinguishable from a played note. Live-record (`melodic-sequencing.md` §3) captures arp output if record is armed, which is the classic "arp into the sequencer" workflow for free.

## 2. Model

```cpp
// firmware/daisy/src/sequencer/arpeggiator.hpp — HAL-free, host-testable
struct ArpConfig {
    uint8_t enabled   = 0;
    uint8_t mode      = 0;   // 0=up 1=down 2=updown(inclusive) 3=updown(exclusive)
                             // 4=as-played 5=random (seeded per transport start)
    uint8_t octaves   = 1;   // 1..4, appended sweeps up
    uint8_t division  = 4;   // internal 96-PPQN ticks per arp step, from a fixed
                             // division table: 1/32..1/2 incl. triplets/dotted
    uint8_t gate_pct  = 75;  // note length as % of division (100 = legato-adjacent)
    uint8_t latch     = 0;   // 1: notes persist after release; new chord replaces on
                             //    first note-on after all-keys-up (Emax latch behavior)
    uint8_t vel_mode  = 0;   // 0=as played, 1=fixed(vel_fixed), 2=step ramp down
    uint8_t vel_fixed = 100;
};
```

- **Held buffer**: ≤ 16 `(note, velocity, order)` entries per slot; insertion-ordered for as-played, sorted view for up/down. All fixed-size, callback-context-safe.
- **Clocking**: fires when the sequencer master phase `Φ` crosses multiples of `division` (so it locks to transport, swing excluded by design — the arp stays straight against a swung pattern, which is the musically expected behavior; revisit only if users ask). When transport is stopped, a free-run phase at the current BPM keeps the arp playable.
- **Note-offs**: gate expiry scheduled through the same pending-off queue as melodic tracks (`melodic-sequencing.md` §1) — one mechanism, shared tests.
- Rebuild-on-change: chord edits (add/remove note, octave change) take effect at the **next arp step**, never mid-step (no torn sorting).

## 3. Protocol & UI

- `MSG_ARP_SET` (0x58, E→D): `{uint8_t slot; ArpConfig}` — whole-config idempotent set (it's 8 bytes; no per-field ops needed). Round-trip + dispatch tests as always. Arp config persists inside the instrument's `.wxi` (new WXCF chunk id, `instrument-model.md` §5).
- UI: one compact panel on the instrument page — enable, mode, division, octaves, gate, latch. LED/playhead feedback not needed for v1.

## 4. Tests

- Host golden streams: each mode × octaves over a held C-E-G — exact `(tick, note, vel)` sequences, incl. up/down inclusive-vs-exclusive endpoints and as-played order; latch semantics (release-all then replay); random mode reproducible under seed; gate-off scheduling; division changes mid-run land on the next step.
- Hardware: arp against MIDI-clock slave transport (compound test with `midi-sync-tempo-follower.md` — arp steps must not drift against the DAW's grid over 10 minutes).

## 5. Implementation stages (one verified commit each)

1. `arpeggiator.hpp` + full host suite.
2. Slot integration (intercept point in the note path) + dispatch/regression tests; free-run clock.
3. Protocol 0x58 + round-trip tests + `.wxi` chunk + doc rows, same commit.
4. UI panel; bench check incl. live-record capture of arp output.
