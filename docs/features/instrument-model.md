# Instrument Model — Presets, Zones, Multisampling, Velocity Layers

**Status**: Core data model built and host-tested — `audio/instrument.hpp` (zones, velocity layers, crossfade, choke, tuning fold, sample-record inheritance, live-params flag), and since 2026-09-02 the **only** note path: `OnNoteOn` resolves every slot through `SfzLoader::ResolveNote()`, a bare sample bound with `MSG_SAMPLE_SELECT` being a one-zone `Instrument` built by `SfzLoader::BindSample()` (origin `Built`, ids from the WAV registry) rather than a second code path. **Corrected 2026-09-02** — §6's protocol table describes ops that were never built; what actually shipped is narrower: `MSG_INST_OP`/`MSG_INST_STATUS` (0x60/0x61) exist and are live, but only for `INST_OP_SFZ_PROBE`/`INST_OP_SFZ_LOAD` (load a complete, externally-authored `.sfz` file into a slot) and `INST_OP_SET_MOD_SLOT` (param-locks-and-modulation.md §9). None of §6's `INST_OP_BIND/SAVE/NEW/SET_ZONE/CLEAR_ZONE/SET_ZONE_SAMPLE/SET_META/SET_CHOKE` exist, there is no `.wxi` reader/writer despite the WXCF *container format* itself being built and shared, and `MSG_INST_ZONE_SYNC` (0x62) is still reserved-unused. In short: **an instrument can only be built by hand-authoring an `.sfz` file off-device today** — there is no on-device zone editor, no save, no "assign this pad to that sample" workflow. See §12 for the reconciled near-term plan (Voice/Preset bank management, on-device pad→sample mapping) that a 2026-09-02 user request asked for directly.
**Lineage**: E-mu Emulator III / Emax "preset" architecture — a keyboard-wide performance object mapping samples across key and velocity ranges, feeding per-voice filter/VCA. WaveX's Stage B signal path (sample → SSI2144 VCF → SSI2164 VCA per voice) *is* the Emax voice architecture; this doc supplies the missing front half.
**Dependencies**: RAM-resident voice manager and unified note path (both built). Supersedes the former stopgap mapping policy in `audio_engine.cpp::OnNoteOn` ("most-recently-loaded sample, root note 60").
**Consumers**: `melodic-sequencing.md`, `param-locks-and-modulation.md`, `sampling-and-recording.md`, `arpeggiator.md`, `output-routing-and-mixer.md`, and the sequencer kit model (`sequencer.md` §3 — see §8 below).

**Scope:** This document defines the *Sample oscillator* half of a WaveX
Instrument — the Zone model, note resolution, sample table and SFZ-shaped
persistence — as built. **Instrument is the user-facing name** (chosen
2026-09-03 over "Patch"); its end state is a two-oscillator synth voice whose
oscillators are typed (Sample now, Wavetable reserved), with a submix, one
filter, three envelopes, two per-voice LFOs and the mod matrix, defined in
`track-and-patch-model.md` §3. The `Zone` array below becomes the body of one
`Oscillator` slot; it is not a universal base type for every oscillator, and
the `.wxi` chunk list in §5 is superseded by the per-oscillator chunk set in
that document's §3.3.

---

## 1. Concept and vocabulary

| Term | Meaning |
|---|---|
| **Zone** | One sample mapped to a key range × velocity range, with root note, tune, gain/pan, region/loop, and filter/envelope overrides. (E-mu called this a "voice"; we avoid that word — `VoiceManager` voices are playback channels.) |
| **Instrument** | The playable, named, saveable sound a Track holds. As built: an ordered set of ≤ 32 Zones plus Instrument-scoped settings (mode, choke map, mod matrix slots, macro maps, output routing). Target: two typed oscillators (each Sample oscillator owning its own Zone array) → submix → filter → amp, `track-and-patch-model.md` §3. E-mu "preset". |
| **Oscillator** | One of an Instrument's two typed slots. This document covers the Sample oscillator; `oscillator-sources.md` defines its boundary from the reserved Wavetable type. |
| **Slot** | One of 16 runtime bindings on the Daisy: sequencer track *t* and MIDI channel *t* play the instrument bound to slot *t*. |
| **Drum mode** | Instrument flag: zones are one-per-key pads, no pitch tracking (`increment` ignores note), choke groups active. A **kit** (`sequencer.md`) is exactly a drum-mode instrument. |
| **Keyboard mode** | Zones span key ranges, notes pitch-track relative to `root_note` (existing 12-TET path in `VoiceManager::Trigger`). |

Design rule carried over from the sequencer doc: **the engine owns the playable representation.** Instruments are resident in Daisy RAM; the ESP32 edits them via small idempotent protocol ops and never resolves note→sample itself.

---

## 2. Data model (Daisy-resident, host-testable)

