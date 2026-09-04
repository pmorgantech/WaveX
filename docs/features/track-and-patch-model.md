# Track and Patch Model — the user-facing paradigm and its end state

**Status**: **Proposed 2026-09-02, extended 2026-09-03; decisions pending** (see §9). Written in response to direct requests for an end-state design so that the Phase 2/2.5 UI and voice work converges on one paradigm rather than accreting. Track/Patch/Pattern/Song vocabulary is confirmed; the broader ownership hierarchy is accepted architecture. Nothing here is built as one unit; several pieces it names already are, and are marked.
**Supersedes, in vocabulary only**: "slot" (`instrument-model.md` §1) and "Voice" as a page/entity name. The engine-side data model in `instrument-model.md` stands; this document says what it is *called* and how tracks, MIDI and memory are arranged around it.
**Dependencies**: `instrument-model.md` (the sampler `Instrument`/`Zone` model, built), `oscillator-sources.md` (typed sampler/wavetable boundary), `output-routing-and-mixer.md` (`TrackMix`, built), `sequencer.md` (tracks, built core), `wxcf.hpp` (built).

---

## 1. Vocabulary

The words the UI, the docs and new code use. Engine identifiers keep their current names until the rename stage (§8 stage 1); the mapping is exact.

| Term | Meaning | Was called | Engine identifier today |
|---|---|---|---|
| **Asset** | A stored audio file or generated PCM resource in the SD library. Assets are referenced by Patches and become resident through a registry; they do not own playback behavior. | sample file, WAV | SD path + sample metadata; wavetable assets are deferred |
| **Oscillator source** | A typed Patch-owned recipe that interprets an Asset as a sampler recording, wavetable, or future source type. Source type determines playback semantics. | sample oscillator | sampler `Zone` mapping today; no common engine type yet |
| **Track** | **Confirmed 2026-09-02:** one sequencer track (formerly "slot") *and* one space in which an active Patch can be loaded, responding to MIDI messages on its designated channel. Sixteen of them. Also owns a mixer strip and a polyphony limit. What a note is *addressed to*. | slot, instrument slot, channel | `kNumInstrumentSlots`, `InstrumentBank::Slot()`, `Voice::slot`, `NoteMessage::channel`; **and** `pattern.hpp`'s inner `Track` struct (a pattern's per-track step row — rename to `TrackSteps` in stage 1 so the word means one thing) |
| **Patch** | **Confirmed 2026-09-02:** one playable, named, tagged, saveable sound: typed oscillator-source definitions plus mappings, filters, envelopes, modulation, tuning, and effects. Gets loaded *into* a Track. The current implementation is sampler-only. | Voice, Preset, Instrument | `Instrument` (the current sampler Patch; keep in code — the E-mu lineage is documented there) |
| **Performance** | The current live configuration of all Tracks: Patch bindings, MIDI routing, mixer, mutes, and shared effects. V1 stores one Performance directly in the Project; it is an ownership concept, not a separate file yet. | multi, part, performance | Project's target `Tracks[16]` plus mixer state |
| **Pattern** | A group of notes/velocities over a fixed span — default **2 bars of 16ths = 32 steps** — with one step row per Track. The sequencer's unit of composition. | pattern | `Pattern` (`pattern.hpp`, built: 1–64 steps, default 16 → 32) |
| **Song** | An ordered arrangement of Patterns over time, at a tempo and swing setting. | song, chain | `sequencer.md` §3's `Songs[≤16]: (pattern, repeats)` — not yet in code |
| **Scene** | A performance snapshot of mixer, macros, mutes, and optional Pattern selection. It references content and never embeds samples, Patches, or Pattern data. | performance | target only; `scenes-and-performance.md` |
| **Project** | The portable root that stores one Performance, Patterns, Songs, Scenes, settings, and references to saved Patches/assets. | project | `.wxp` target; not yet in code |
| **Bank** | A numbered or browsable storage grouping for Assets or Patches. It is useful for discovery/program change but is not a musical ownership layer. | bank | no target runtime entity; current `InstrumentBank` becomes `TrackBank` |
| **Zone** | One sample mapped to a key × velocity range inside a sampler Patch. Unchanged. | zone | `Zone` |
| **Sample** | A resident PCM buffer with its own record (markers, gain, name). Referenced by zones; owned by nobody but the registry. | sample | `LoadedSampleInfo` / `s_loaded_samples` |
| **Voice** | One of the 8 polyphony channels rendering one resolved oscillator-source trigger (currently one Zone's sample). An *engine* term; it never names a page or a user entity again. | voice | `Voice`, `kNumVoices` |
| **MIDI channel** | 1–16 on the DIN/USB input. A *routing input* to tracks, never the same word as Track. | channel | `MidiEvent::channel` |
| **Mod slot** | One row of a Patch's modulation matrix. Keeps "slot" — it is the conventional word there and it is never confused with a Track once Track exists. | mod slot | `ModSlot`, `Instrument::mod_slots[8]` |

Why **Track** and not Channel: with MIDI routing configurable (§2.2), track≠channel is the whole point, and the sequencer and mixer already say "track". Why **Patch** and not Preset: "Preset" in this codebase will be wanted for saved *device state* (scenes, `scenes-and-performance.md`), and MIDI's own word for "select the sound on a channel" is Program Change → a Patch. "Patch" also carries no implication that the thing came from the factory.

### 1.1 Ownership hierarchy

The hierarchy is a set of ownership and reference rules, not a requirement that
every object be serialized inside the object above it:

```text
SD library / asset pool
└── Assets <--------------------------┐
                                      |
Project                               |
├── Performance                       |
│   └── Tracks[16]                    |
│       ├── active Patch -------------┘ (Patch source definitions reference Assets)
│       └── MIDI routing, polyphony, mixer strip
├── Patterns -> TrackSteps[16] -> Tracks
├── Scenes   -> performance state + optional Pattern reference
└── Songs    -> ordered Pattern references, repeats, tempo, overrides
```

The resulting save policy is explicit:

- Assets belong to the library/pool and are referenced by stable path.
- A Patch owns sound design and source mappings; a drum Kit is a drum-mode
  Patch, not another layer.
- A Track owns the active Patch binding, MIDI routing, polyphony policy, and
  mixer strip.
- The Performance is the live set of all Track/Patch bindings, routing, mixer
  state, mutes, and shared effects. V1 stores one directly in the Project.
- A Pattern owns musical time: notes, triggers, probability, micro-timing, and
  parameter locks. It does not own or silently replace Track Patches.
- A Scene owns recallable performance state, not content.
- A Song owns form by arranging Pattern references and overrides.
- A Project owns the portable working set and its references. Export/snapshot
  may copy referenced assets; normal save does not duplicate them.
- A Bank is a browser/storage view only and never becomes a second owner of a
  Patch, Track, or Voice.

This gives the Performance an explicit stable boundary: Pattern changes
preserve the Track/Patch setup unless an explicit Scene or Project action
changes it. Multiple independently named Performances or Parts can be added
later without changing what a Pattern owns.

### 1.2 Source type is below Patch, Voice is below note resolution

Sampler and wavetable data may both be PCM, but they are different oscillator
contracts. A sampler Patch maps arbitrary recordings through Zones; a future
wavetable Patch scans fixed-length single-cycle frames. They share the Patch,
Track, Pattern, and Song hierarchy, not source-specific assumptions. See
`oscillator-sources.md`.

A Patch is the saved blueprint. A Voice is the temporary runtime allocation
created after a Track's Patch resolves a note or trigger. UI and persistence
must never use “Voice” as a synonym for Patch.

---

## 2. Track

### 2.1 Fields (engine-resident, host-testable)

```cpp
struct Track {
    Instrument  patch;            // the resident Patch (instrument.hpp), by value as today
    uint8_t     midi_in;          // 0 = Omni, 1..16 = that channel, 0xFF = Off
    uint8_t     poly_limit;       // 1..kNumVoices; 0 = no limit (default)
    uint8_t     priority;         // steal priority, 0 = lowest (default), see §5
    // TrackMix strip lives in TrackMixer (built); sequencer lane in Pattern (built).
};
```

`InstrumentBank` becomes `TrackBank` holding 16 of these; `Slot()` becomes `Track()`. Nothing about `Instrument` changes.

### 2.2 MIDI routing: poly, omni, and everything between

A note from the **MIDI input** carries a channel. A note from an **internal source** (Play page grid, sequencer, arpeggiator) is addressed to a track directly. These are different things and the wire must say which:

- `NoteMessage::channel` gains an addressing bit: `NOTE_ADDR_TRACK = 0x80`. Set → `channel & 0x0F` is a track index (internal sources). Clear → it is a MIDI channel (the ESP32 MIDI task forwards raw events unchanged, as it does today).
- The Daisy routes: a MIDI-addressed note-on goes to **every** track whose `midi_in` matches (`Omni`, or equal channel), each resolving through its own Patch. That is layering for free, and it is exactly standard MIDI behaviour:
  - **Multi/poly mode** (default): track *t* has `midi_in = t+1`. One channel per track, as today.
  - **Omni**: any set of tracks with `midi_in = Omni` all play from any channel. "All tracks omni" is the classic single-timbral mode; "two tracks omni" is a split-less layer.
  - **Off**: sequencer-only tracks that ignore the keyboard.
- Note-off routes identically. Voices already carry their track, so a note-off reaching three tracks releases the right three voices.
- The existing global ESP32-side channel filter (`midi_set_input_channel`) becomes redundant and is removed rather than kept as a second, conflicting filter.

Routing lives on the **Daisy**, not the ESP32, because tracks live there, it is a 16-entry compare in the note handler (main loop, not the callback), and it keeps one note = one link message regardless of how many tracks it lands on. The mixer's "ESP32 expands solo before sending" precedent does not apply: solo is UI state, routing is engine state.

Protocol: `MSG_TRACK_OP` (new; 0x63 from the instrument block, or a fresh id — decide at implementation) with `{track, op, value}`: `SET_MIDI_IN`, `SET_POLY_LIMIT`, `SET_PRIORITY`. Idempotent, tiny, one round-trip test.

### 2.3 What a track does *not* own

Filter/envelope/tuning are Patch properties (§3). Today's `VoiceLiveParams` is engine-global "what the knobs say" and applies to every track; the end state is that the Patch page edits **the selected track's Patch**, and `OnControlChange` writes into that Patch's zones (or its patch-level defaults, §3.2) rather than into a global. `ZONE_FLAG_LIVE_FILTER_ENV` — added 2026-09-02 as the bridge for on-device-built instruments — is retired at that point: the flag existed only because nothing could yet write a zone. (`VoiceManager::ApplyLiveParams`, which pushes edits onto *sounding* voices, stays; it just receives the track's values rather than a global.)

