# SFZ Import — Loading Third-Party Multisamples into the Instrument Model

**Status**: V1 browser/import path code-complete and host/compile-tested; hardware audition remains outstanding. The browser lists `.sfz`, preflights references and memory through `MSG_INST_OP`/`MSG_INST_STATUS`, and starts a cooperative runtime load. WXCF `.wxi` persistence and editable zone sync remain deferred.
**Lineage**: SFZ (Cakewalk/ARIA/sfizz format) is the most common free-format multisample distribution format — the goal is "drop a folder of `.sfz` + `.wav` files on the SD card, get a playable instrument," not to build a general-purpose SFZ player.
**Dependencies**: `firmware/daisy/src/audio/instrument.hpp` (Zone/Instrument/ResolveNoteOn — read this file and `docs/features/instrument-model.md` §1-3 first), `VoiceManager::Trigger` (`firmware/daisy/src/audio/voice_manager.hpp`), the existing WAV-loading/`SampleMemMgr` path used by the Phase-1 stopgap in `audio_engine.cpp` (search `OnSampleLoad`, `SampleMemMgr::alloc`).
**Consumers**: the ESP32 sample browser and Daisy note routing. The importer still produces the same `Instrument`/`Zone` data that the future `.wxi` path will consume.

---

## 1. Goal and scope

**Goal**: given a selected `.sfz` file and its referenced `.wav` samples on SD, preflight the complete resident footprint, then produce a populated `Instrument` bound into a Track (`Tracks`) and playable via the existing note-on/off path.

**Explicit non-goals for v1** (do not implement, do not gold-plate):
- Full SFZ opcode coverage. Real-world `.sfz` files use hundreds of opcodes (round-robin `seq_*`, `sw_*` keyswitches, per-region LFOs/EQ, `curve_*` tables, effects buses). **Unsupported opcodes must be silently skipped**, not rejected — a file that partially maps is more useful than one that fails to load.
- Auditioning an `.sfz` from the browser. Audition remains WAV-only; the SFZ Audition softkey is visibly disabled.
- Editable-zone synchronization (`MSG_INST_ZONE_SYNC`). Probe/load status uses 0x60/0x61; zone editing remains a follow-up.
- `.wxi` persistence. The imported `Instrument` lives in RAM for the session; re-parsing the `.sfz` on next boot is fine for v1.
- Streamed (non-resident) samples, `<curve>`/`<effect>` headers, per-region filters beyond `cutoff`.

---

## 2. Where this fits relative to `instrument-model.md`

That doc defines the target data model and a `.wxi`-based load path (its stage 4-6). This feature is a **second front door into the same model**: an SFZ parser + opcode mapper that fills an `Instrument`'s `Zone[]` array directly, plus a minimal version of stage 4's "sample table populated from loaded WAVs" — scoped down to what a single instrument load needs (no multi-slot refcounted eviction; see §6).

