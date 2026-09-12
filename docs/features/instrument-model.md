# Sampler Instruments and Sample Ownership

**Status:** As-built sampler model and load path, reviewed 2026-09-06.
The Track/Instrument/Sample Pool core, SFZ import and WXI read path exist.
New-copy Instrument saves and the on-device Pad Map are built. Key Map editing
and Bank recall remain open.
The full two-oscillator target and persistence vocabulary are defined in
[track-and-patch-model.md](track-and-patch-model.md).

## Contents

- [1. Entities and ownership](#1-entities-and-ownership)
- [2. Sampler data model](#2-sampler-data-model)
- [3. Note resolution](#3-note-resolution)
- [4. Loading and lifetime](#4-loading-and-lifetime)
- [5. Persistence](#5-persistence)
- [6. Protocol](#6-protocol)
- [7. UI and remaining work](#7-ui-and-remaining-work)
- [8. Kits](#8-kits)
- [9. Validation](#9-validation)

## 1. Entities and ownership

| Entity | Identity and owner | Relationship |
|---|---|---|
| Track | Index in the Daisy `Tracks` table | Holds an Instrument, MIDI routing and performance settings |
| Instrument | Track-owned runtime sound | Ordered sampler Zones and instrument-scoped modulation slots |
| Zone | Index in an Instrument | Key/velocity range, sample reference and playback overrides |
| Sample Pool record | Resident sample id | Shared by Zones across Tracks; owns metadata and an allocator handle |
| Voice | Callback-owned playback slot | Copies complete trigger parameters and borrows immutable sample storage |
| Bank | Target file of numbered Instruments | Recall copies an Instrument into a Track; not another runtime voice owner |

All resident samples use the same Pool, whether loaded individually or through
an import. There is no longer a separate SFZ-private sample table.
The foreground owns registry mutations; the audio callback must not traverse
partially constructed records.

Track references are represented by the Pool's `used_by` mask. A manually
loaded sample can also be pinned, keeping it resident after its last Track
reference is released. A sample's data cannot be freed until every callback
reference, including queued triggers and sequencer maps, has been retired.

## 2. Sampler data model

The authoritative engine types are
[`instrument.hpp`](../../firmware/daisy/src/audio/instrument.hpp),
[`sample_pool.hpp`](../../firmware/daisy/src/audio/sample_pool.hpp), and
[`sample_registry.hpp`](../../firmware/shared/audio/sample_registry.hpp).
The current Instrument has a bounded Zone array. A Zone contains:

- A Pool sample id, inclusive key/velocity ranges and root note.
- Coarse/fine tuning, gain/pan, region and loop overrides.
- Filter/envelope values, choke group and playback flags.

A zero region/loop override inherits the resolved sample record where the
resolver specifies it. Do not duplicate sentinel handling in a UI or a second
resolver. Source positions are frames at the source sample rate.

The current engine is sampler-based. The target Instrument's two typed
oscillators, additional envelopes/LFOs and Bank format are described in the
Track/Instrument model; the presence of corresponding WXI fields does not
mean the audio renderer consumes every field.

## 3. Note resolution

The main-loop note handler resolves the addressed Track through
`SfzLoader::ResolveNote()` and the Instrument's bounded Zone scan:

1. Match key and velocity ranges, respecting the fixed layer-trigger limit.
2. Resolve each sample id through the common Pool into resident PCM and
   metadata.
3. Compose root-note tuning, source/output rate, zone gain and playback
   overrides into complete `VoiceTriggerParams`.
4. Queue those values for callback-owned `VoiceManager::Trigger()`.

Drum mode disables note-relative pitch tracking. Layering and velocity
crossfade are bounded by the model's trigger capacity. Choke groups use
a short release; note-off releases the addressed Track/key except one-shot
voices. Root note comes from the Zone rather than a hard-coded C4 fallback.

The note wire byte distinguishes explicit Track addressing from incoming MIDI
channel addressing. MIDI channels can fan out to matching Tracks according to
their configured `midi_in`; they are not unconditional Track indices.
See [inter-mcu-protocol.md](inter-mcu-protocol.md) for that contract.

Sequencer preview triggers use foreground-resolved snapshots instead of
reading the mutable Instrument table in the callback; see
[sequencer.md](sequencer.md).

## 4. Loading and lifetime

`SfzLoader` is a cooperative main-loop state machine. It parses/probes
references, checks memory admission, retires the replaced Track's voices,
loads aligned SD chunks, and commits the new Instrument.
[SFZ import](sfz-import.md) owns syntax/opcode support and file conventions.

A failed preflight preserves the prior Track. Once destructive replacement has
begun, later I/O failure is reported as failure; retaining the old Instrument
through every late failure would require a separate transactional ownership
model and admission policy.

Samples referenced by other Tracks remain resident. Rebinding a Track to a
sample from its own import transfers that sample's reference before discarding
the import's other unreferenced samples. Releasing a Track must not free the
sample selected for its replacement.

Resident format and memory admission limits come from the shared policy and
`hardware_config.h`; the current resident renderer accepts PCM16 mono/stereo.
Long concurrent streamed voices remain roadmap work. The singleton streaming
audition is a separate path and does not imply a resident Pool entry.

## 5. Persistence

The shared [WXCF container](../../firmware/shared/wxcf/wxcf.hpp) supplies
little-endian header/chunk I/O. The
[WXI codec](../../firmware/shared/wxi/wxi.hpp) defines the instrument chunk
layout; [track-and-patch-model.md §3.3](track-and-patch-model.md#33-persistence-wxi-over-wxcf)
explains its typed oscillator structure.

WXI encoding/decoding, loader ingestion, new drum Instruments, naming and
new-copy saves exist. The Pad Map saves to 0:/wavex/instruments/<name>.wxi
through a request-specific temporary file. It checks every write and close,
then renames the closed file. Existing destinations are refused; there is no
unlink-and-replace window. Failed writes/close/rename never replace a saved copy.
Power-loss recovery remains a hardware gate; FAT directory updates alone
are not a proof of crash consistency.

Save snapshots the Instrument into the existing document mapper in the main
loop. Stored zones use card paths, preserve sparse pad indices, and include
the current Instrument filter, amp envelope, modulation slots and zone
overrides. Unsupported future oscillator/envelope fields normalize to current
defaults in the new copy; the original file stays untouched. Sample PCM and
Sample Pool marker edits are separate from this Instrument save.
Streaming audition stops before a save takes the shared SD path; resident
voices continue. Bank and Project serializers remain open.

## 6. Protocol

Use the current catalog in [inter-mcu-protocol.md](inter-mcu-protocol.md)
and the definitions in `protocol.h`.
`INST_OP_SFZ_PROBE`, `INST_OP_SFZ_LOAD` and
`INST_OP_SET_MOD_SLOT` are implemented. The loader also recognizes WXI input;
the historical SFZ op names do not restrict its accepted extension.

Kit creation, naming, pad assignment/clear/choke and new-copy saves use
the shared Instrument operations and sixteen-pad synchronization snapshot.
Per-pad sound edits use the dedicated operation/readback contract in the
[protocol catalog](inter-mcu-protocol.md#per-pad-sound-editing-additive-to-protocol-6).
General keyboard-zone editing and Bank recall remain target work; new
operations require shared definitions and round-trip tests before consumers.

## 7. UI and remaining work

Instrument's Sample tab opens Pad Map. Its sixteen pads select and audition
notes 60-75 on the selected Track. Assign opens a paged resident-sample picker;
load additional samples or saved WXI files through Sample > Browse.
Choke controls edit the selected pad. Shift exposes Rename, Clear pad,
Sound and Track navigation. New kit confirms replacement before showing
the name keyboard. Save copy requires a new filename.

Sound edits cutoff and amp attack/decay/sustain for the selected populated pad.
The first edit copies the Instrument defaults into the zone, then changes
the selected field. Inherit clears that shared filter/envelope override.
Edits affect subsequent hits; sounding overridden voices keep their copied
cutoff and amp envelope when Instrument controls move. Resonance remains
Instrument-owned. One-shot pads ignore note-off, so release is not exposed.
Save copy preserves the overrides through the existing WXI fields.

Key Map, dedicated Instrument Browser, Bank/Track pages, and expanded
oscillator/envelope/LFO editors remain target work.

## 8. Kits

A kit is a drum-mode Instrument. It has the same Track ownership, shared
Sample Pool references, note resolver and WXI persistence as a keyboard
Instrument. Pad assignment and replacement retire only the edited Track's
voices and prepared sequencer triggers before dropping references. A sample
used by another pad or Track remains resident. Choke groups are Track-local:
two kits can both use group 1 without choking each other.

Each sequencer step selects a MIDI note and resolves the matching Track's
prepared zones at that note and velocity. Notes 60-75 address the sixteen
kit pads; sparse pads remain silent. Pad Map and Play can audition the pads
independently.

## 9. Validation

Host tests cover Instrument resolution, tuning/layer bounds, rendering,
format codecs and cooperative loader ownership/failure transitions.
Packet dispatch tests prove routing separately from real loader tests.

Selected two-board HIL covers sparse kit creation, pad assignment and sound
editing, WXI save/reload, inheritance reset, step-note selection and preservation
of another Track's resident voice. Callback workload measurements and saved-file
restart checks are recorded in
[callback-performance-log.md](../callback-performance-log.md).

Bench verification must still establish audible SFZ root notes and loops,
layered release/choke behavior, concurrent Track imports, memory refusal,
MIDI routing and arbitrary power-loss recovery.
See [testing_guide.md](../testing_guide.md) and the roadmap's hardware gates.

## Related

- [Track/Instrument target](track-and-patch-model.md)
- [Oscillator source boundary](oscillator-sources.md)
- [Modulation](param-locks-and-modulation.md)
- [Architecture](../architecture.md)


## Expanded Instrument storage foundation (2026-09-12)

The engine model and WXI mapper now retain two typed oscillator maps, three
envelopes, two LFO parameter sets, trim/tuning, oscillator mix, filter tracking
and modulation amount, velocity curve, tags, output and poly mode. A cutoff
edit followed by Save preserves these stored settings. The loader plans up
to 64 zone references and deduplicates paths across both maps. Removing a zone
releases a sample only when neither oscillator still uses it.

This milestone establishes storage and ownership. The active renderer and
existing Key Map/Pad Map controls still address Oscillator 1; Oscillator 2
rendering, additional modulators and their controls are the next stage.
Reserved wavetable sources remain unimplemented. Output/poly mode are retained
settings, not a claim that their routing/voice policy is active.

Normal firmware stores the foreground Track records in cacheable D2 RAM,
constructed explicitly after System initialization. DMA buffers remain in
their existing regions. The SRAM-debug layout places those records in its
spare AXI range and moves document scratch within its guarded budgets.
Both layouts compile; the host tests cover field preservation, cross-map
sample deduplication and sample retention when Oscillator 1 is cleared.