New module: `firmware/daisy/src/audio/instrument.hpp` (+ `instrument_bank.hpp`). HAL-free like `voice_manager.hpp` — operates on ids and plain structs, so zone lookup, layering, velocity crossfade, and choke logic are all host-testable. Its `Zone` array is intentionally sampler-specific; do not add wavetable frame/index fields to `Zone` to simulate a common source type.

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

## 3. Note-on resolution (replaces the former stopgap)

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
- Streamed zones (long samples) are **out of scope for v1** — they wait on the concurrent-streamed-voices work in roadmap Phase 1. A zone whose sample exceeds `WAVEX_INST_MAX_RAM_SAMPLE_BYTES` (default 4 MB) is rejected with a distinct error code so the UI can say *why*.

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

Instrument file (`0:/wavex/instruments/<name>.wxi`, file_type=1). The chunk layout is now specified in `track-and-patch-model.md` §3.3 — `HEAD`, one typed `OSC1`/`OSC2` chunk per oscillator (a Sample oscillator's body is its Zone array with each Zone's sample **path**), `FILT`, `AMP`, `ENV1..3`, `LFO1..2`, `MODM`, reserved `FXCH` — replacing the five-chunk sketch this section carried before. Zone wire form mirrors §2 but with fixed-width fields and no floats-with-NaN risk (validate on read). A Bank (`.wxb`) nests 128 of these chunk streams under per-slot `INST` chunks so one reader serves both.

---

## 6. Protocol (reserved block 0x60–0x67; conventions per `inter-mcu-protocol.md` §4)

> **This section is the original target design and does not describe what
> shipped.** Only `INST_OP_SFZ_PROBE`, `INST_OP_SFZ_LOAD` and
> `INST_OP_SET_MOD_SLOT` exist in `protocol.h` today, and the real
> `InstOpMessage` uses named fields per op (matching every other multi-shape
> message in this protocol, e.g. `SeqPatternOpMessage`), not the
> union-by-op byte blob sketched below. Treat the op table and wire shape
> here as unimplemented proposals to revise at build time, not as a
> spec to implement literally — see §12 for what to actually build next.

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

---

## 12. Quick design: Instrument bank management and pad→sample mapping (2026-09-02)

> **Superseded in scope, 2026-09-04.** The workflow, the Bank (128 Instruments
> in a `.wxb`), the Sample Pool and the stage order now live in
> `track-and-patch-model.md` §3.6, §4, §6.1 and §8. The pad→sample op in §12.1
> (`INST_OP_SET_PAD_SAMPLE`) is unchanged and is that document's stage-4 Pad
> Map piece; §12.2's "single resident import" constraint is what stage 3
> removes. Kept for the engine-level detail; do not plan from the stage list in
> §12.3.

Requested directly (bench session, not yet a roadmap phase): "manage a bank of voices/presets" and "map different samples to different pads/keys". Both are already this document's job — **`Instrument` *is* the Voice/Preset entity** (E-mu called it "preset"; this doc's own §1 table already names the mapping), and **a multi-sample pad/key map is already the `Zone` model** (§1: one zone = one sample × one key range; §8: "a kit is a drum-mode instrument", pad *p* ↔ a zone with `key_lo = key_hi = pad_note(p)`). Neither needs a new entity or a new data model. What's missing is entirely the on-device *workflow* to build and manage one without hand-authoring an `.sfz` file off-device — see the corrected Status line and §6's note above.

An `Instrument` is the preset: use per-key (possibly single-key) zones rather
than introducing a distinct lighter Voice/Preset entity.