Read `docs/features/instrument-model.md` §1-3 and `firmware/daisy/src/audio/instrument.hpp` before starting. The `Zone` struct (instrument.hpp:46-64) is the target; do not modify its shape unless a mapping genuinely requires a new field (if so, it belongs in that doc too — flag it, don't silently diverge).

---

## 3. File layout and discovery

V1 convention (selected and implemented):

```
0:/wavex/sfz/<instrument-name>/<instrument-name>.sfz
0:/wavex/sfz/<instrument-name>/*.wav
```

- `sample=` paths inside the `.sfz` are relative to the `.sfz` file's own directory, unless a `<global>` or `<control>` header sets `default_path=` (also relative to the `.sfz` file). Resolve both.
- `.sfz` and `.wav` files are both listed by the browser (case-insensitive extension matching). Selecting an SFZ sends an asynchronous probe for slot 0. The optional fixed boot load at `WAVEX_DAISY_SFZ_BOOT_PATH` remains available for bench/debug use.
- The info pane reports unique referenced WAV count, zone count, total resident audio bytes, and reserve-adjusted available memory. Missing or unsupported references and an over-budget total are warnings and disable Load.

---

## 4. SFZ parser

Line-oriented, not a real grammar — SFZ has no nesting beyond header→opcodes.

**Must handle:**
- Headers: `<region>`, `<group>`, `<global>` (ignore `<control>`, `<master>`, `<curve>`, `<effect>` — parse past them without erroring, but don't act on their opcodes beyond `default_path` in `<control>`/`<global>`).
- `opcode=value` pairs, whitespace- or newline-separated, multiple per line.
- `//` line comments (rest of line).
- Quoted values are **not** standard SFZ (values run to next whitespace), but sample paths may contain spaces — SFZ handles this by treating everything up to the next recognized `opcode=` token as part of the value. A pragmatic approach: since `sample=` is almost always the last opcode on its line in real-world files, special-case it to consume the rest of the line. Document this as a known limitation rather than writing a full tokenizer with opcode-name lookahead.
- `#define $VAR value` macros (very common — many libraries parameterize velocity layers this way) with `$VAR` substitution in later opcode values. Simple text substitution before/during tokenizing is sufficient; no recursive macros needed.
- Cascading: opcodes set in `<global>` apply to all groups/regions; `<group>` opcodes apply to all regions in that group; `<region>` opcodes override both. Implement as: carry a "current defaults" struct, reset group-level defaults at each `<group>`, snapshot into a fresh `Zone` at each `<region>`, apply region-level opcodes on top, then commit the `Zone` when the next header or EOF is hit.
- Unknown opcodes: parse the key=value pair (so parsing doesn't desync) and drop it. Do not error, do not warn per-opcode (a debug-level log line total count is enough — see `docs/logging.md` for the per-module log macros).

**Where it runs**: main loop only (file I/O), never the audio callback. This is not real-time code — treat it like the existing SD/WAV loading path, not like `VoiceManager::Render`.

**Testing**: host-tested with fixture `.sfz` text (string literals or small fixture files under `firmware/daisy/tests/`), following the existing pattern in `instrument_test.cpp`. No SD card or hardware needed to test the parser and opcode mapper — only the actual file read and WAV decode touch FatFs.

---

## 5. Opcode → Zone mapping

| Zone field | SFZ opcode(s) | Conversion / notes |
|---|---|---|
| `sample_id` | `sample=` | Path resolved per §3; loaded via §6, id assigned after load |
| `key_lo`, `key_hi` | `lokey=`, `hikey=`, or `key=` (sets both) | Accept numeric (0-127) or note-name (`c4`, `f#3`) — see note-name convention below |
| `vel_lo`, `vel_hi` | `lovel=`, `hivel=` | Default 1/127 if absent, matching `Zone`'s defaults |
| `root_note` | `pitch_keycenter=`, or `key=` if `pitch_keycenter` absent | Falls back to `lokey` per SFZ spec if neither is given |
| `coarse_tune` | `transpose=` (semitones) | Direct |
| `fine_tune` | `tune=` (cents, -100..100) | Direct |
| `gain` | `volume=` (dB) | `gain = powf(10.0f, db / 20.0f)` |
| `pan` | `pan=` (-100..100) | `pan = (sfz_pan + 100.0f) / 200.0f` → 0..1 |
| `start_frame` | `offset=` (frames) | Direct |
| `end_frame` | `end=` (frames) | Direct; `end=-1` in SFZ means "disabled region" — treat as skip-this-region, not zero |
| `loop_start`, `loop_end` | `loop_start=`, `loop_end=` | The current shared WAV parser does not read `smpl` chunks, so absent SFZ loop points remain absent in v1; embedded-loop fallback is follow-up work rather than assumed behavior. |
| `loop_mode` | `loop_mode=` (`no_loop`, `one_shot`, `loop_continuous`, `loop_sustain`) | Map `loop_continuous`/`loop_sustain` → `1`, else `0`. `one_shot` should also set `ZoneFlags::ZONE_FLAG_ONE_SHOT` |
| `choke_group` | `group=` + `off_by=` | SFZ's polyphonic mute: a region with `group=N` is choked by any other region firing with `off_by=N`. This is inverted from `Zone::choke_group` (which chokes same-group on trigger) — see §5.1 |
| `cutoff_hz` | `cutoff=` (Hz) | Direct; if absent, leave `Zone`'s default (20000.0f, i.e. no filtering) |
| `attack_s`,`decay_s`,`sustain`,`release_s` | `ampeg_attack=`, `ampeg_decay=`, `ampeg_sustain=` (0-100), `ampeg_release=` | `sustain = ampeg_sustain / 100.0f`; times pass through in seconds |

### 5.1 `group`/`off_by` vs `choke_group`

SFZ's model is "region A has `group=1`; region B has `off_by=1`" — B firing chokes anything currently sounding with `group=1`. `Zone::choke_group` (per `instrument-model.md` §3 step 4 / `Voice::Choke`) is symmetric — same nonzero group chokes itself. For v1, treat SFZ `group=N` as `choke_group=N` directly and **ignore `off_by`** unless it names the same group (the common case, e.g. hihat open/closed both using `group=1`/`off_by=1`) — that collapses to the existing symmetric behavior for free. Document any SFZ file using asymmetric `off_by` groups as unsupported for now.

### 5.2 Note-name convention

Standard SFZ (and this implementation) uses **c4 = 60** (`pitch_keycenter=c4` means MIDI note 60). Some other tools/libraries use `c3 = 60`. This is the single most common real-world bug source (an instrument sounding an octave off) — write the note-name parser once, unit-test it against the c4=60 table explicitly, and if a bench-tested `.sfz` sounds an octave wrong, that's the first thing to check before assuming a bug elsewhere.

---

## 6. Sample loading and the (minimal) sample table

Reuse, don't reinvent: the existing WAV-loading path (`SampleMemMgr::alloc` + chunked SD read from the main loop, currently invoked from the Phase-1 stopgap in `audio_engine.cpp`) already does the hard part. What's missing is the layer above it that a *set* of zones needs:

1. **Path collection**: walk the parsed regions, resolve each `sample=` to an absolute path, deduplicate (multiple regions commonly reference the same file at different pitches — do not load it twice).
2. **Load + assign**: for each unique path, load through the existing mechanism, get back whatever handle/pointer the current loader produces, mint a `sample_id`, and record `{sample_id → SampleRef}` in a small table that backs a `SampleResolver` (per `instrument.hpp`'s `SampleResolver::resolve` callback — this table is the `ctx`, and `resolve()` is a lookup into it).
3. **Assign `zone.sample_id`** to the id from step 2 for every region that referenced that path.
4. **RAM guard**: sum the loaded bytes; if the instrument's total exceeds a reserve-guarded budget (mirror `instrument-model.md` §4's `WAVEX_INST_LOAD_RESERVE_BYTES` default 8 MB, and its 4 MB per-sample cap), **fail the whole load** with a clear log message rather than partially loading — a half-loaded instrument with silent zones is worse than a load error.
5. **Zone cap**: `kMaxZones = 32` (instrument.hpp:31). If the `.sfz` defines more than 32 regions, **reject the load with a count in the error message** (`"N regions, max 32"`) rather than silently truncating — silent truncation produces a working-but-wrong instrument (missing key ranges) that's hard to diagnose later. A future pass could merge adjacent same-sample regions to fit larger instruments; out of scope for v1.

This remains narrower than `instrument-model.md` §4's full design: v1 owns one resident SFZ bank and replaces it as a unit. Runtime inspection/loading is asynchronous and cooperative. Parsing advances one line per main-loop pass, probing advances one WAV per pass, and loading reads one aligned chunk per pass. The UI receives both aggregate loaded/total bytes and current-WAV loaded/size bytes, displayed as two progress bars.

---

## 7. Wiring into note-on/off

Bind the resulting `Instrument` into a Track (`instrument.hpp`'s `Tracks::Track(n)`), then on note-on call `tracks.ResolveNote(track, note, velocity, resolver, out, kMaxLayerTriggers)` and feed each returned `VoiceTriggerParams` to `VoiceManager::Trigger()` — this path is already built and host-tested (`ResolveNoteOn`, `instrument_test.cpp`). On note-off, release voices for `(note, track)` per `instrument-model.md` §3 step 5 (`Voice::track`; `VoiceManager::ReleaseTrack()` is the note-off path).

This can be **additive** alongside the existing Phase-1 stopgap note routing in `audio_engine.cpp::OnNoteOn` — gate it behind whether the target slot has an SFZ-loaded instrument bound, rather than ripping out the stopgap as part of this feature. Deleting the stopgap entirely is `instrument-model.md` stage 3's job and has its own dependencies (protocol, UI) that shouldn't block this feature landing.

---

## 8. Real-time safety review

| Path | Context | Notes |
|---|---|---|
| `.sfz` parse + opcode mapping | main loop, cooperative | Fixed-capacity parser; one line per pump; no heap allocation |
| WAV probe/load per unique sample | main loop, cooperative | One probe or aligned read chunk per pump; reuses `SampleMemMgr` and the shared WAV parser |
| Zone/sample-table publication | main loop | Old voices are stopped and acknowledged by the callback before their sample memory is released; the new table publishes after every read succeeds |
| `ResolveNoteOn` | main-loop note handler | Bounded 32-zone scan; copies complete trigger snapshots into the existing SPSC queue |
| `VoiceManager::Trigger` / slot release | audio callback | Fixed-size voice operations only; no allocation, I/O, logging, or blocking |

No parsing, mapping, filesystem work, allocation, or instrument mutation runs in the audio callback. The only callback additions are fixed-size trigger/release semantics (`one_shot`, original trigger key, and slot-scoped layered release).

---

## 9. Testing strategy

- **Parser + opcode mapper**: host-tested, no hardware/SD needed. Fixture `.sfz` text covering: basic region, `<group>` cascading, `#define` macros, key-name vs numeric keys, missing opcodes (verify `Zone` defaults survive), a region exceeding valid ranges, an unsupported opcode mixed into a valid line (verify it's skipped, not fatal), a 33rd region (verify rejection with count in the message).
- **Note-name parsing**: explicit unit test table proving `c4` → 60, `c#4`/`db4` → 61, etc. — this is the highest-value single test given §5.2.
- **Sample table / dedup**: host-tested with a mock resolver (same pattern `instrument_test.cpp` uses for `SampleResolver`), verifying two regions referencing the same path get the same `sample_id` and the loader is invoked once.
- **End-to-end playback**: requires hardware audition (SD card, real `.sfz` + `.wav` set) — note in the PR/commit which physical instrument was used to verify, per this repo's existing bench-verification convention (see `docs/backlog.md` and recent commit messages for the pattern).

---

## 10. Implementation stages (one verified commit each, per repo convention)

1. ~~**SFZ parser**, host-tested: headers, opcodes, comments, `#define` macros, cascading defaults. Produces an intermediate per-region opcode map.~~ Done.
2. ~~**Opcode → Zone mapper**, host-tested: the table in §5, explicit note-name tests, unit conversions, unsupported-opcode skip behavior, >32-region rejection.~~ Done.
3. ~~**Sample table / loader glue**, host-tested: path resolution + dedup, resolver table, RAM reserve and per-sample cap.~~ Done; target-side FatFs/SDRAM integration is compile-verified, not hardware-verified.
4. ~~**Wiring**: fixed boot path, slot-0 bind, additive note-on/off routing, layered slot release and one-shot behavior.~~ Code-complete and compile-verified.
5. ~~**Browser/runtime loading**: `.sfz` listing, disabled Audition, reference/memory preflight, warning-gated Load, and total/current-WAV progress.~~ Code-complete; protocol round trips are host-tested and both firmware targets compile.
6. **Bench verification remains**: select and load a real third-party `.sfz` multisample, confirm both progress bars, warning states, key-range switching, tuning, loop behavior and one-shot release on hardware, and record the instrument/opcode findings in `docs/backlog.md`.

---

## 11. Open questions / follow-ups (not blocking, don't gold-plate now)

- Merging same-sample adjacent regions to fit >32-region instruments (piano-style libraries) — revisit only if a concrete instrument needs it.
- `off_by` groups that don't collapse to symmetric `choke_group` (§5.1) — note as unsupported until a real file needs it.
- Multi-slot refcounted instruments and editable zone UI remain with `instrument-model.md` stages 4-7; this browser slice owns one replaceable resident bank.
- `.wxi` persistence of an SFZ-imported instrument (skip re-parsing on every boot) — trivial once `instrument-model.md` §5's WXCF writer exists; not worth building early.
