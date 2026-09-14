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

Instrument sound controls on Osc, Env, Mod, Filter and Amp preview
automatically. The backend keeps one per-Track undo point for filter cutoff /
resonance and Instrument amp trim gain / pan; it does not snapshot PCM
references, key maps, zones or names. The first edit establishes the baseline,
and the shared Apply/Revert actions commit or restore that sound baseline.
Successful WXI saves commit the audible working values; a failed save keeps the
undo point. Changing pages or Tracks retains it, while replacing the
Instrument clears it. Amp edits address Instrument trim gain and pan rather
than the master volume.

The Instrument Filter editor selects one of four per-voice modes: low-pass,
high-pass, band-pass or notch, and which topology renders it: the WaveX
state-variable filter (`SVF`) or the zero-delay-feedback four-pole ladder
(`Ladder`). Both are Instrument-owned, travel with the Instrument between
Tracks and Banks, persist in WXI and are restored by Revert; zones never
override either. Cutoff and resonance retain their existing float ranges. The
WaveX state-variable path returns LP, HP, BP or Notch from the shared filter
state; the ladder renders LP, HP and BP as its weighted stage sums at the
slope and Notch as input minus its 12 dB band-pass tap, an approximation
that nulls at the cutoff. Under the one RES control the ladder
self-oscillates from about 74% and the SVF, which cannot, rises to Q 16;
both sit near +13 to +15 dB at 70%. Slope (12 or 24 dB) and drive are
Instrument-owned as well: on the SVF the slope adds a second TPT stage and
drive raises and soft-saturates the input with the same gain law as the
ladders while soft-limiting the resonance path; on the ladders the slope
picks the 12 or 24 dB tap and drive pushes their input stage. On every
topology drive 0 is the clean filter and more drive is louder and dirtier. All four filter settings preview
on held notes, undo with Revert and persist in WXI. At zero
cutoff, high-pass and notch pass the input while low-pass and band-pass are
silent. At or above Nyquist, low-pass and notch pass while high-pass and
band-pass are silent, on both topologies. Changing mode preserves the
first-stage state and clears only the second stage whose input changed;
changing topology retunes the incoming filter and starts it from cleared
state; source cursors are unaffected either way.

The current handoff updates sounding voices for Instrument gain/pan, oscillator
level/mix/coarse/fine/key tracking, Env 2/3 and LFO settings while preserving
each voice's source cursor, loop/sample references, zone gain/tuning, envelope
phase and LFO phase/note age. Gate/free admission policy applies to the next
note. Map/sample assignment and Instrument replacement still use their
voice-stop/next-note boundaries.

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

This milestone establishes storage and ownership. The existing Key Map/Pad Map controls still address Oscillator 1.
The following renderer milestone adds Oscillator 2; additional modulators
and touch controls remain open.
Reserved wavetable sources remain unimplemented. Output/poly mode are retained
settings, not a claim that their routing/voice policy is active.

Normal firmware stores the foreground Track records in cacheable D2 RAM,
constructed explicitly after System initialization. DMA buffers remain in
their existing regions. The SRAM-debug layout places those records in its
spare AXI range and moves document scratch within its guarded budgets.
Both layouts compile; the host tests cover field preservation, cross-map
sample deduplication and sample retention when Oscillator 1 is cleared.

## Two sample sources per voice

Each oscillator resolves its own key/velocity map. The nth valid match from
each map forms one voice, in stable zone order; each zone is used once and
the existing four-layer limit remains. A missing partner is silent.
The Oscillator 1 match owns shared filter, amp, pan, choke and note-off
overrides; Oscillator 2 supplies them when no Oscillator 1 match exists.
Oscillator levels and the linear mix apply before the shared filter.
Instrument trim gain and tuning compose with each zone's settings.

Each source keeps its own native-rate compensation, tuning, region, loop
and fade. With two sources, region fades apply before the submix.
A shorter source becomes silent while its partner plays; reaching both
source ends releases the shared envelope. MIDI note-off and Track stop
retain their shared-voice behavior. Pitch and normalized position locks
affect both sources using each source's own region and tuning.

The output uses one mono submix and a shared Instrument/zone pan.
Stored oscillator pan fields remain reserved; independent stereo oscillator
panning is not exposed. The transport can read/edit oscillator level, mix,
coarse/fine tune and key tracking, and copy a map into an empty oscillator.
Those edits update sounding voices through the bounded prepared handoff while
preserving source cursors and zone-specific gain/tuning. Instrument > Osc now
exposes both source selectors, level, submix, coarse/fine tuning and key
tracking with authoritative readback. Drag or step values, then use Shift >
Apply or Revert. Copy Other
clones the other source into an empty map. Opening Instrument only reads the
selected Track and never binds the Browser's last loaded sample.

The editor preserves untouched floating-point settings exactly and accepts the
complete saved WXI level/tuning ranges. A changed Instrument revision discards
a stale draft; pending operations are polled for their retained result. A
timeout refreshes the authoritative state without blindly retrying a mutation.
Both maps save through existing WXI copies. Key Map opens the oscillator
selected in Osc and edits its 32 keyboard zones independently; its header
names the oscillator. Assignment, clear and key/velocity ranges share one
Track revision across both maps. The voice-stop barrier protects replacement,
and clearing a zone retains samples used elsewhere in either map.
Pad Map still edits the first oscillator's fixed drum pads. Envelope/matrix
editing is described below. The backend now owns two per-voice LFO parameter
sets on the Instrument, and the LFO touch page plus four-mode filter editor
are built. Additional modulation destinations remain open.

