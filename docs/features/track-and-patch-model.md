# Track and Instrument Model — the user-facing paradigm and its end state

**Status**: **Proposed 2026-09-02; all decisions taken 2026-09-03/04** (see §9). Written in response to direct requests for an end-state design so that the Phase 2/2.5 UI and voice work converges on one paradigm rather than accreting. Track/Instrument/Pattern/Song/Bank vocabulary is confirmed; the ownership hierarchy is accepted architecture; the two-oscillator Instrument (§3) is *designed now, implementation order to be decided* (§8). Nothing here is built as one unit; the pieces that already are, are marked. The file keeps its historical name so links from code comments, commits and other docs stay valid.
**Supersedes, in vocabulary only**: "slot" (`instrument-model.md` §1), "Voice" as a page/entity name, and **"Patch"** (used 2026-09-02 to 2026-09-03 for what is now called an Instrument — the user chose Instrument on 2026-09-03 because it is the natural noun for a sampler and matches the `.wxi` extension). The engine-side data model in `instrument-model.md` stands; this document says what it is *called*, what it grows into, and how Tracks, MIDI and memory are arranged around it.
**Dependencies**: `instrument-model.md` (the sampler `Instrument`/`Zone` model, built), `oscillator-sources.md` (typed sampler/wavetable boundary), `param-locks-and-modulation.md` (mod matrix, envelopes, LFOs — partly built), `output-routing-and-mixer.md` (`TrackMix`, built), `sequencer.md` (tracks, built core), `wxcf.hpp` (built).

---

## 1. Vocabulary

The words the UI, the docs and the code use. Engine identifiers were renamed to match in stage 1 (2026-09-04); the "engine identifier" column records what they were called before, for reading older history.

| Term | Meaning | Was called | Engine identifier today |
|---|---|---|---|
| **Sample** | A resident PCM buffer with its own record (markers, gain, name), loaded from an SD asset. Referenced by Instruments; owned by nobody but the Sample Pool. | sample, WAV | `LoadedSampleInfo` / `s_loaded_samples` (bare) and `Sfz::SampleTable` (import) |
| **Sample Pool** | The single registry of resident Samples (§4): every Sample the Daisy holds, whoever loaded it, refcounted by path. What the Sample Manager lists. | registry, loaded samples | two registries today; `SampleRegistry` target |
| **Asset** | A stored audio file on the SD card: a recording, a wavetable, an `.sfz` set. Becomes a Sample when loaded into the Pool. | file | SD path + sidecar metadata |
| **Oscillator** | One of an Instrument's **two** typed sound sources (§3.1): a *Sample* oscillator (a Zone map over Samples — a multisample or a pad map) or, later, a *Wavetable* oscillator. Type determines playback semantics (`oscillator-sources.md`). | oscillator source, layer | sampler `Zone[]` today; no typed `Oscillator` yet |
| **Zone** | One Sample mapped to a key × velocity range inside a Sample oscillator. Unchanged. | zone | `Zone` |
| **Instrument** | **Confirmed 2026-09-03 (replacing "Patch"):** one playable, named, tagged, saveable sound: two Oscillators → submix → Filter → Amp/Pan, with three envelopes, two LFOs and a mod matrix (§3). Saved as `.wxi`. Gets loaded *into* a Track. A drum **Kit** is a drum-mode Instrument, not another kind of thing. | Voice, Patch, Preset, Program, Sound | `Instrument` (`instrument.hpp` — the name was already right) |
| **Track** | **Confirmed 2026-09-02:** one sequencer track (formerly "slot") *and* one space in which an active Instrument can be loaded, responding to MIDI messages on its designated channel. Sixteen of them. Also owns a mixer strip and a polyphony limit. What a note is *addressed to*. | slot, instrument slot, channel | `kNumTracks`, `Tracks::Track()`, `Voice::track`, `NoteMessage::channel` (was `kNumInstrumentSlots`, `InstrumentBank::Slot()`, `Voice::slot`); `pattern.hpp`'s per-track step row is `TrackSteps` (was `Track`) so the word means one thing |
| **Bank** | **Confirmed 2026-09-04:** a numbered table of **128 Instrument slots** (0–127) saved as one self-contained file, `.wxb`, addressable by MIDI Program Change (§3.6). A Bank is *storage* — loading slot *n* copies that Instrument into a Track; a Bank never owns runtime sound. | bank, sound pool, program bank | none; the 16 runtime Tracks are `Tracks` (was `InstrumentBank`) so "Bank" means only this |
| **Voice** | One of `WAVEX_NUM_VOICES` polyphony channels rendering one resolved note through an Instrument's signal path. An *engine* term; it never names a page or a user entity again. Track → Instrument → Voice is a dynamic allocation from one shared pool, never a static Track↔Voice mapping (§5). | voice | `Voice`, `WAVEX_NUM_VOICES` (`hardware_config.h`; was `kNumVoices`) |
| **Performance** | The current live configuration of all Tracks: Instrument bindings, MIDI routing, mixer, mutes, and shared effects. V1 stores one Performance directly in the Project; it is an ownership concept, not a separate file yet. | multi, part, performance | Project's target `Tracks[16]` plus mixer state |
| **Pattern** | A group of notes/velocities over a fixed span — default **2 bars of 16ths = 32 steps** — with one step row per Track. The sequencer's unit of composition. | pattern | `Pattern` (`pattern.hpp`, built: 1–64 steps, default 16 → 32) |
| **Song** | An ordered arrangement of Patterns over time, at a tempo and swing setting. | song, chain | `sequencer.md` §3's `Songs[≤16]: (pattern, repeats)` — not yet in code |
| **Scene** | A performance snapshot of mixer, macros, mutes, and optional Pattern selection. It references content and never embeds Samples, Instruments, or Pattern data. | performance | target only; `scenes-and-performance.md` |
| **Project** | The portable root that stores one Performance, Patterns, Songs, Scenes, settings, the current Bank, and references to saved Instruments/assets. | project | `.wxp` target; not yet in code |
| **MIDI channel** | 1–16 on the DIN/USB input. A *routing input* to Tracks, never the same word as Track. | channel | `MidiEvent::channel` |
| **Mod slot** | One row of an Instrument's modulation matrix. Keeps "slot" — it is the conventional word there and it is never confused with a Track once Track exists. | mod slot | `ModSlot`, `Instrument::mod_slots[8]` |