**Deliberately out of scope here, per explicit instruction**: making more than one instrument slot resident at once ([backlog](../backlog.md#only-one-instrument-slot-can-be-resident-at-a-time)). "Managing a bank" below means browsing/saving/loading named files on SD — loading one still swaps whatever is currently resident, exactly like `INST_OP_SFZ_LOAD` already does. That is a real, useful capability on its own (an E-mu/Emax workflow is "load a preset, play it" more often than "layer many at once"), and nothing below is wasted if slot residency is later made concurrent.

### 12.1 Pad→sample mapping (multi-sample keys/pads), v0

The full zone editor (§7 item 2 — drag key/vel ranges on a mini-keyboard, per-zone tune/gain/pan/filter/ADSR) is real work and stays the eventual target. A much smaller slice covers "assign a resident sample to each pad" without it:

- **Engine**: one new function alongside `SfzLoader::SetModSlot()` (same file, same pattern) — `SfzLoader::SetPadSample(uint8_t slot, uint8_t pad_index, uint16_t sample_id)`. Builds/overwrites a single one-key `Zone` (`key_lo = key_hi = pad_index`, `vel_lo = 1, vel_hi = 127`, `root_note = pad_index`, `in_use = true`) directly in `s_bank.Slot(slot).zones[pad_index]`, and sets `mode = InstrumentMode::Drum`. `sample_id` is one of the **plain-WAV loader's ids** (`s_loaded_samples[]`, `MSG_SAMPLE_LOAD`/`MSG_SAMPLE_SELECT`), not an SFZ-scoped id — this is the part that needs deciding (see below), and is why this is v0-sized rather than a full `SampleResolver` rework.
- **The `SampleResolver` question** — **decided and built 2026-09-02** (with roadmap Phase 2.5 item 1's branch unification, which needed the same answer). `Instrument::origin` (`None` / `SfzImport` / `Built`) says which registry a zone's ids index; `SfzLoader::ResolveNote()` switches on it between `s_sample_table.Resolver()` and a bridging resolver the engine registers once via `SfzLoader::SetLoadedSampleResolver()` (`audio_engine.cpp::ResolveLoadedSample`, function pointer + context per `ModSlotResolver`'s precedent). That resolver also fills `SampleRef`'s sample-record fields (region, loop, fades, gain from the sidecar `SampleMetadata`), which a zone inherits wherever it leaves its own at 0 — §2's rule. `SetPadSample()` therefore has nothing to decide: it writes a one-key zone into a `Built` instrument exactly as `BindSample()` writes a whole-keyboard one, with `ZONE_FLAG_LIVE_FILTER_ENV` set until a zone editor gives the zone its own filter/ADSR. `SfzLoader::ForgetLoadedSample()` already drops any `Built` zone whose sample is unloaded.
- **Protocol**: one narrow op, `INST_OP_SET_PAD_SAMPLE {slot, pad_index, sample_id}` (three small fields — far narrower than §6's full `INST_OP_SET_ZONE`), riding the same `MSG_INST_OP`/`InstOpMessage` extension pattern `INST_OP_SET_MOD_SLOT` just established (named fields, `path` unused for this op, same commit gets a round-trip + dispatch test).
- **UI**: reuse the Instrument page's sample-cycling technique (`UIVoicePage::cycleSample()`, `ui_voice_page.cpp`, added 2026-09-02) on a per-pad basis: a "Pad Map" tab or page listing 16 pads, each showing its currently-assigned sample (or "none"), with Value −/+ cycling the FOCUSED pad's assignment through resident samples via the same `inter_mcu_get_sample_meta()` probe. No new sample-list infrastructure needed - it is the third page to reuse that probe (Sample Manager, Instrument, now this).

This delivers "different pads play different samples" as a real on-device workflow without the WXCF save path or the full zone editor - it is one engine function, one protocol op, and one UI page/tab, each independently host-testable and small enough to be its own commit.

### 12.2 Bank management (browse, save, load, rename, delete)

Needs §5's WXCF *container format* (already built, shared, host-tested) plus an instrument-specific reader/writer and the save/load/list protocol ops §6 never got beyond proposing:

- **`.wxi` reader/writer** (`firmware/shared/wxcf/` consumer, alongside the container itself): serializes an `Instrument` (name, mode, zones, mod slots) to/from the WXCF chunk layout §5 already specifies. Host-testable round-trip, no hardware needed.
- **Protocol**: `INST_OP_SAVE {slot, path}` (persist the slot's current in-RAM instrument), `INST_OP_NEW {slot, mode}` (blank instrument, for building a pad map from scratch rather than starting from a loaded one). A **list** op is also needed and is not in §6 at all - the existing `MSG_BROWSE_REQ/RESP` (0x30/0x31) already lists directory contents generically and can list `0:/wavex/instruments/*.wxi` the same way the Sample Browser lists `.sfz`/`.wav`, so this may need no new message at all, just a browser page pointed at that directory.
- **UI**: an "Instrument Browser" page, structurally a near-clone of the existing Sample Browser (list a directory, preflight, load) pointed at `.wxi` files instead of samples/`.sfz`, plus a "Save As" action from the Instrument page (currently a disabled placeholder button with the honest label "needs the Instrument file (.wxi)" — this is exactly what removes that gap) that prompts a filename and sends `INST_OP_SAVE`.
- **Naming**: `Instrument::name[24]` already exists in §2's data model (not yet in the currently-built `instrument.hpp` - `Instrument` there has no `name` field yet, only `mode` + `zones[]`; adding one is a small, additive change). The Instrument page already carries a RAM-only `voice_name_` today (per its class doc: "the name is held in RAM so the entity is real even while its storage is not") - saving should read that field into the persisted `.wxi`, retiring the RAM-only caveat.

### 12.3 Suggested stage order

Independent of each other except where noted; either column can go first.

1. `SfzLoader::SetPadSample()` + `INST_OP_SET_PAD_SAMPLE` + the bridging `SampleResolver` decision (§12.1) — host-tested, round-trip + dispatch tests, no UI yet.
2. Pad Map UI (reuses the SAMPLE-cycling pattern already shipped on the Instrument page).
3. `.wxi` reader/writer, host-tested round-trip (no protocol yet - loadable only via the same boot-time `LoadSfzInstrument`-style path used today, to prove the format before wiring it to a message).
4. `INST_OP_SAVE`/`INST_OP_NEW` + round-trip/dispatch tests + `inter-mcu-protocol.md` update.
5. Instrument Browser UI page + Instrument page's "Save As"/"Load" wiring (retiring those two disabled placeholders).
