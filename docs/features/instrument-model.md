# Instrument Model — Presets, Zones, Multisampling, Velocity Layers

**Status**: Core built and host-tested — `audio/instrument.hpp` (zones, velocity layers, crossfade, choke, tuning fold; 18 host tests) and the WXCF container. Remaining: the sample table that populates a `SampleResolver` from loaded WAVs, the `MSG_INST_OP/STATUS/ZONE_SYNC` protocol (0x60–0x62), deleting the Phase-1 stopgap note→sample policy, the ESP32 UI, and callback wiring. Phase 2.5 in `roadmap.md`.
**Lineage**: E-mu Emulator III / Emax "preset" architecture — a keyboard-wide performance object mapping samples across key and velocity ranges, feeding per-voice filter/VCA. WaveX's Stage B signal path (sample → SSI2144 VCF → SSI2164 VCA per voice) *is* the Emax voice architecture; this doc supplies the missing front half.
**Dependencies**: Phase 1 voice manager (done), Phase 1 item 8 note path (done). Supersedes the item-8 stopgap mapping policy in `audio_engine.cpp::OnNoteOn` ("most-recently-loaded sample, root note 60").
**Consumers**: `melodic-sequencing.md`, `param-locks-and-modulation.md`, `sampling-and-recording.md`, `arpeggiator.md`, `output-routing-and-mixer.md`, and the sequencer kit model (`sequencer.md` §3 — see §8 below).

---

## 1. Concept and vocabulary

| Term | Meaning |
|---|---|
| **Zone** | One sample mapped to a key range × velocity range, with root note, tune, gain/pan, region/loop, and filter/envelope overrides. (E-mu called this a "voice"; we avoid that word — `VoiceManager` voices are playback channels.) |
| **Instrument** | An ordered set of ≤ 32 zones plus instrument-scoped settings (mode, choke map, mod matrix slots, macro maps, output routing). E-mu "preset". |
| **Slot** | One of 16 runtime bindings on the Daisy: sequencer track *t* and MIDI channel *t* play the instrument bound to slot *t*. |
| **Drum mode** | Instrument flag: zones are one-per-key pads, no pitch tracking (`increment` ignores note), choke groups active. A **kit** (`sequencer.md`) is exactly a drum-mode instrument. |
| **Keyboard mode** | Zones span key ranges, notes pitch-track relative to `root_note` (existing 12-TET path in `VoiceManager::Trigger`). |

Design rule carried over from the sequencer doc: **the engine owns the playable representation.** Instruments are resident in Daisy RAM; the ESP32 edits them via small idempotent protocol ops and never resolves note→sample itself.

---

## 2. Data model (Daisy-resident, host-testable)

New module: `firmware/daisy/src/audio/instrument.hpp` (+ `instrument_bank.hpp`). HAL-free like `voice_manager.hpp` — operates on ids and plain structs, so zone lookup, layering, velocity crossfade, and choke logic are all host-testable.

```cpp
// instrument.hpp — engine-side (NOT wire) structs
static constexpr uint8_t  kMaxZones           = 32;
static constexpr uint8_t  kMaxLayerTriggers   = 4;   // zones fired per note-on, cap
static constexpr uint8_t  kNumInstrumentSlots = 16;

struct Zone {
    uint16_t sample_id      = 0;      // key into the slot's sample table (§4)
    uint8_t  key_lo = 0,  key_hi = 127;   // inclusive MIDI note range
    uint8_t  vel_lo = 1,  vel_hi = 127;   // inclusive velocity range
    uint8_t  root_note      = 60;
    int8_t   coarse_tune    = 0;      // semitones, ±48
    int8_t   fine_tune      = 0;      // cents, ±99
    float    gain           = 1.0f;   // linear, pre-velocity
    float    pan            = 0.5f;
    // Region/loop. 0 = "use sidecar marker, else whole file" (resolved at
    // sample-load time into the sample table entry; a nonzero value here is
    // a per-zone override of the sidecar).
    uint32_t start_frame = 0, end_frame = 0;
    uint32_t loop_start  = 0, loop_end  = 0;
    uint8_t  loop_mode      = 0;      // 0=off, 1=forward (ping-pong: later, needs Voice support)
    uint8_t  choke_group    = 0;      // 0=none, 1..8 (drum mode)
    uint8_t  output_bus     = 0;      // 0=stereo mix; Stage B: 1+group (output-routing doc)
    uint8_t  flags          = 0;      // bit0: vel_xfade (§3.2), bit1: one_shot (ignore note-off)
    // Per-zone playback-parameter overrides (feed VoiceTriggerParams):
    float    cutoff_hz      = 20000.0f;
    float    attack_s = 0.001f, decay_s = 0.05f, sustain = 0.8f, release_s = 0.1f;
    bool     in_use         = false;
};

struct Instrument {
    char     name[24]       = "";
    uint8_t  mode           = 0;      // 0=keyboard, 1=drum
    Zone     zones[kMaxZones];
    // Mod-matrix slots and macro maps live here too — defined in
    // param-locks-and-modulation.md §3, stored per instrument.
};
```