---

## 3. Patch

### 3.1 What it is

For the implemented sampler source, `Instrument` (`instrument.hpp`) plus the
fields a *named, saveable* thing needs. All additive. This is the sampler Patch
representation, not a requirement that future wavetable metadata be forced into
`Zone`:

```cpp
struct Instrument {                      // == Patch
    char            name[24];            // instrument-model.md §2 already specified it; not yet in code
    uint8_t         tags;                // bitmask over a fixed vocabulary (§3.4); 8 bits, grow to 16 if needed
    InstrumentMode  mode;                // Keyboard / Drum (built)
    InstrumentOrigin origin;             // built 2026-09-02; see §4 for its future
    int8_t          transpose;           // semitones, patch-wide (zone tune composes on top)
    int8_t          fine_tune;           // cents, patch-wide
    float           trim_gain;           // linear; the patch's own level, NOT the track fader
    float           trim_pan;            // 0..1; the patch's own centre, NOT the track pan
    Zone            zones[kMaxZones];    // built
    ModSlot         mod_slots[8];        // built
    // Reserved, not fields yet: FX selection + params (a WXCF chunk id is reserved so files
    // written before FX exists load after it), patch-level filter/env defaults (§3.2).
};
```

**Trim vs. mixer.** A Patch's `trim_gain/pan` is part of the *sound* ("this piano is quiet"); the Track's `TrackMix` fader/pan/mute/solo is part of the *mix* ("bring the piano down in this song"). Both exist today in different places (zone gain/pan; `TrackMix`). Loading a Patch never touches the mixer strip. This is the same split every DAW and every E-mu makes, and it is what lets a Patch be reused across projects.