Why **Track** and not Channel: with MIDI routing configurable (§2.2), track≠channel is the whole point, and the sequencer and mixer already say "track". Why **Instrument** and not Patch/Preset/Program: it is what E-mu, Akai and every sampler manual call the thing a keyboard plays; "Preset" is wanted for saved *device state* (scenes); "Program" is MIDI-accurate but unfamiliar as a noun; "Patch" was tried and felt wrong for a sampler. Tracks are numbered **1–16 on screen** and 0–15 on the wire, formatted only through `trackDisplayNumber()` (`ui/current_track.h`, built 2026-09-04).

### 1.1 Ownership hierarchy

The hierarchy is a set of ownership and reference rules, not a requirement that
every object be serialized inside the object above it:

```text
SD assets ──load──▶ Sample Pool (resident Samples, refcounted) ◀──┐
                                                                  │ Oscillator Zones reference Samples
Bank (.wxb): Instrument slots[128] ──copy into──▶ Track           │
                                                                  │
Project                                                           │
├── Performance                                                   │
│   └── Tracks[16]                                                │
│       ├── active Instrument ────────────────────────────────────┘
│       └── MIDI routing, polyphony, mixer strip
├── Patterns -> TrackSteps[16] -> Tracks
├── Scenes   -> performance state + optional Pattern reference
└── Songs    -> ordered Pattern references, repeats, tempo, overrides
```

The resulting save policy is explicit:

- Assets belong to the card and are referenced by stable path; Samples belong
  to the Pool and are referenced by runtime id only inside the engine — never
  in a file.
- An Instrument owns sound design: its Oscillators (and their Zones), filter,
  amp, envelopes, LFOs and mod matrix. A drum Kit is a drum-mode Instrument,
  not another layer.
- A Track owns the active Instrument binding, MIDI routing, polyphony policy,
  and mixer strip.
- The Performance is the live set of all Track/Instrument bindings, routing,
  mixer state, mutes, and shared effects. V1 stores one directly in the Project.
- A Pattern owns musical time: notes, triggers, probability, micro-timing, and
  parameter locks. It does not own or silently replace Track Instruments.
- A Scene owns recallable performance state, not content.
- A Song owns form by arranging Pattern references and overrides.
- A Bank owns 128 saved Instruments as a unit. It is a file: loading from it
  copies; nothing plays "from the Bank".
- A Project owns the portable working set and its references. Export/snapshot
  may copy referenced assets; normal save does not duplicate them.

This gives the Performance an explicit stable boundary: Pattern changes
preserve the Track/Instrument setup unless an explicit Scene or Project action
changes it. Multiple independently named Performances or Parts can be added
later without changing what a Pattern owns.

### 1.2 Oscillator type is below Instrument, Voice is below note resolution

Sampler and wavetable data may both be PCM, but they are different oscillator
contracts. A Sample oscillator maps arbitrary recordings through Zones; a
future Wavetable oscillator scans fixed-length single-cycle frames. They share
the Instrument, Track, Pattern, and Song hierarchy, not source-specific
assumptions. See `oscillator-sources.md`.

An Instrument is the saved blueprint. A Voice is the temporary runtime
allocation created after a Track's Instrument resolves a note or trigger. UI
and persistence must never use "Voice" as a synonym for Instrument.

### 1.3 Nothing claims a Track without asking (rule, 2026-09-03)

Anything that binds, replaces or reserves a Track — loading an Instrument,
loading a Sample onto a Track, recalling a Bank slot from the UI — names the
Track it will use and, **if that Track already holds something, asks before
replacing it** ("Track 3 holds *Piano* — replace?"). An empty Track needs no
prompt. The boot-time SFZ autoload that silently owned Track 1 was the
mechanism behind the 2026-09-03 "Select does nothing" finding
(`WAVEX_DAISY_SFZ_BOOT_ENABLED` now defaults to 0). MIDI Program Change is the
one exception (§3.6): it is addressed to a Track by the user's own routing and
cannot wait for a dialog.

---

## 2. Track

### 2.1 Fields (engine-resident, host-testable)

```cpp
struct Track {
    Instrument  instrument;       // the resident Instrument (instrument.hpp), by value as today
    uint8_t     midi_in;          // 0 = Omni, 1..16 = that channel, 0xFF = Off
    uint8_t     poly_limit;       // 1..WAVEX_NUM_VOICES; 0 = no limit (default)
    uint8_t     priority;         // steal priority, 0 = lowest (default), see §5
    uint8_t     program_change;   // 1 = Program Change on midi_in recalls Bank slots (§3.6), 0 = ignore
    // TrackMix strip lives in TrackMixer (built); sequencer lane in Pattern (built).
};
```

`Tracks` (renamed from `InstrumentBank` in stage 1) holds 16 of these behind `Track()`. Nothing about `Instrument` changes for this step.

### 2.2 MIDI routing: poly, omni, and everything between

A note from the **MIDI input** carries a channel. A note from an **internal source** (Play page grid, sequencer, arpeggiator) is addressed to a track directly. These are different things and the wire must say which:

- `NoteMessage::channel` gains an addressing bit: `NOTE_ADDR_TRACK = 0x80`. Set → `channel & 0x0F` is a track index (internal sources). Clear → it is a MIDI channel (the ESP32 MIDI task forwards raw events unchanged, as it does today).
- The Daisy routes: a MIDI-addressed note-on goes to **every** track whose `midi_in` matches (`Omni`, or equal channel), each resolving through its own Instrument. That is layering for free, and it is exactly standard MIDI behaviour:
  - **Multi/poly mode** (default): track *t* has `midi_in = t+1`. One channel per track, as today.
  - **Omni**: any set of tracks with `midi_in = Omni` all play from any channel. "All tracks omni" is the classic single-timbral mode; "two tracks omni" is a split-less layer.
  - **Off**: sequencer-only tracks that ignore the keyboard.
- Note-off routes identically. Voices already carry their track, so a note-off reaching three tracks releases the right three voices.
- The existing global ESP32-side channel filter (`midi_set_input_channel`) becomes redundant and is removed rather than kept as a second, conflicting filter.

Routing lives on the **Daisy**, not the ESP32, because tracks live there, it is a 16-entry compare in the note handler (main loop, not the callback), and it keeps one note = one link message regardless of how many tracks it lands on. The mixer's "ESP32 expands solo before sending" precedent does not apply: solo is UI state, routing is engine state.