Memory: `sizeof(Zone)` ≈ 64 B packed-ish; 32 zones × 16 slots ≈ 33 KB plus instrument headers — allocate the bank statically in AXI SRAM or as one boot-time SDRAM extent. **No allocation at note time or edit time.**

---

## 3. Note-on resolution (replaces the item-8 stopgap)

`audio_engine.cpp::OnNoteOn(note, velocity, channel)` becomes:

1. `slot = channel & 0x0F` → `Instrument& ins = bank.SlotInstrument(slot)`.
2. Scan `ins.zones[]` for `in_use && key_lo ≤ note ≤ key_hi && vel_lo ≤ velocity ≤ vel_hi`. Collect up to `kMaxLayerTriggers` matches (32-entry linear scan, branch-light — trivially inside the control-tick budget; this runs in the SPSC-drain context in the callback, so it must stay allocation- and I/O-free, which it is).
3. For each matching zone, build a `VoiceTriggerParams` from the zone + sample-table entry:
   - `sample/sample_frames/channels/sample_rate_hz` from the sample table (§4);
   - `note`, `velocity` pass through; `root_note` from zone; drum mode forces `note = root_note` (no pitch tracking);
   - tuning: coarse/fine fold into the pitch ratio — extend `VoiceTriggerParams` with `float pitch_ratio_mul = 1.0f` (a pure multiplier composed onto the existing 12-TET × rate-compensation product; also the hook `tuning-and-scales.md` uses). `pitch_ratio_mul = 2^((coarse + fine/100)/12)`.
   - region/loop/cutoff/ADSR/pan from zone; `gain` folds into velocity gain (extend `VoiceTriggerParams` with `float gain_mul = 1.0f` rather than munging velocity).
4. Choke (drum mode): if `zone.choke_group != 0`, call new `VoiceManager::Choke(group, fast_release_s = 0.005f)` before triggering — puts every active voice tagged with that group into a 5 ms release (classic open/closed hat). Requires `Voice` to carry `uint8_t choke_group` (set at trigger) and `Envelope` to accept an override release time on `Release()` — both small, host-testable extensions.
5. `OnNoteOff(note, channel)`: release all voices for `(note, slot)` **except** zones flagged `one_shot`. Requires `Voice` to also carry its slot id: extend `Voice` with `uint8_t slot`.

### 3.1 Velocity layers and switching

Non-overlapping `vel_lo/vel_hi` ranges give hard velocity switching (Emax primary/secondary). Overlapping ranges layer (both fire), bounded by `kMaxLayerTriggers`.

### 3.2 Velocity crossfade (`flags & ZONE_FLAG_VEL_XFADE`)

E-mu's positional/velocity crossfade, cheap version: when set, the zone's gain is additionally scaled by a linear ramp across its velocity span — `xfade_gain = (velocity − vel_lo + 1) / (vel_hi − vel_lo + 1)` for an "upper" zone; a zone may instead set the inverse ramp via a second flag bit (`ZONE_FLAG_VEL_XFADE_DOWN`). Two overlapping zones with opposite ramps crossfade smoothly through the overlap. Pure trigger-time arithmetic — zero per-sample cost.

---

## 4. Sample table and loading

