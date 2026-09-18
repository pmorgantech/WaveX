# Sequencer and Digital Voice Playback

**Status:** The scheduler, transport, command queue, callback trigger path and
playhead publication are implemented. Each of the 16 pattern rows addresses
the matching Track's Instrument at the step's selected MIDI note. Velocity
layers and crossfades use the step's velocity. Chords and melodic gate lanes
remain future Phase 2.5 work.
Voice-scoped parameter locks and their touch editor are implemented.
Physical panel integration, MIDI clock hardware validation and
song/project persistence remain open Phase 2 work in [roadmap.md](../roadmap.md).
Host tests and device compilation do not establish audible timing or the
hardware phase gate.

## Contents

- [1. Ownership and execution](#1-ownership-and-execution)
- [2. Clocking](#2-clocking)
- [3. Pattern and voice models](#3-pattern-and-voice-models)
- [4. Edits and protocol](#4-edits-and-protocol)
- [5. UI surfaces](#5-ui-surfaces)
- [6. Validation](#6-validation)

## 1. Ownership and execution

The Daisy audio callback owns `SequencerTransport` and advances it through
`drain_sequencer()` in `audio_engine.cpp`. The main loop decodes transport,
pattern and clock commands into a fixed SPSC queue. The callback drains the
queue before ticking, so foreground edits do not mutate a pattern being read
by the scheduler.

The main loop prepares each Track's zones into immutable sample/parameter
snapshots, excluding loading Tracks. A fixed engine-lifetime allocation from
the existing SDRAM allocator holds the pending map and triple-buffer mailbox;
its bytes remain allocated when the Sample Pool is cleared. This storage is
accounted in allocator usage, and allocation failure leaves sequencing silent.
The callback acquires a consumer-owned snapshot without copying the whole map.
For a trigger, it scans at most 32 contiguous zone keys and copies at most four
matched parameter records. Tuning, Sample Pool lookups and inherited parameter
resolution remain foreground work; only key/velocity matching and crossfade
gain depend on the actual event.

Rebinding revokes only the affected Track before requesting the callback's
voice-stop acknowledgement. The callback acquires the revoked map before
acknowledging retirement, so no later step can resurrect a freed sample.
Other Tracks continue. A completed or failed load republishes the current
bindings, and sample edits refresh future triggers without modifying already
sounding snapshots.

The ESP32 edits and displays sequencer state; it never generates audio trigger
timing. Callback playhead state crosses a mailbox to the main loop, which
coalesces UART publication. The existing Play grid and live-parameter controls
are independent of the step-editor workflow.

## 2. Clocking

The timebase is the audio frame count, with the current control tick at an
audio-block boundary. `sequencer_scheduler.hpp` implements double-precision anchored
musical timing with intra-block frame offsets, swing, microtiming, retriggers
and seeded probability.

`tempo_follower.hpp` and its transport integration are host-testable.
[MIDI sync](midi-sync-tempo-follower.md) distinguishes the implemented core
from the remaining physical timing and bench work. Wire deltas are in the
ESP32 clock domain; they must not be treated as absolute Daisy timestamps.

Internal Continue seeks to its SPP. MIDI Start/Continue wait for the following
Clock; SPP relocates Patterns and Song sections/repeats while stopped. External
Stop preserves the paused Song lease; local Stop releases it. Live recording
remains separate work.

## 3. Pattern and voice models

The shared `sequencer/pattern_data.hpp` model contains 16 rows, up to 64 steps
per row and four parameter locks per step. The default length is 16 steps.
Row r addresses Track r; each step owns a MIDI note (default 60), velocity
and trigger data. Notes 60-75 select the sixteen default kit pads. Empty or
loading Tracks are silent and never borrow another Track's Instrument.
Retriggers retain the primary hit's note, velocity and step identity even when
the pending pattern is edited.

The foreground prepares immutable zone data and derives a compact lookup
when drum zones each own a distinct note within a sixteen-note window.
The callback then inspects only that pad's zone, retaining its velocity
bounds and fades. Wider ranges and overlapping layers keep the bounded,
ordered zone scan; empty and retired pads remain silent.

These are one-note drum-shaped triggers. Changing note does not implement
melodic gate lengths, automatic note-offs, chords or live recording.

The target hierarchy is defined once in
[track-and-patch-model.md](track-and-patch-model.md): Patterns address Tracks;
Tracks hold Instruments; a Kit is a drum-mode Instrument; Songs own their
arrangement and tempo. The target default is 32 steps. Bank/Project/Song
storage and melodic step-note lanes are not implemented merely because the
scheduler can play a pattern.

Digital voice playback is already implemented in `voice_manager.hpp`:
resident PCM16 mono/stereo, root-note-aware tuning, layering, choke/one-shot
semantics, region/fade/loop parameters, per-voice filter and envelopes.
Live filter edits reach release tails; envelope edits preserve an already
releasing/choked voice's release. The filter A/B console and its measurement
requirements live in [logging.md](../logging.md) and
[performance_monitoring.md](../performance_monitoring.md).

Browser audition streams a file without making it resident. Loading and
binding create the Instrument used by Play. Binding state is reported by the
Daisy, not inferred from the frontend metadata cache.

## 4. Edits and protocol

`SequencerTransport` keeps pending and active pattern copies. Commands change
the pending copy; it is committed while stopped or after a processed step
boundary. The callback is the sole runtime writer of both copies.

Use the existing transport, pattern-op, playhead and MIDI messages in
[inter-mcu-protocol.md](inter-mcu-protocol.md).
MSG_SEQ_PATTERN_SYNC provides the pending-pattern page readback below.
KIT_OP is not a live competing Instrument format.

Locks are stored by the pattern model and carried in `TriggerEvent`, but
`drain_sequencer()` does not yet apply them to voice parameters. The
trigger-override and one-step lifetime rules are in
[param-locks-and-modulation.md](param-locks-and-modulation.md#2-parameter-locks).
Add pure mapping/clamping tests when implementing that path.

## 5. UI surfaces

The existing Play page provides Pads and Keys with shared note lifecycle,
Track selection, binding status and live sound controls. Navigation and
threading are described in [ui-architecture.md](../ui-architecture.md).

Remaining surfaces include live lock recording, pattern/song selection
and melodic gate/chord lanes. Panel keys already have a logical model;
LEDs and endless-pot drivers are separate remaining prerequisites in
[panel-controls.md](panel-controls.md). Do not describe a debug-console
transport command as a completed panel workflow.

### Pattern files (as-built)

From the sequencer, choose **Shift → Patterns → Files**. Enter a name and choose
**Save copy**, or enter an existing name and choose **Load → Confirm**.
**New → Confirm** clears the working pattern and restores its default groove
settings. The keyboard's checkmark dismisses the keyboard; file operations
use the named softkeys.

Patterns are new-copy saves in `0:/wavex/patterns/<name>.wxpat`. Names use
1–23 ASCII letters/numbers, spaces, hyphens or underscores, without outer
spaces. Existing names are refused. The page reports the last successful
save/load name; it is not a dirty-state indicator or a file browser.

A pattern contains all sixteen rows and all 64 steps, including hidden steps,
mute, length, scale, swing, notes, velocity, probability, microtiming,
retriggers and stored locks. It contains no tempo, Track instruments or
runtime sample IDs. Loading stops sequencing after validation; current
voices retain their own lifecycle. Tempo, clock settings and Track bindings
stay in the session. Chords, gate lanes and song arrangement remain separate
roadmap work.

The codec is [pattern_file.hpp](../../firmware/shared/wxcf/pattern_file.hpp):
WXCF file type 5, schema 1.0, one metadata chunk and sixteen row chunks.
Fields are explicitly little-endian; native C++ struct layout is never a
file format. Duplicate/missing required chunks, invalid field values, newer
major versions and truncation fail the load. Unknown chunks are skipped
incrementally within a 64 KiB file limit.

The foreground owns the FatFs job. It creates a unique temporary file,
checks every write and close, then renames to a previously unused destination.
Failed jobs remove only their own temporary. Interrupted temporary files are
ignored; this is not a claim of FAT recovery after arbitrary power loss.

One fixed pattern buffer crosses the callback boundary by exclusive
release/acquire ownership. Saving captures one row on each block without
scheduled triggers, restarts on intervening edits, and fails after 500
callback opportunities if no consistent capture is possible. Loading decodes
privately, then the callback installs the complete validated pattern and
discards that block's old-pattern events. Load/New block incoming pattern
edits and Play/Continue until completion; Stop, tempo configuration and
readback remain available. No filesystem work runs in the callback.

The storage pump advances at most eight codec records per main-loop service.
Accepted Save/Load operations stop the singleton streaming preview before
filesystem work, matching kit-save admission. Resident Track voices continue;
preview playback can be started again afterward. Read retries and rejected
requests do not repeat that stop. File status retains the active and last
completed request IDs. Read retries recover a lost completion without
replaying Save, Load or New.

## 6. Validation

The named-file path has host/sanitizer coverage for every truncated file
prefix, field/chunk validation, short writes, failed close/rename, retained
completion, capture retries and install ownership. The two-board test in
`tests/hil/test_pattern_files.py` passed on 2026-09-08: it saved/reloaded hidden
Track 16 / Step 64 data, rejected duplicate/missing names, exercised New and
Load confirmation/cancel, preserved tempo and bindings, and kept another
Track's held voice alive. Both device images compiled and flashed; real
touchscreen captures are `logs/sequencer-notes.png` and
`logs/pattern-files.png`. The whole Phase 2 gate and arbitrary-power-loss
recovery are not established by this focused test.

The later sustained write test found SD controller CRC/time-out errors and
missing directory entries. The default bus clock was reduced, and six
save/load cycles passed in a fresh diagnostic directory. Existing visible
files reload after restart, but new saves in the original test directory
still require recovery/recreation approval. The final ten-minute audio/grid
capacity run passed with file operations excluded; see
[the complete bench evidence](../callback-performance-log.md).


Host coverage includes scheduler event ordering and timing, transport edits
between steps, probability/retrigger boundaries, tempo-follower state,
command-queue handoff, Track mapping, scoped snapshot revocation and voice
rendering. Lock application remains separate work. The hardware regressions in
`tests/hil/test_sequencer_tracks.py` exercise four independently released
Tracks (including Track 16), rebinding and SFZ import during sequencing, and sample-edit
refresh for subsequent hits.

Hardware acceptance remains in the roadmap: audible pitch across the Keys,
SFZ root-note behavior, sample-loop behavior, live parameter sweeps,
sample-offset triggering, MIDI sync/jitter, peer restart during playback,
DWT callback headroom and an eight-voice zero-underrun soak.
No performance improvement is claimed without the corresponding DWT result.

## Related

- [Instrument and sample ownership](instrument-model.md)
- [Melodic sequencing target](melodic-sequencing.md)
- [Testing guide](../testing_guide.md)


## Touch grid implementation

The main menu's Sequencer page shows four Tracks by sixteen steps, with
Track and step paging across sixteen Tracks and sixty-four steps. Touch a
cell to select its Track/step and toggle it; drag the tempo, swing, length,
scale, velocity, probability and note tiles to edit. Play/Stop leaves the pattern
running across navigation. Shift exposes Track mute, Step off and confirmed
Clear row (all sixty-four steps, including locks and hidden pages).

Daisy owns the pending pattern and publishes requested sixteen-step windows
from the callback through a snapshot mailbox. Request IDs reject stale
responses after a page/Track change. The UI disables unread cells and retries
lost readback; link loss invalidates its editable cache. Main-loop UART
publication retains unsent state, including a coalesced 25 Hz playhead.
Tempo configuration preserves transport position. The Note tile selects MIDI
0-127; notes 60-75 also display their default pad number. Parameter-lock
application and melodic gate/chord lanes remain separate work.

The connected-board regression test covers edits at Track 16/step 64, tempo
and swing changes, navigation while playing, and confirmed row clearing
through on-screen softkeys. It does not verify physical panel wiring, audible
timing, external MIDI synchronization or the Phase 2 gate.


### Per-step parameter locks

Sequencer → Shift → Locks edits four voice-scoped overrides on the selected
step. Tapping a step in this view selects it without toggling the note.
The existing pattern codec retains every lock, including hidden steps.
Application, mappings and the future live-recording/analog scope are documented
in [param-locks-and-modulation.md](param-locks-and-modulation.md).


### Project Pattern selection

The stopped slot manager is implemented separately from standalone files:
[Pattern management](pattern-management.md). It captures the outgoing working
Pattern and installs the selected slot without changing session settings.
Queued launch switches on the Daisy full-loop boundary with intra-block frame
offsets. [Song execution](song-sequencing.md) counts repeats and advances sections
on the same clock, with immutable Pattern references and explicit stop/loop rules. No UI timer schedules musical events.