### 3.2 Patch-level defaults vs. zone overrides

Today every zone carries its own cutoff/ADSR (from SFZ, where that is normal). A user building a Patch on-device should not have to set ADSR on 16 pads. So: Patch-level `filter`/`env` defaults, and a per-zone "override" flag (`ZONE_FLAG_OWN_FILTER_ENV`, the inverse of today's live flag). The Patch page's Filter and Env tabs edit the Patch defaults; a zone editor can override per zone. SFZ import sets the override flag on every zone (it always has per-zone values), so imported Patches sound exactly as they do now.

### 3.3 Persistence: `.wxi` over WXCF

`instrument-model.md` §5 specified it; nothing implements it yet. Chunks:

| chunk | payload |
|---|---|
| `HEAD` | name, tags, mode, transpose/fine, trim gain/pan |
| `ZONE` ×N | one `Zone` each **plus the sample's path** (never a runtime id) and the sample's sidecar-independent overrides |
| `MODM` | the 8 mod slots |
| `FXCH` | *reserved*, empty — readers skip it today |
| `DEFS` | patch-level filter/env defaults (§3.2) |

Load: parse, dedupe paths, load samples through the shared registry (§4), bind. Exactly the SFZ loader's pipeline with a different front half — `SfzLoader`'s phases (`Probe → AwaitVoiceStop → Allocate → Read → Commit`) become a generic **PatchLoader** with two parsers feeding the same `MappedInstrument`. **An `.sfz` is an import format for a Patch, not a different kind of thing**; "Save" always writes `.wxi`. The Patch Browser lists both.

Location: `0:/wavex/patches/<name>.wxi`, samples referenced by absolute card path. A "Save with samples" (copy referenced WAVs into `patches/<name>/`) is a later convenience, not v1.

### 3.4 Tags

A fixed vocabulary, bitmask: `Drum, Bass, Lead, Pad, Keys, FX, Vocal, Loop`. Free-text tags need a string table, a search UI and a keyboard; a bitmask needs eight checkboxes and a filter row in the browser. Start there; the WXCF chunk can grow.

---

## 3.5 Pattern and Song (the sequencer's nouns)

Confirmed 2026-09-02 alongside Track and Patch, so that the four words are defined in one place. The sequencer design proper is `sequencer.md`; this section records only how these nouns relate to Tracks and Patches and where the current model differs.

```
Project (.wxp)
├── Tracks[16]      : Patch (by .wxi/.sfz path), midi_in, poly_limit, TrackMix strip   ← §2
├── Patterns[≤128]  : length (default 32 = 2 bars of 16ths), scale, swing*, TrackSteps[16]
└── Songs[≤16]      : tempo, swing*, ordered (pattern, repeats) list
```

- **A Pattern's step row *t* plays through Track *t*'s Patch.** There is no separate "kit" object: a drum kit is a drum-mode Patch on a Track (`instrument-model.md` §8, already decided). Loading a different Patch into a Track changes what every Pattern's row *t* sounds like — which is the point, and how a Song can be re-voiced.
- **Tempo belongs to the Song.** Today it is a single project-level value (`sequencer.md` §3 "Tempo, master params"); a Song that carries its own tempo is what "arranged over time at a tempo" means. The project keeps a default for pattern-mode playback with no Song selected.
- **Swing (*)**: built as *pattern*-level (`Pattern::swing`, 50–75), and that is musically right — different patterns can have different feel. The Song's swing is a **default** that a Pattern follows unless it sets its own (`swing = 0` meaning "follow song"). Confirm or simplify (§9 item 8).
- **Default length 32.** `pattern.hpp` supports 1–64 steps and defaults to 16; the default becomes 32 (2 bars × 16 sixteenths). No model change, one constant plus the step-editor UI showing two bars.
- Steps carry `{on, velocity, probability, micro_offset, retrig, param_locks[≤4]}` (built) — "notes/volumes" is `on`/`velocity`; the rest is already there for later.

---

## 4. Sample residency: one registry

Today there are **two** sample registries: `audio_engine.cpp`'s `s_loaded_samples[]` (bare WAVs, pushed to the ESP32 as `MSG_SAMPLE_META`, visible in the Sample Manager, editable in Sample Edit) and `sfz_loader.cpp`'s private `s_loaded_samples[]` + `Sfz::SampleTable` (an import's samples: invisible to every page, un-editable, released wholesale when the next import replaces them). `Instrument::origin` (2026-09-02) exists to tell a zone which registry its ids mean. Three of the four items in the backlog's residency entry and two of the three 2026-09-02 bench findings (§8 stage 0) are this split.

End state: **one registry, refcounted by path**, exactly as `instrument-model.md` §4 specified and never built:

- `SampleRegistry` (shared, host-testable, HAL-free over a `SampleMemMgr`): `{sample_id, path hash, refcount, handle, LoadedSampleInfo record}`; capacity **1024** (was 32). Ids are registry-allocated, unique for the life of the boot; `sample_id` is `uint16_t` on the wire, so 1024 is well inside the id space.

  1024 is a deliberate stretch past what the Track/zone model alone needs — 16 Tracks x 32 zones tops out at 512 zone references — so that a sample library can stay resident across Instrument changes instead of being reloaded per swap. Five constraints come with it, and none of them are satisfied by simply raising the constant:

  - **The registry must be indexed, not scanned — measured, and it does not survive 1024.** This is the constraint that actually breaks, and it is not a bandwidth problem. `find_loaded_sample` (`audio_engine.cpp`) is a linear scan, and `ResolveNoteOn` calls the resolver once per zone that passes key/velocity filtering, stopping only after `kMaxLayerTriggers` (4) zones resolve successfully — so a note where many zones match but fail to resolve walks up to `kMaxZones` (32) scans.

    Measured on the Daisy at 480 MHz (DWT, `BenchmarkRegistryScan`, worst-case miss scan, nothing else resident):

    | entries | ns/scan | ns/entry | x32 zones |
    |--:|--:|--:|--:|
    | 32 (SRAM, today) | 1 412 | 44 | 45 us |
    | 32 (SDRAM) | 1 633 | 51 | 52 us |
    | 128 | 7 762 | 60 | 248 us |
    | 512 | 29 120 | 56 | 931 us |
    | **1024** | **205 645** | **200** | **6 580 us** |

    Three things that estimation would have got wrong:

    - **Per-entry cost is not flat.** It holds near 51-60 ns to 512 entries, then jumps ~3.5x to 200 ns at 1024. The working set crosses ~123 KB there and cache/SDRAM row behaviour changes, so the curve cannot be linearly extrapolated — which is precisely why this needed measuring rather than reasoning.
    - **SDRAM is barely the issue.** SRAM vs SDRAM at 32 entries is 44 vs 51 ns/entry, ~16%. Entry count and the 120-byte stride dominate, not which memory the table sits in.
    - **The 32-zone worst case is 6.6 ms**, past the `< 5 ms` in-to-sound budget the note path documents — from lookup alone, on a main loop that also refills SD. Typical cost is far lower (a hit averages half a scan, and most instruments match 1-4 zones, so ~0.1-0.4 ms at 1024), but the worst case is a real instrument shape, not a pathological one.

    The fix was measured too, at 1024 entries, both variants still reading the record from SDRAM afterwards because a real resolve has to:

    | lookup | ns/lookup | x32 zones | vs linear |
    |---|--:|--:|--:|
    | linear scan (today's shape) | 188 391 | 6 028 us | 1x |
    | binary search over a 4 KB SRAM index | 990 | 32 us | **190x** |
    | id encodes its own slot — no search | 330 | 11 us | **575x** |

    Both clear the budget by two orders of magnitude, so the choice is not about speed. What the numbers show is that **the floor is the record read, not the search**: direct indexing costs ~330 ns, which is essentially one random 120-byte SDRAM record access — the thing you must do regardless. Binary search costs 3x that, and the extra is the search itself (10 unpredictable branches plus index probes), not memory.

    That is also why **a hash buys nothing here.** A hash and a direct index both end at "compute a position, read the record", so both land on the same ~330 ns floor; the hash only adds a collision path and a table to size. Prefer making `sample_id` carry its own registry slot — e.g. slot index in the low bits, a generation counter in the spare high bits, with the record's own id re-checked on read so a stale id fails instead of silently resolving to whatever recycled that slot. That is O(1), needs no auxiliary structure and no extra memory at all, and it is the cheapest thing measured. Ids become registry-allocated under this design anyway, so this is the moment to pick the encoding.

    Do not ship the linear scan at this capacity.

  - **The registry table cannot live in Daisy SRAM.** `sizeof(LoadedSampleInfo)` is **120 B** (measured — it embeds an 88 B `SampleMetadata`), so 1024 entries is **120 KB** against roughly 140 KB of SRAM left at 73% used. Taking ~86% of the remaining internal RAM for one table is not viable, so the records belong in SDRAM (64 MB, currently unused) — which means the registry is only available on boots where SDRAM came up, the same way `sdram_available` already gates the SFZ loader. Note this is the *records*; the 4 KB lookup index above still belongs in SRAM.
  - **Audio data, not table size, is the real cap.** The sample arena is ~60 MB; 1024 resident samples means averaging <60 KB each (~0.6 s of 16-bit mono at 48 kHz). 1024 is the ceiling the table permits, not a working set to expect. Admission still runs through `WAVEX_INST_LOAD_RESERVE_BYTES`, and eviction must stay explicit rather than the silent oldest-first drop `audio_engine.cpp` does today.
  - **The frontend cannot mirror the whole registry.** `SampleMetadata` is 88 B, so 1024 records is 88 KB — PSRAM on the ESP32-P4, not internal SRAM. At UART4's 2 Mbaud (~200 KB/s at 8N1) a full mirror is ~510 ms, so it cannot be pushed eagerly on every change. The Sample Manager needs a windowed/paged query (`MSG_SAMPLE_META_REQ` by range) rather than the current "push everything, cache 8" model. A ~20-row visible window is ~2 KB, about 10 ms — comfortable against a 500 ms refresh.
  - **A page must be ONE batched message, not one per record.** `UART_MAX_PAYLOAD` is 2048, so ~23 `SampleMetadata` records fit in a single message, and the Daisy TX queue is only 4 deep (`daisy_uart_link.cpp` `MSG_QUEUE_SIZE`). Replying to a page with 20 separate messages drops most of them as queue overflow — exactly the defect the `MSG_TRACK_BINDING` broadcast had before it was changed to drain a bounded number per main-loop pass.
- `Load(path)` returns the existing entry (refcount++) if the path is already resident. Binding a Patch to a track refs its samples; unbinding/replacing derefs; refcount 0 frees — after the voice-stop handshake, which becomes **per-track** (`VoiceManager::StopSlot()` is already built; `s_voice_stop_all` stops being the only tool).
- The Sample Manager lists the registry — every resident sample, whoever loaded it, with a "used by: Track 3 (Piano), Track 7" column. Sample Edit can edit any of them; an import's samples get markers like any other.
- `MSG_SAMPLE_META` capacity on the ESP32 (8 today, the "can only describe 8 of 32" backlog item) grows, but at a 1024 registry it **pages** rather than mirrors — see the frontend constraint above. The Sample Manager's current "probe ids 1..64 every 500 ms" rebuild does not survive this and becomes a range query over the visible window.
- `Instrument::origin` then means only "imported vs. built on-device" for the UI, not "which registry". `SfzLoader::ForgetLoadedSample` and the bridging resolver go away; there is one `SampleResolver`.

This is what makes **two soundfonts at once** ordinary: a Patch on track 1 and a Patch on track 2 hold refs into the same registry; loading a third evicts nothing unless memory is short. Budget: `WAVEX_INST_LOAD_RESERVE_BYTES` (8 MB) stays as the guard; the RAM-resident cap per sample (`WAVEX_INST_MAX_RAM_SAMPLE_BYTES`, 4 MB) stays until streamed zones exist.

---

## 5. Polyphony

`kNumVoices = 8` is a **measured DTCM/CPU budget**, not a design choice, and the SVF's per-voice cost was never measured on hardware (`digital-voice-audition.md` Stage 1 said it must be). Before any number is promised: read the DWT callback-cycle counters with 8 voices sounding through the filter. If there is headroom, 12 or 16 is a one-constant change plus the linker report.

Allocation policy, one shared pool (per-track reservations waste voices on an 8-voice machine):

1. A free voice, if any.
2. Else, if the requesting track is at its `poly_limit`, steal **its own** oldest voice (a mono bass track retriggers itself; a 4-voice pad track cycles its own chord). This is how a drum track never cuts a pad and vice versa.
3. Else steal globally: a voice in its release tail first, then the oldest voice on the **lowest-priority** track, then the oldest overall. (Today: release-tail first, then oldest — `FindVoiceToSteal()`; this adds the two track-aware steps in front of it.)

Choke groups stay per-Patch (drum mode). All of this is `VoiceManager` logic, host-testable, no protocol change beyond `MSG_TRACK_OP`.

---

## 6. UI map

Pages reorganised around the two nouns. Each page owns exactly one thing.

| Page | Owns | Exists today as |
|---|---|---|
| **Track** | which track is selected (a global the other pages follow), its Patch name, MIDI in, poly limit, mixer strip, Load Patch / Save Patch | nothing; the "Slot" parameter on Play/Sample Manager/Voice pages is a partial stand-in |
| **Patch** | the selected track's Patch: Sample/Zones tab, Filter, Env, Mod, Tune, Name/Tags | "Voice" page (rename; add Mod and Name/Tags tabs; retire the SLOT param — the Track page owns that) |
| **Patch Browser** | `0:/wavex/patches/*.wxi` and `*.sfz`; preflight; load into the selected track; tag filter | Sample Browser's `.sfz` handling, split out |
| **Pad Map** | Drum-mode Patch: 16 pads × sample (instrument-model.md §12.1) | nothing |
| **Sample Browser** | what is on the card; audition; load into the registry | exists; loses its `.sfz` duties |
| **Sample Manager** | what is resident; who uses it; unload; **set current sample for editing** | exists; today cannot see an import's samples (§4) |
| **Sample Edit** | the **current sample**'s markers/gain/fades | exists; today only knows the Browser's last load (§8 stage 0) |
| **Play** | the grid, addressed to the selected track | exists; loses its Slot param (follows the Track page's selection) |
| **Mixer** | 16 strips | `output-routing-and-mixer.md` stage 3, not built |

"Selected track" and "current sample" are the two pieces of shared UI state; both are ESP32-side, both survive page navigation like `SampleBrowserState` does.

---

## 7. Protocol deltas (summary)

| Change | Kind |
|---|---|
| `NoteMessage::channel` bit 7 = `NOTE_ADDR_TRACK` | additive; MIDI task never sets it, internal sources always do |
| `MSG_TRACK_OP {track, op, value}` | new message |
| `INST_OP_SET_PAD_SAMPLE`, `INST_OP_SAVE`, `INST_OP_NEW`, `INST_OP_SET_NAME`, `INST_OP_SET_DEFAULTS` | new ops on the existing `MSG_INST_OP` |
| `MSG_INST_OP` load path accepts `.wxi` as well as `.sfz` | behaviour |
| `MSG_SAMPLE_META` gains `ref_count` / `used_by` bitmask (16 bits) | additive field |
| Doc rename: slot → track in `inter-mcu-protocol.md` | docs |

`PROTOCOL_VERSION` does not need to move for any additive change here; `NOTE_ADDR_TRACK` is the one to think about, and it is backward-compatible (an old Daisy masks `& 0x0F` and behaves as today).

---

## 8. Stages (one verified commit each; order matters where marked)

0. **Bench findings, 2026-09-02** — fix before anything else, all small: Sample Edit gets a "current sample" chosen from the Sample Manager; SFZ import falls back to a depth-limited subfolder search for a sample it cannot find at the resolved path; the Sample Manager says *why* an import's samples are absent until §4 lands (a status line, not a fix). See `backlog.md`.
1. **Rename** — UI strings and docs: Slot → Track, Voice page → Patch. Code identifiers in a second, purely mechanical commit (`kNumInstrumentSlots → kNumTracks`, `Slot() → Track()`, `Voice::slot → Voice::track`, `NoteMessage::channel` documented as track-or-channel). No behaviour change; builds and tests only.
2. **Track model + MIDI routing** — `Track` struct, `midi_in`, `NOTE_ADDR_TRACK`, Daisy-side fan-out, `MSG_TRACK_OP`, remove the ESP32 global channel filter. Host tests for the router. Track page (minimal: select track, MIDI in).
3. **Shared sample registry** (§4) — the big one; dissolves single residency, makes an import's samples visible/editable, enables two soundfonts. `SfzLoader` → `PatchLoader` over the registry; per-track stop handshake. Sample Manager shows "used by".
4. **Patch completion** — name/tags/trim/transpose, patch-level defaults (§3.2, retiring `ZONE_FLAG_LIVE_FILTER_ENV`), `.wxi` reader/writer (host round-trip), `INST_OP_SAVE/NEW/SET_NAME`, Patch Browser, Patch page's Save/Load. Pad Map op + page (instrument-model.md §12.1/12.3) fits here or before 3 — it does not depend on the registry.
5. **Polyphony policy** (§5) — measure first; `poly_limit`, `priority`, track-aware steal.
6. **FX** — reserved chunk only; no design here.

Stages 1, 2 and 4's pad-map piece are independent of 3 and can be done in any order. Nothing in Goal B (`digital-voice-audition.md` stages 5–8) waits on any of this except that the sequencer addresses tracks with `NOTE_ADDR_TRACK` from stage 2 on.

---

## 9. Decisions to confirm

1. **Names**: Track / Patch / Voice(engine-only) as in §1. Alternatives considered: Channel (rejected: collides with MIDI once routing is configurable), Preset (rejected: wanted for device-state snapshots), Program (MIDI-accurate but unfamiliar as a noun), Sound/Kit (Elektron/MPC; "Kit" stays as the informal name for a drum-mode Patch).
2. **Routing on the Daisy** (§2.2) rather than the ESP32.
3. **Trim on the Patch, fader on the Track** (§3.1) — i.e. loading a Patch never moves the mixer.
4. **Tags as a fixed bitmask** (§3.4) for v1.
5. **Registry capacity 128 and per-track voice-stop** (§4) — or a smaller number if SDRAM accounting says so.
6. **Measure before choosing the voice count** (§5): commit to 8 in the UI only after the DWT numbers are in.
7. **Tempo on the Song**, project-level default for pattern mode (§3.5).
8. **Swing: Song default, Pattern override** (§3.5) — or strictly Song-level, dropping `Pattern::swing`.
9. **Default pattern length 32** (§3.5).

Items 1 (Track, Patch), and the Pattern/Song definitions, were **confirmed 2026-09-02**. Items 2–6 are taken as accepted by default unless objected to; 7–9 are the new ones.

---

## 10. Deliberately out of scope

Streamed (disk) zones; FX design; per-zone editors beyond the Pad Map; multi-output routing (Stage B); song/project files (they reference Patches by path and are `sequencer.md`'s). The wavetable renderer is also deliberately deferred and unscheduled; only the typed source boundary and ownership rules are established here.