Per-slot sample table: `{uint16_t sample_id → wxsamp_t handle, frames, channels, rate, resolved sidecar markers}`, ≤ 32 entries (one per zone max).

- Instrument files reference samples **by path** (like `SampleLoadMessage`). On instrument load (§6), the Daisy walks the zone list, deduplicates paths, and loads each through the existing `OnSampleLoad` machinery (`SampleMemMgr::alloc` + chunked SD read from the main loop — load is *not* real-time). Progress/failures surface via `MSG_INST_STATUS`.
- **Eviction rule**: binding a new instrument to a slot releases the old slot's table entries not shared with other slots (refcount by path hash). `VoiceManager::StopAll()` semantics apply, but scoped: add `VoiceManager::StopSlot(slot)` (hard-stop only voices whose `slot` matches) so rebinding slot 3 doesn't cut off slots 0–2. This is why `Voice` grows a `slot` field in §3.
- RAM budget guard: refuse to load (with `INST_STATUS` error) rather than partially load, if total bytes exceed the free sample RAM minus a configurable reserve (`WAVEX_INST_LOAD_RESERVE_BYTES`, default 8 MB, in `hardware_config.h`).
- Streamed zones (long samples) are **out of scope for v1** — they wait on the streamed-voice-concurrency refactor (roadmap Phase 1 item 2's open half). A zone whose sample exceeds `WAVEX_INST_MAX_RAM_SAMPLE_BYTES` (default 4 MB) is rejected with a distinct error code so the UI can say *why*.

---

## 5. Persistence: the WXCF chunk container (shared, defined here)

One container format for every WaveX SD artifact (instruments here; patterns/projects in `sequencer.md`; scenes in `scenes-and-performance.md`; tunings in `tuning-and-scales.md`):

```
File   := Header, Chunk*
Header := magic "WXCF" (4 B), u16 file_type, u16 file_version, u32 total_len
Chunk  := u16 chunk_id, u16 chunk_version, u32 payload_len, payload bytes
```

- Little-endian throughout; readers **skip unknown chunk_ids** (forward compatibility) and reject unknown `file_version` majors.
- Writes are atomic: write `<name>.tmp`, `f_close`, `f_rename` (per `offline-sample-editing.md` §2).
- Implementation: `firmware/shared/wxcf/wxcf.hpp` — HAL-free reader/writer over a `read(off,len)/write` callback pair, so it round-trip-tests on host and runs over FatFs on target.

Instrument file (`0:/wavex/instruments/<name>.wxi`, file_type=1): chunk 1 = instrument header (name, mode), chunk 2 = zone array (count × wire-Zone), chunk 3 = mod-matrix slots, chunk 4 = macro maps, chunk 5 = sample path table (paths referenced by zone `sample_id`). Zone wire form mirrors §2 but with fixed-width fields and no floats-with-NaN risk (validate on read).

---

## 6. Protocol (reserved block 0x60–0x67; conventions per `inter-mcu-protocol.md` §4)

All structs get the standard named-constructor treatment and round-trip tests in `firmware/shared/tests/` in the same commit they're added.

| Type | ID | Dir | Payload | Purpose |
|---|---|---|---|---|
| MSG_INST_OP | 0x60 | E→D | `InstOpMessage` | small idempotent edit ops (below) |
| MSG_INST_STATUS | 0x61 | D→E | `InstStatusMessage{slot, op_echo, status, error_code, loaded_zones, total_zones, ram_bytes}` | ack/progress/error for ops and loads |
| MSG_INST_ZONE_SYNC | 0x62 | D→E | `InstZoneSyncMessage{slot, zone_index, wire-Zone}` | zone readback for UI (on page open / after load) |

`InstOpMessage` (fits a 128 B class): `{uint8_t slot; uint8_t op; uint8_t zone_index; uint8_t reserved; union-by-op payload[96]}` with ops:

| op | payload | semantics |
|---|---|---|
| INST_OP_BIND | path[96] | load `<path>.wxi` into slot (async; progress via INST_STATUS) |
| INST_OP_SAVE | path[96] | persist slot's instrument (atomic) |
| INST_OP_NEW | mode | blank instrument in slot |
| INST_OP_SET_ZONE | wire-Zone (≤ 88 B) | create/overwrite zone `zone_index` |
| INST_OP_CLEAR_ZONE | — | free zone |
| INST_OP_SET_ZONE_SAMPLE | path[96] | (re)point zone's sample path; triggers load |
| INST_OP_SET_META | name[24], mode | rename / mode switch |
| INST_OP_SET_CHOKE | zone_index, group | convenience for pad UI |

Edits apply **between control ticks** (double-buffer the zone being written, or briefly mark it `!in_use` during the copy — a zone edit may never tear mid-trigger). Same discipline as `sequencer.md` §4's "edits applied between steps".

---

## 7. UI (ESP32 pages, per `ui-architecture.md` patterns)

1. **Instrument page** (per slot): zone list (key range, vel range, sample name), softkeys New/Load/Save/Mode.
2. **Zone editor**: key/vel range drag on a mini-keyboard widget, root note (play-to-set: next MIDI note received sets root — Emax workflow), tune, gain/pan, region/loop (reuses waveform preview via existing `MSG_PREVIEW_REQ` path), filter/ADSR.
3. **Keyboard-split quick action**: "spread selected N samples chromatically / across N equal splits" — the Emax auto-placement niceties; pure ESP-side loop emitting `INST_OP_SET_ZONE`s.

UI never blocks on loads: `INST_STATUS` drives progress toasts (deferred-update pattern, never LVGL off the UI task).

---

## 8. Relationship to the sequencer kit model

`sequencer.md` §3 defines `Kits[≤16]: pad → sample ref + voice params`. **A kit is a drum-mode instrument** — pad *p* ↔ zone with `key_lo = key_hi = pad_note(p)`. `KIT_OP` (0x54, reserved in `sequencer.md` §4) is subsumed by `MSG_INST_OP`; keep 0x54 reserved-unused. Sequencer tracks address instrument slots, melodic or drum alike — one resolution path, one engine surface (this is the consolidation that keeps Phase 2 UI honest).

---

## 9. Real-time safety review

| Path | Context | Budget |
|---|---|---|
| Zone scan + trigger build | audio callback (SPSC drain) | 32-entry scan + arithmetic; no alloc/IO. Measure with DWT once on hardware; expected ≪ 50 µs |
| Choke | audio callback | ≤ 8 voice-state writes |
| Zone edits | main loop (message dispatch) | copy ≤ 88 B, guarded against tearing |
| Instrument load | main loop, chunked | same machinery as `OnSampleLoad`; playback continues |
| Save | main loop, chunked WXCF write | ≤ 2 ms per step (render-scheduler budget rule) |

---

## 10. Implementation stages (one verified commit each)

1. **`instrument.hpp` + zone resolution, host-tested**: `Instrument`, `InstrumentBank`, `ResolveNoteOn()` returning trigger-param sets; velocity switch/layer/crossfade tests; no wiring.
2. **Voice manager extensions**: `Voice::slot`, `Voice::choke_group`, `VoiceManager::Choke()`, `StopSlot()`, `VoiceTriggerParams::{gain_mul, pitch_ratio_mul}`; host tests.
3. **Engine wiring**: `OnNoteOn/OnNoteOff` route through the bank (slot = channel); delete the stopgap policy; dispatch-level tests extended (pattern: `message_dispatch_test.cpp`).
4. **Sample table + async load path**, INST_STATUS reporting; host tests with a mock loader.
5. **WXCF container** (`firmware/shared/wxcf/`) + `.wxi` save/load, round-trip host tests.
6. **Protocol messages** (0x60–0x62) + round-trip tests + `inter-mcu-protocol.md` update, same commit.
7. **ESP32 UI pages** (instrument, zone editor), bench-verified.

## 11. Open questions

- Ping-pong loop mode needs a `Voice` render-loop change (direction flag) — defer until asked for.
- Per-zone one_shot vs kit-level "gate mode" toggle — v1 ships zone flag only.
- Crossfade *looping* (rendering a crossfaded loop seam — the famous Emax tool) is an **offline render op**: added to `offline-sample-editing.md` §4 Tier 2 by this design (`xfade_loop(loop_start, loop_end, xfade_ms)` → writes new file + sidecar loop markers).
