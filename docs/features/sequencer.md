# Sequencer and Digital Voice Playback

**Status:** The scheduler, transport, command queue, callback trigger path and
playhead publication are implemented. Each of the 16 pattern rows addresses
the matching Track's Instrument at the step's selected MIDI note. Velocity
layers and crossfades use the step's velocity. Chords and melodic gate lanes
remain future Phase 2.5 work.
Physical panel integration, MIDI clock output, parameter-lock application and
project persistence remain open Phase 2 work in [roadmap.md](../roadmap.md).
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
audio-block boundary. `sequencer_scheduler.hpp` implements fixed-point
musical timing with intra-block frame offsets, swing, microtiming, retriggers
and seeded probability.

`tempo_follower.hpp` and its transport integration are host-testable.
[MIDI sync](midi-sync-tempo-follower.md) distinguishes the implemented core
from the remaining ESP32 ingest/output and bench work. Wire deltas are in the
ESP32 clock domain; they must not be treated as absolute Daisy timestamps.

Internal-mode Continue currently restarts the scheduler at the top. Stored
song-position/input-mode fields do not imply implemented song resume or live
recording.

## 3. Pattern and voice models

The current bounded `pattern.hpp` model contains 16 rows, up to 64 steps
per row and four parameter locks per step. The default length is 16 steps.
Row r addresses Track r; each step owns a MIDI note (default 60), velocity
and trigger data. Notes 60-75 select the sixteen default kit pads. Empty or
loading Tracks are silent and never borrow another Track's Instrument.
Retriggers retain the primary hit's note, velocity and step identity even when
the pending pattern is edited.

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

Remaining surfaces include parameter-lock editing, pattern/song selection
and melodic gate/chord lanes. Panel keys already have a logical model;
LEDs and endless-pot drivers are separate remaining prerequisites in
[panel-controls.md](panel-controls.md). Do not describe a debug-console
transport command as a completed panel workflow.

## 6. Validation

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