The oscillator UI milestone historically passed 258 ESP32 host tests, 379
shared tests and both device builds. Two-board tests verify staged apply/revert,
copying, Track-entry preservation and WXI recall. The historical pre-LFO,
pre-filter-mode two-source callback gate is recorded at 65.6921% peak over
605.2 seconds with zero underruns in
[the performance log](../callback-performance-log.md). These checks do not
close the one-hour phase soak or physical-panel gate.

Second-map Key Map validation covers both protocol operations, four UI-model
tests and 26 loader tests. The two-board test assigns separate samples and
key ranges to both maps, saves/reloads one WXI, reads both maps back, and
checks note admission at every range edge (26.41 seconds). Both device
images compile. This foreground editor patch does not change the measured
voice renderer.

## Three runtime envelopes and matrix editor

Env 1 remains the per-sample amp envelope. Env 2 and Env 3 are independently
configured from the Instrument and advance once per block over the voice's
active frames, including its sequencer trigger offset. All three are matrix
sources: the original Env 2 id is unchanged, with Env 1/3 appended. Env 1
is sampled without a second advance. Note-off and choke release the auxiliary
envelopes; they cannot keep a finished sample voice alive.

The revisioned modulator messages edit one envelope or matrix slot and return
all three envelopes/eight slots. The existing WXI mapper preserves them.
Env 1 edits also use the existing live Track handoff, respecting pad overrides
and parameter locks; Env 2/3 settings use the bounded held-note handoff while
preserving envelope phase and note age.
Legacy amp and matrix edits invalidate stale touch drafts. Env 2/3 routing
uses the matrix, so no second implicit modulation path is added.

The Env and Mod touch editors now use those same revisioned snapshots.
Env selects one of three envelopes and edits attack/decay/release in milliseconds
and sustain in percent. Mod selects one of eight slots and edits source,
destination, signed depth, curve and the optional unipolar-to-bipolar remap.
Edits preview automatically in the backend working Instrument. Apply accepts
the common undo baseline, Revert restores the common snapshot, and a changed
backend revision discards only a stale delivery. Tab changes and Track changes
wait only for an outstanding delivery; they do not require Apply/Revert. A lost
acknowledgement causes readback without a blind mutation retry. Unknown saved
matrix fields
remain readable and can be explicitly cleared; unavailable sources are not
presented as working choices. Envelope and matrix edits save with WXI copies
from Pad Map. Additional modulation destinations remain open.
The historical clean db6f180 envelope gate measured 66.9571% peak over 605.2
seconds, with eight voices, both maps, 64 live routes, four locks per hit,
full filter and drive, SD streaming, grid traffic and six file cycles, with
zero underruns. Capture/image attribution is in
[the performance log](../callback-performance-log.md).

The runtime-envelope milestone passes the Daisy host suite plus the prepared
envelope handoff regression, normal and SRAM debug builds. A device test
checks revision rejection, duplicate delivery, Env 3/matrix edits and WXI
save/reload (1.17 seconds). That verifies control/persistence behavior, not
the pending capacity gate or an audio listening test.


The touch milestone passes all 264 ESP32 host tests and the ESP32 device
build. A two-board test (12.07 seconds) verifies Env 3 and slot 8 draft
isolation, Apply/Revert, guarded tab navigation, clear/revert, WXI save/reload
and independent navigation/modulation-depth console fields. Device captures
are `logs/envelope-editor-20260912.png` and
`logs/modulation-editor-20260912.png`. These checks do not establish audible
modulation quality, physical controls or the one-hour phase soak.

## Two per-voice LFOs (backend)

Each Instrument now retains two per-voice LFO settings. The backend evaluates
the LFOs with a Q32 free-running frame/beat epoch and supports waveform, Hz or
tempo-division rate, sync, retrigger, pitch-follow, delay and fade. Hz rates
are clamped to 0.02–20 Hz; tempo divisions cover 1/16 through 4 bars, and
pitch-follow applies only in Hz mode. The first per-voice source remains the
existing source id; the second is appended, while the retired global source id
continues to read zero for wire compatibility.

Typed revisioned GET/SET snapshots carry these settings and WXI retention
accepts legacy 15-byte LFO records by defaulting the missing pitch-follow field
to zero; new records use 16 bytes. The ESP32 LFO page presents eight tiles in
two rows of four for the selector and seven settings, and shares the common
sound Apply/Revert transport. Edits preview automatically, and a successful
WXI save persists the audible working copy. Held LFO, oscillator, gain and Env
2/3 voices retain their phase and continue through held-note edits; map/sample
assignment and Instrument replacement remain the explicit stop/next-note
boundaries.
The frontend suite passes 274 tests, and the two-board LFO HIL verifies the
transport, automatic preview and WXI readback (11.70 seconds). The inspected
1280×720 capture is `logs/instrument-lfo-20260912.png`; all eight tiles fit
without clipping.