Protocol: `MSG_TRACK_OP` (new; 0x63 from the instrument block, or a fresh id — decide at implementation) with `{track, op, value}`: `SET_MIDI_IN`, `SET_POLY_LIMIT`, `SET_PRIORITY`, `SET_PROGRAM_CHANGE`. Idempotent, tiny, one round-trip test.

### 2.3 What a track does *not* own

Filter/envelope/tuning are Instrument properties (§3). Today's `VoiceLiveParams` is engine-global "what the knobs say" and applies to every track; the end state is that the Instrument page edits **the selected track's Instrument**, and `OnControlChange` writes into that Instrument (its defaults, or a zone override, §3.2) rather than into a global. `ZONE_FLAG_LIVE_FILTER_ENV` — added 2026-09-02 as the bridge for on-device-built instruments — is retired at that point: the flag existed only because nothing could yet write a zone. (`VoiceManager::ApplyLiveParams`, which pushes edits onto *sounding* voices, stays; it just receives the track's values rather than a global.)

### 2.4 The selected Track (shared UI state — built 2026-09-04)

Every page acts on **one** selected Track (`ui/current_track.h`): Play sends notes on it, the Sample Manager assigns to it, the Instrument page edits it, the browser loads into it. Before this existed, four pages kept four private copies, which was the whole of the "which Track?" bench finding. Rules:

- The Track selector shows **eight Tracks per page** (1–8, 9–16); paging, not a 16-wide strip, so the numbers stay legible on the 1280×720 panel.
- The selection is **displayed on every page** as a header chip — `T3 · Piano` — alongside the SHIFT chip, and **changed from anywhere** with one gesture (Shift + encoder, or Track −/+ softkeys on the pages that act on a Track: Play, Instrument, Sample). The Track page (§6) holds what does not fit in a chip: MIDI in, poly limit, program change, mixer strip, Load/Save. Decided 2026-09-04 (§9 item 4); the bench finding was precisely that no page said which Track it was acting on.

---

## 3. Instrument

### 3.1 What it is — the signal path (target, decided 2026-09-04)

An Instrument is a small synthesizer whose oscillators happen to play samples:

```text
Osc 1 (Sample | Wavetable) ─┐
                            ├─ submix (osc mix, osc2 level) ─▶ Filter ─▶ Amp / Pan ─▶ Track mix bus
Osc 2 (Sample)             ─┘                                                       (Stage B: per-voice DAC / TDM slot)

Env 1 → Amp (hard-wired)      LFO 1 (per voice)      Mod matrix: 8 rows
Env 2 → Filter (default)      LFO 2 (per voice)      source → destination × depth × curve
Env 3 → Pitch (default)       + 1 global LFO (engine)
```

Why **submix, then one filter**, rather than a filter per oscillator: Stage B has exactly one analog VCF/VCA per voice (`analog-voice-board.md`), so the digital voice takes the same shape and the analog build is a routing change, not a different instrument; it is also half the filter cost. Both oscillators are resolved from the note at trigger time — each from its **own** Zone map — so a Keyboard Instrument can layer two multisamples and a drum pad can be "body + click". An oscillator whose map has no Zone for the note is silent and costs nothing.

```cpp
// Target shape. Everything below the first line is additive to today's Instrument.
enum class OscType : uint8_t { Off = 0, Sample = 1, Wavetable = 2 /* reserved; 8 values total */ };
enum class FilterType : uint8_t { SvfLp = 0, SvfHp, SvfBp, SvfNotch /* 4 defined; 8 values reserved */ };

struct Oscillator {
    OscType  type;                    // Off / Sample / Wavetable
    float    level;                   // into the submix
    float    pan;
    int8_t   coarse_tune, fine_tune;  // composes with each Zone's own tune
    uint8_t  keytrack;                // 1 = pitch follows the note (Keyboard); 0 = fixed (pads)
    Zone     zones[kMaxZonesPerOsc];  // Sample: the key × velocity map (kMaxZonesPerOsc = 32, as kMaxZones today)
    // Wavetable: table asset + position params, when built — its own chunk, never Zone fields (oscillator-sources.md)
};

struct Instrument {
    char            name[24];            // built 2026-09-04
    uint8_t         tags;                // bitmask (§3.4)
    InstrumentMode  mode;                // Keyboard / Drum (built) — see §3.2
    InstrumentOrigin origin;             // built 2026-09-02; "imported vs built on-device" only, once §4 lands
    int8_t          transpose, fine_tune;
    float           trim_gain, trim_pan; // the Instrument's own level/centre, NOT the Track fader
    Oscillator      osc[kNumOscillators];// 2
    float           osc_mix;             // 0 = osc 1 only … 1 = osc 2 only; mod destination
    struct { FilterType type; float cutoff_hz, resonance, keytrack, env2_amount; } filter;
    struct { uint8_t velocity_curve; }                                              amp;
    Adsr            env[3];              // env[0] amp (hard-wired), env[1] filter, env[2] pitch by default
    LfoParams       lfo[2];              // per-voice LFOs: wave, rate (Hz or tempo division), delay/fade, retrigger
    ModSlot         mod_slots[8];        // built
    uint8_t         output;              // 0 = Track's stereo bus (now); Stage B: DAC/TDM slot. Moves here from Zone::output_bus
    uint8_t         poly_mode;           // poly / mono / legato
    // Reserved, not fields yet: FX selection + params (a WXCF chunk id is reserved so files written
    // before FX exists load after it).
};
```

**What exists today** (`voice_manager.hpp`, `instrument.hpp`): one Sample oscillator (zone-resolved), the TPT SVF (lowpass only exposed), Env 1 (amp, per sample), Env 2 (`env2`, block-rate mod source `SRC_ENV_FILTER`), block-rate cutoff/gain/pitch/pan modulation, the 8-row mod matrix with per-trigger sources, and two engine-global LFOs. **Not built**: Osc 2 and the submix, `FilterType`, Env 3, the per-voice LFOs, the typed `Oscillator` wrapper, `output`/`poly_mode` on the Instrument.

**Trim vs. mixer.** An Instrument's `trim_gain/pan` is part of the *sound* ("this piano is quiet"); the Track's `TrackMix` fader/pan/mute/solo is part of the *mix* ("bring the piano down in this song"). Loading an Instrument never touches the mixer strip. This is the same split every DAW and every E-mu makes, and it is what lets an Instrument be reused across projects.

**Filter type.** The TPT SVF (`svf_filter.hpp`) is the v1 filter: it is built, stable under modulation, and already computes LP/BP/HP internally, so exposing the mode is cheap. `FilterType` is a 3-bit field — 8 values reserved, 4 defined — so a 24 dB cascade or a ladder model is a new value and a new renderer, not a file-format change.

**Envelopes.** Env 1 is hard-wired to the amp because a voice must always have one. Env 2 and Env 3 reach their defaults *through the mod matrix*: the Filter page's "Env amount" and the Osc page's "Pitch env" knobs edit those two matrix rows, so the common case needs no matrix editing and the matrix stays the single truth. All three envelopes are mod sources (`SRC_ENV1..3`; today's `SRC_ENV_FILTER` keeps its wire value and becomes Env 2).

**LFOs.** Two per-voice LFOs owned by the Instrument (rate, wave, delay/fade, retrigger — the E-mu delayed-vibrato shape) plus **one** engine-global LFO (free-running or tempo-synced; the "modular's LFO bank" case, `param-locks-and-modulation.md` §5). An Instrument must sound the same on any Track, which is why its vibrato and wobble are its own; the global one is for performance-wide movement. Today's `SRC_LFO1` becomes the global LFO, `SRC_LFO2` is retired-but-reserved (reads 0, like `SRC_PARA_ENV`), `SRC_LFO_VOICE` is voice LFO 1, and `SRC_LFO_VOICE2` is appended.

**Mod matrix destinations** grow, append-only, from today's `CUTOFF / GAIN / PITCH / PAN` to add `RESONANCE`, `OSC1_PITCH`, `OSC2_PITCH`, `OSC_MIX`, `OSC2_LEVEL`, `WT_POS1`, `WT_POS2` (wavetable position — the accumulator the wavetable design scrubs), `LFO1_RATE`, `LFO2_RATE`. Evaluation stays control-rate, per voice, per `param-locks-and-modulation.md` §3.

### 3.2 One Instrument type, two editors (drum pads vs. keyboard ranges)

Akai has two program types — *Drum* (a pad is one fixed-pitch sound with its own filter/envelopes) and *Keygroup* (a key range pitch-tracks one multisample, parameters per range). The differences are only (a) whether pitch tracks the note and (b) how finely the voice parameters are set. WaveX does not need two types: `InstrumentMode::Drum` (built) is "zone per key, note forced to root, no pitch tracking" and `Keyboard` is "ranges that pitch-track", over the **same** `Zone` model; and the "per-pad parameter set" is the per-zone override below. So there is one Instrument type and **two editors** — a **Pad Map** (16 pads, per-pad Sample, choke, and optional per-pad filter/env) and a **Key Map** (key/velocity ranges over a keyboard) — chosen by `mode`. Decided 2026-09-04 (§9 item 8).

**Instrument-level defaults vs. zone overrides.** Today every zone carries its own cutoff/ADSR (from SFZ, where that is normal). A user building an Instrument on-device should not have to set ADSR on 16 pads. So: Instrument-level `filter`/`env` values, and a per-zone "override" flag (`ZONE_FLAG_OWN_FILTER_ENV`, the inverse of today's live flag). The Instrument page's Filter and Env tabs edit the Instrument; the Pad Map can override per pad (the MPC drum case). SFZ import sets the override flag on every zone (it always has per-zone values), so imported Instruments sound exactly as they do now.

### 3.3 Persistence: `.wxi` over WXCF

`instrument-model.md` §5 specified the container; nothing implements the Instrument reader/writer yet. Chunks are laid out so that a file written before a feature exists still loads after it (readers skip unknown chunks), and so that an oscillator's chunk is typed by the oscillator, never by the Instrument:

| chunk | payload | status |
|---|---|---|
| `HEAD` | name, tags, mode, transpose/fine, trim gain/pan, output, poly mode | — |
| `OSC1`, `OSC2` | `{type, level, pan, tune, keytrack}` then a type-specific body: **Sample** = Zone array, each Zone **plus its Sample's card path** (never a runtime id) and per-zone overrides; **Wavetable** = its own versioned body (`oscillator-sources.md`) | — |
| `FILT` | type, cutoff, resonance, keytrack, env2 amount | — |
| `AMP` | velocity curve | — |
| `ENV1`, `ENV2`, `ENV3` | ADSR each | — |
| `LFO1`, `LFO2` | per-voice LFO params | — |
| `MODM` | the 8 mod rows | — |
| `FXCH` | *reserved*, empty — readers skip it today | — |

Load: parse, dedupe paths, load Samples through the Pool (§4), bind. Exactly the SFZ loader's pipeline with a different front half — `SfzLoader`'s phases (`Probe → AwaitVoiceStop → Allocate → Read → Commit`) become a generic **InstrumentLoader** with two parsers feeding the same `MappedInstrument`. **An `.sfz` is an import format for an Instrument, not a different kind of thing**; "Save" always writes `.wxi`. The Instrument Browser lists both.

Location: `0:/wavex/instruments/<name>.wxi`, Samples referenced by absolute card path. A "Save with samples" (copy referenced WAVs into `instruments/<name>/`) is a later convenience, not v1. Card layout, so user data stays apart from everything else:

```text
0:/wavex/samples/      user WAVs, any subfolders (Browse root)
0:/wavex/sfz/          imported multisample sets (as today)
0:/wavex/instruments/  *.wxi
0:/wavex/banks/        *.wxb  (+ optional <name>/samples/ for "save with samples")
0:/wavex/projects/     *.wxp
0:/wavex/recordings/   Record page output
```

### 3.4 Tags

A fixed vocabulary, bitmask: `Drum, Bass, Lead, Pad, Keys, FX, Vocal, Loop`. Free-text tags need a string table, a search UI and a keyboard; a bitmask needs eight checkboxes and a filter row in the browser. Start there; the WXCF chunk can grow.

### 3.5 Pattern and Song (the sequencer's nouns)

Confirmed 2026-09-02 alongside Track and Instrument, so that the four words are defined in one place. The sequencer design proper is `sequencer.md`; this section records only how these nouns relate to Tracks and Instruments and where the current model differs.

```
Project (.wxp)
├── Tracks[16]      : Instrument (by .wxi/.sfz path), midi_in, poly_limit, TrackMix strip   ← §2
├── Bank            : path of the current .wxb (§3.6)
├── Patterns[≤128]  : length (default 32 = 2 bars of 16ths), scale, swing*, TrackSteps[16]
└── Songs[≤16]      : tempo, swing*, ordered (pattern, repeats) list
```

- **A Pattern's step row *t* plays through Track *t*'s Instrument.** There is no separate "kit" object: a drum kit is a drum-mode Instrument on a Track (`instrument-model.md` §8, already decided). Loading a different Instrument into a Track changes what every Pattern's row *t* sounds like — which is the point, and how a Song can be re-voiced.
- **Tempo belongs to the Song.** Today it is a single project-level value (`sequencer.md` §3 "Tempo, master params"); a Song that carries its own tempo is what "arranged over time at a tempo" means. The project keeps a default for pattern-mode playback with no Song selected.
- **Swing (*)**: built as *pattern*-level (`Pattern::swing`, 50–75), and that is musically right — different patterns can have different feel. The Song's swing is a **default** that a Pattern follows unless it sets its own (`swing = 0` meaning "follow song"). Confirm or simplify (§9 item 8 of the 2026-09-02 list).
- **Default length 32.** `pattern.hpp` supports 1–64 steps and defaults to 16; the default becomes 32 (2 bars × 16 sixteenths). No model change, one constant plus the step-editor UI showing two bars.
- Steps carry `{on, velocity, probability, micro_offset, retrig, param_locks[≤4]}` (built) — "notes/volumes" is `on`/`velocity`; the rest is already there for later.

### 3.6 Bank: 128 Instruments in one file (`.wxb`)

Every machine surveyed (Digitakt's 128-Sound pool, Octatrack's 128 Flex slots, E-mu's bank image, EP-133's numbered sounds, MIDI itself) has a numbered table of ready sounds. WaveX's is the **Bank**:

- **Shape**: 128 slots, each empty or one embedded Instrument — the same chunk stream as a `.wxi`, nested under a per-slot `INST` chunk, so the two readers are one reader. Samples are referenced by card path, as in `.wxi`. `file_type` is allocated in `wxcf.hpp` alongside the others.
- **Residency**: only the Bank's *index* (128 × name/tags/offset, ~5 KB) is resident; an Instrument's chunk is read when its slot is recalled. Its Samples come from the Pool, so switching between Instruments of one Bank loads only the Samples not already resident — this is the workload the 1024-entry Pool (§4) is sized for. A "Preload Bank samples" action pulls every referenced Sample into the Pool up front, subject to the same admission rules.
- **Recall** (`MSG_BANK_OP`): `LOAD_BANK path`, `SAVE_BANK path`, `LOAD_SLOT n → track` (UI: with the §1.3 confirmation if the Track is occupied), `STORE_TRACK track → slot n`, `CLEAR_SLOT n`, `PRELOAD`. Status via `MSG_BANK_STATUS` with the same progress shape as `MSG_INST_STATUS`.
- **Program Change**: MIDI PC *n* on a Track's `midi_in` channel loads Bank slot *n* into that Track when `Track::program_change` is on (default on). It replaces without a dialog — the message *is* the instruction, addressed by the user's own routing — but the Track is stopped per-track (§4), never globally. Bank Select MSB/LSB (multiple current Banks) is out of scope for v1; one current Bank per Project.
- **Not** an owner: a Bank never plays. The 16 Tracks hold copies. Editing a Track's Instrument does not write back to the Bank until `STORE_TRACK`; that is what makes "tweak, compare, keep or discard" safe.
- **Location**: `0:/wavex/banks/<name>.wxb`. "Save with samples" copies referenced WAVs into `banks/<name>/samples/` and rewrites the paths, for a self-contained set that survives a card reorganisation.

---

## 4. Sample residency: one registry — the Sample Pool

**Built 2026-09-05** (stage 3): `firmware/shared/audio/sample_registry.hpp` instantiated as `AudioEngine::SamplePool` (`audio/sample_pool.hpp`). Until then there were **two** sample registries: `audio_engine.cpp`'s `s_loaded_samples[]` (bare WAVs, pushed to the ESP32 as `MSG_SAMPLE_META`, visible in the Sample Manager, editable in Sample Edit) and `sfz_loader.cpp`'s private `s_loaded_samples[]` + `Sfz::SampleTable` (an import's samples: invisible to every page, un-editable, released wholesale when the next import replaced them), with `Instrument::origin` telling a zone which registry its ids meant. The design that replaced them, kept here because its measurements are the reason for its shape:

**One registry, refcounted by path** — the Sample Pool the UI names — exactly as `instrument-model.md` §4 specified:

- `SampleRegistry` (shared, host-testable, HAL-free over a `SampleMemMgr`): `{sample_id, path hash, refcount, handle, LoadedSampleInfo record}`; capacity **`WAVEX_SAMPLE_POOL_CAPACITY` = 1024** (was 32; decided 2026-09-04). Ids are registry-allocated, unique for the life of the boot; `sample_id` is `uint16_t` on the wire, so 1024 is well inside the id space.

  1024 is a deliberate stretch past what the Track/zone model alone needs — 16 Tracks × 2 oscillators × 32 zones tops out at 1024 zone references, and a Bank's working set is meant to stay resident across Instrument changes instead of being reloaded per swap. Five constraints come with it, and none of them are satisfied by simply raising the constant:

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
  - **Audio data, not table size, is the real cap.** The sample arena is ~60 MB; 1024 resident samples means averaging <60 KB each (~0.6 s of 16-bit mono at 48 kHz). 1024 is the ceiling the table permits, not a working set to expect. Admission still runs through `WAVEX_INST_LOAD_RESERVE_BYTES`, and **eviction is never silent: a load that does not fit — in bytes or in entries — fails with a reason the UI shows** ("Pool full: 1024 samples", "RAM: 3.2 MB needed, 1.1 MB free"), replacing the oldest-first drop `audio_engine.cpp` does today. The user unloads; the engine never guesses.
  - **The frontend cannot mirror the whole registry.** `SampleMetadata` is 88 B, so 1024 records is 88 KB — PSRAM on the ESP32-P4, not internal SRAM. At UART4's 2 Mbaud (~200 KB/s at 8N1) a full mirror is ~510 ms, so it cannot be pushed eagerly on every change. The Sample Manager needs a windowed/paged query (`MSG_SAMPLE_META_REQ` by range) rather than the current "push everything, cache 8" model. A ~20-row visible window is ~2 KB, about 10 ms — comfortable against a 500 ms refresh.
  - **A page must be ONE batched message, not one per record.** `UART_MAX_PAYLOAD` is 2048, so ~23 `SampleMetadata` records fit in a single message, and the Daisy TX queue is only 4 deep (`daisy_uart_link.cpp` `MSG_QUEUE_SIZE`). Replying to a page with 20 separate messages drops most of them as queue overflow — exactly the defect the `MSG_TRACK_BINDING` broadcast had before it was changed to drain a bounded number per main-loop pass.
- `Load(path)` returns the existing entry (refcount++) if the path is already resident. Binding an Instrument to a track refs its Samples; unbinding/replacing derefs; refcount 0 frees — after the voice-stop handshake, which becomes **per-track** (`VoiceManager::StopTrack()` is already built; `s_voice_stop_all` stops being the only tool).
- The Sample Manager lists the Pool — every resident Sample, whoever loaded it, with a "used by: Track 3 (Piano), Track 7" column. Sample Edit can edit any of them; an import's samples get markers like any other.
- `MSG_SAMPLE_META` capacity on the ESP32 (8 today, the "can only describe 8 of 32" backlog item) grows, but at a 1024 registry it **pages** rather than mirrors — see the frontend constraint above. The Sample Manager's current "probe ids 1..64 every 500 ms" rebuild does not survive this and becomes a range query over the visible window.
- `Instrument::origin` then means only "imported vs. built on-device" for the UI, not "which registry". `SfzLoader::ForgetLoadedSample` and the bridging resolver go away; there is one `SampleResolver`.

This is what makes **two soundfonts at once** ordinary: an Instrument on track 1 and an Instrument on track 2 hold refs into the same Pool; loading a third evicts nothing. It is also what lets a Track holding an import accept a bare-Sample assignment: today `SfzLoader::BindSample` must refuse (the import owns its samples and can only release them through the load handshake), and the UI says so; with the Pool the replacement is a deref plus a per-track stop, behind the §1.3 confirmation. Budget: `WAVEX_INST_LOAD_RESERVE_BYTES` (8 MB) stays as the guard; the RAM-resident cap per sample (`WAVEX_INST_MAX_RAM_SAMPLE_BYTES`, 4 MB) stays until streamed zones exist.

---

## 5. Polyphony

`WAVEX_NUM_VOICES = 8` (`hardware_config.h`, since stage 1; was `kNumVoices` in `voice_manager.hpp`) is a **measured DTCM/CPU budget**, not a design choice, and the SVF's per-voice cost was never measured on hardware (`digital-voice-audition.md` Stage 1 said it must be). It is the only place the digital voice count is written; `voices_` sizes off it and nothing else in the engine hard-codes 8 for polyphony. Decided 2026-09-04:

- It lives in **`hardware_config.h`** next to the other tunables, so 8 → 16 is that one edit plus a DWT measurement and the linker report. `VoiceManager` `static_assert`s against it.
- It is **independent of the analog voice count** (`TdmVoiceSink::kNumSlots` = 8 PCM1690 slots, the 8-group CV calibration tables — hardware facts). The Stage B invariant "voice index == TDM slot == CV group" holds for the first 8 voices; digital voices beyond that render to the stereo codec only. `kMaxMixChannels` (mixer) is a third, unrelated 8.
- The two-oscillator voice (§3.1) roughly doubles oscillator cost per voice while leaving filter, amp envelope and block-rate modulation flat. Before any number is promised: read the DWT callback-cycle counters with `WAVEX_NUM_VOICES` voices sounding, both oscillators active, through the filter. If there is headroom, 12 or 16 is the one-constant change.

Allocation policy, one shared pool (per-track reservations waste voices on a small machine):

1. A free voice, if any.
2. Else, if the requesting track is at its `poly_limit`, steal **its own** oldest voice (a mono bass track retriggers itself; a 4-voice pad track cycles its own chord). This is how a drum track never cuts a pad and vice versa.
3. Else steal globally: a voice in its release tail first, then the oldest voice on the **lowest-priority** track, then the oldest overall. (Today: release-tail first, then oldest — `FindVoiceToSteal()`; this adds the two track-aware steps in front of it.)

Choke groups stay per-Instrument (drum mode). All of this is `VoiceManager` logic, host-testable, no protocol change beyond `MSG_TRACK_OP`.

---

## 6. UI map

Pages reorganised around the nouns. Each page owns exactly one thing.

| Page | Owns | Exists today as |
|---|---|---|
| **Track** | which Track is selected (8 per page, §2.4), its Instrument name, MIDI in, poly limit, program-change on/off, mixer strip, Load / Save Instrument | the selected Track (`current_track.h`) exists; the page does not |
| **Instrument** | the selected Track's Instrument. Tabs: **Osc** (1/2, type, level/pan/tune, → Key Map or Pad Map by mode), **Filter**, **Amp**, **Env** (1/2/3), **LFO** (1/2), **Mod**; Name/Tags on the Track page's Save | Instrument page (renamed from "Voice" in stage 1, 2026-09-04; Sample/Env/Amp/Filter/Mod tabs exist; its TRACK param follows the selected Track — done 2026-09-04) |
| **Key Map** | Keyboard-mode oscillator: zones over key × velocity ranges | nothing |
| **Pad Map** | Drum-mode oscillator: 16 pads × Sample, choke, optional per-pad filter/env (instrument-model.md §12.1) | nothing |
| **Instrument Browser** | `0:/wavex/instruments/*.wxi` and `sfz/**/*.sfz`; preflight; load into the selected Track (asks, §1.3); tag filter | Sample Browser's `.sfz` handling, split out |
| **Bank** | the current `.wxb`: 128 slots, Load/Save Bank, recall slot → Track, store Track → slot, preload | nothing |
| **Sample Browser** | what is on the card; audition; **Load onto the selected Track** (§6.1) | exists; Load binds to the selected Track (stage 2, 2026-09-04); loses its `.sfz` duties |
| **Sample Manager** | the Pool: what is resident, who uses it, unload, assign to Track / to pad, **set current sample for editing** | exists; today cannot see an import's samples (§4); "Assign" with a replace confirm (stage 2, 2026-09-04); "to pad" waits for the Pad Map |
| **Sample Edit** | the **current sample**'s markers/gain/fades | exists |
| **Play** | the grid, addressed to the selected Track | exists; follows the selected Track (2026-09-04) |
| **Mixer** | 16 strips | `output-routing-and-mixer.md` stage 3, not built |

"Selected Track" and "current sample" are the two pieces of shared UI state; both are ESP32-side, both survive page navigation like `SampleBrowserState` does. The selected Track is always visible as the header chip (§2.4), so no page needs its own Track readout — the strip on Play that today says "Track 3: …" collapses into the chip.

### 6.1 Workflows — what "frictionless" means in taps

The survey's common trait: every groovebox loads a sample straight onto the current pad/track and it plays, creating the sound object implicitly. WaveX's equivalents, counted from the main menu:

- **A. Hear a card sample on the keys (4 taps).** Play → pick Track → Browse → **Load**. Load with a Track selected builds a **Quick Instrument** on the Daisy — Init + one Zone at root C4 on Osc 1, Env/Filter at defaults — and binds it to that Track (today: `MSG_SAMPLE_LOAD` + `MSG_SAMPLE_SELECT`, which already does exactly this via `SfzLoader::BindSample`). Keys play it immediately. If the Track is occupied, the §1.3 prompt names what it would replace. Audition remains the non-destructive preview.
- **B. Build a kit.** Track → Instrument → Osc → Mode: Drum → Pad Map → focus pad → Browse → Load lands on that pad (`INST_OP_SET_PAD_SAMPLE`); or Sample Manager → "To pad". Save → `instruments/`.
- **C. Load a soundfont.** Browse → `.sfz` → Load → asks which Track (built 2026-09-04), keeps its name (built). Two on two Tracks share the Pool (§4).
- **D. Bank.** Bank page → Load Bank; recall slot *n* → Track (prompt if occupied); tweak; Store back; Save Bank. From a DAW: Program Change *n*.
- **E. Sequence.** Patterns address Tracks (Phase 2); nothing here changes that.

### 6.2 Replacing is always confirmed

Per §1.3: Load (sample, `.wxi`, `.sfz`), Assign, and Bank recall from the UI show "Track *n* holds *X* — replace?" when *n* is occupied. There is no undo for a replaced Instrument (its Samples may be freed), which is why the prompt exists; "Store to Bank first" is offered on the same dialog when a Bank is loaded.

---

## 7. Protocol deltas (summary)

| Change | Kind |
|---|---|
| `MSG_TRACK_BINDING_REQ` / `MSG_TRACK_BINDING` (0x47/0x48) carrying state, sample id and the Instrument name | **built 2026-09-04** |
| `NoteMessage::channel` bit 7 = `NOTE_ADDR_TRACK` | additive; MIDI task never sets it, internal sources always do |
| `MSG_TRACK_OP {track, op, value}` — `SET_MIDI_IN`, `SET_POLY_LIMIT`, `SET_PRIORITY`, `SET_PROGRAM_CHANGE` | new message |
| `INST_OP_NEW`, `INST_OP_SAVE`, `INST_OP_SET_NAME`, `INST_OP_SET_PAD_SAMPLE`, `INST_OP_SET_ZONE`, `INST_OP_SET_OSC`, `INST_OP_SET_FILTER`, `INST_OP_SET_ENV`, `INST_OP_SET_LFO` | new ops on the existing `MSG_INST_OP`; `MSG_INST_ZONE_SYNC` (0x62, reserved) for editor readback |
| `MSG_INST_OP` load path accepts `.wxi` as well as `.sfz` | behaviour |
| `MSG_BANK_OP` / `MSG_BANK_STATUS` (§3.6) | new messages |
| `MSG_SAMPLE_META_PAGE_REQ` / `MSG_SAMPLE_META_PAGE` (0x49/0x4A): one window of the Pool per frame | **built 2026-09-05** |
| `MSG_SAMPLE_META` gains `used_by` bitmask (16 bits) and a pinned flag; PROTOCOL_VERSION 2 | **built 2026-09-05** |
| `MSG_SAMPLE_LOAD` failure carries a reason code (`SAMPLE_STATUS_LOAD_FAILED` + `SampleLoadFailReason`) | **built 2026-09-04** |
| Doc rename: slot → track in `inter-mcu-protocol.md` | docs |

`PROTOCOL_VERSION` does not need to move for any additive change here; `NOTE_ADDR_TRACK` is the one to think about, and it is backward-compatible (an old Daisy masks `& 0x0F` and behaves as today).

---

## 8. Stages (one verified commit each)

Listed by dependency. Stages 1–3 are done; the order in which the later stages are taken up is **not yet decided** (decision §9 item 13).

0. ~~**Bench findings, 2026-09-02/03**~~ — **done** (`2728662`, `33a1312`, `504406a`, `6f87ce6`): current sample for Sample Edit; SFZ subfolder fallback; shared selected Track with 1-based display; Instrument name on the wire; Sample Manager explains a refused Select; loading an Instrument asks which Track; boot autoload off.
1. ~~**Rename**~~ — **done 2026-09-04** in two commits: UI strings and docs (Slot → Track, Voice page → Instrument, "Patch"/"preset" → Instrument), then the identifiers (`kNumTracks`, `Tracks::Track()`, `Voice::track`, `TrackSteps`, `WAVEX_NUM_VOICES`, `UIInstrumentPage`, `SfzLoader::TrackLoaded/TrackName`, `VoiceManager::StopTrack/ReleaseTrack`; `NoteMessage::channel` documented as the Track address). Still saying "slot": the wire-struct field names in `protocol.h` and the local variables in `audio_engine.cpp`/`sfz_loader.cpp` that carry them — these go with the §7 protocol-doc rename, not before it.
2. ~~**Load-to-Track workflow**~~ (§6.1 A, §6.2) — **done 2026-09-04**: Browse "Load" of a sample goes straight onto an empty selected Track and binds it when the Daisy reports it resident; an occupied (or not-yet-reported) Track opens the Track picker with "Track *n* holds *X* — replace?"; a Track holding an imported Instrument is not a valid sample target until the Pool (stage 3) and the picker says so. Sample Manager "Select" is "Assign" and asks once before replacing a different sample. `MSG_SAMPLE_STATUS` gained `SAMPLE_STATUS_LOAD_FAILED` with a `SampleLoadFailReason`, so a failed load says why instead of timing out. "To pad" waits for the Pad Map (stage 4).
3. ~~**Sample Pool**~~ (§4) — **done 2026-09-05** (`1622b9b` and the commit after it): `WaveX::Audio::SampleRegistry` (shared, host-tested; ids name their slot with a generation, 2 KB SRAM index, records in a 192 KB SDRAM partition, `WAVEX_SAMPLE_POOL_CAPACITY` = 1024, admission fails rather than evicts); the engine's WAV list and the loader's private table both replaced by it; an import's samples are Pool entries (hits by path cost nothing), released per Track behind a per-track voice stop (`s_voice_stop_tracks`), so two imports share files and a sample replaces an import; `MSG_SAMPLE_META_PAGE_REQ/PAGE` page the Pool in one frame per window with `used_by`/pinned on each record; the Sample Manager pages and shows "used by". Verified on the bench by `tests/hil/test_sample_pool.py`. Not renamed: `SfzLoader` keeps its name (it still only reads `.sfz`; the `.wxi` reader of stage 4 is where `InstrumentLoader` earns the rename).
4. **Instrument file and editors** — `.wxi` reader/writer with the full §3.3 chunk set (osc2/env3/LFO chunks may be written empty until stage 5), `INST_OP_NEW/SAVE/SET_NAME/SET_PAD_SAMPLE/SET_ZONE`, Instrument-level defaults + zone overrides (§3.2, retiring `ZONE_FLAG_LIVE_FILTER_ENV`), Instrument Browser, Pad Map and Key Map pages, Track page. The Pad Map piece does not depend on stage 3 and may go first.
5. **Voice architecture** (§3.1) — typed `Oscillator` wrapper, Osc 2 + submix, `FilterType`, Env 3, two per-voice LFOs (global LFO 2 retired), new mod destinations, `output`/`poly_mode` on the Instrument, `INST_OP_SET_OSC/FILTER/ENV/LFO`, Instrument page tabs. **DWT-measured** with both oscillators at `WAVEX_NUM_VOICES` before the count is changed. Can be split per sub-item; each is host-testable in `VoiceManagerTest`.
6. **Bank** (§3.6) — `.wxb` reader/writer (nests the stage-4 Instrument chunks), `MSG_BANK_OP/STATUS`, Bank page, Program Change recall, "save with samples".
7. **Track model + MIDI routing** (§2) — `Track` struct, `midi_in`, `NOTE_ADDR_TRACK`, Daisy-side fan-out, `MSG_TRACK_OP`, remove the ESP32 global channel filter. Needed by the sequencer (Goal B) for `NOTE_ADDR_TRACK`; otherwise independent.
8. **Polyphony policy** (§5) — measure first; `poly_limit`, `priority`, track-aware steal.
9. **FX** — reserved chunk only; no design here.

Stage 4's pad-map piece and stage 7 are independent of 3 and can be done in any order. Stage 6 needs 4. Stage 5 needs nothing but changes the `.wxi` chunks it writes, so it should not trail stage 4 by long. Nothing in Goal B (`digital-voice-audition.md` stages 5–8) waits on any of this except that the sequencer addresses tracks with `NOTE_ADDR_TRACK` from stage 7 on.

---

## 9. Decisions

Recorded with the date each was taken. "Open" items need a yes/no before the stage that depends on them.

| # | Decision | Status |
|---|---|---|
| 1 | Names: Track / **Instrument** / Voice (engine-only) / Pattern / Song / Bank / Sample Pool (§1). "Patch" (2026-09-02) withdrawn. | **2026-09-03** |
| 2 | Routing on the Daisy (§2.2) rather than the ESP32. | accepted by default |
| 3 | Trim on the Instrument, fader on the Track (§3.1) — loading an Instrument never moves the mixer. | accepted by default |
| 4 | Selected Track shown as a header chip on every page and changed from anywhere, plus a Track page for MIDI-in/poly/mixer (§2.4). The alternative — a Track page only, other pages following it silently — was rejected: the bench finding was precisely that the user could not tell which Track a page was acting on. | **2026-09-04** |
| 5 | Sample Pool capacity **1024**, indexed not scanned, records in SDRAM, **fail with reason instead of evicting** (§4). | **2026-09-04** |
| 6 | Measure before choosing the voice count (§5); `WAVEX_NUM_VOICES` is the single constant, separate from the analog 8. | **2026-09-04** |
| 7 | Tempo on the Song, project-level default for pattern mode (§3.5). | accepted by default |
| 8 | One Instrument type, two editors (§3.2) — no separate Drum/Keygroup program types; `mode` picks Pad Map or Key Map; per-pad parameters are zone overrides. The alternative, two Instrument types with different files and pages as Akai does, was rejected: one type means a kit and a multisample are the same file, load the same way and can even be both (drum pads on Osc 1, a chromatic layer on Osc 2). | **2026-09-04** |
| 9 | Swing: Song default, Pattern override (§3.5). | accepted by default |
| 10 | Default pattern length 32 (§3.5). | accepted by default |
| 11 | Bank = 128 embedded Instruments in one `.wxb`, index-resident, Program-Change addressable (§3.6). | **2026-09-04** |
| 12 | Replacing an occupied Track is always confirmed from the UI (§1.3, §6.2); Program Change replaces without a dialog. | **2026-09-04** (PC exception to confirm) |
| 13 | Tracks 1–16 on screen, eight per page (§2.4). | **2026-09-04** |
| 14 | Voice page → Instrument page rename lands in stage 1. | **2026-09-04**, done |
| 15 | Two oscillators per Instrument; Osc 2 Sample-only for v1; Wavetable is a reserved `OscType` (§3.1). | **2026-09-04** |
| 16 | Two per-voice LFOs owned by the Instrument plus one engine-global LFO; global LFO 2 retired (§3.1). | **2026-09-04** |
| 17 | Env 1 hard-wired to amp; Env 2/3 routed through the matrix with shortcut knobs (§3.1). | **2026-09-04** |
| 18 | SVF as the v1 filter; `FilterType` reserves 8 values (§3.1). | **2026-09-04** |
| 19 | The voice architecture is designed now (§3.1, §3.3 chunks); **implementation ordering of stages 4–8 to be decided** (§8). | **2026-09-04** |

---

## 10. Deliberately out of scope

Streamed (disk) zones; FX design; Bank Select (multiple current Banks); per-zone editors beyond the Pad Map and Key Map; multi-output routing beyond the `output` field (Stage B); song/project files (they reference Instruments and the Bank by path and are `sequencer.md`'s). The wavetable renderer is deliberately deferred and unscheduled; only its `OscType` value, its mod destination and its chunk placement are established here.
