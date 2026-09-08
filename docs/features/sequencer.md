# Sequencer and Digital Voice Playback

**Status:** The scheduler, transport, command queue, callback trigger path and
playhead publication are implemented. Each of the 16 pattern rows addresses
the matching Track's Instrument at MIDI note 60. Editable note lanes remain
future work.
The panel step editor, MIDI clock output, parameter-lock application and
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

The main loop resolves each Track's Instrument into a complete voice-map
snapshot and publishes it through a triple-buffer mailbox. Loading Tracks are
excluded. Rebinding revokes only the affected row before requesting the
callback's voice-stop acknowledgement; other rows keep their bindings.
A completed or failed load republishes the current bindings, and sample edits
refresh future triggers without modifying voices already holding a snapshot. The callback uses
those prepared trigger parameters, including intra-block offsets, without SD
I/O, allocation or note-resolution work against the foreground sample table.
Sample retirement must revoke these snapshots before freeing their storage.

The ESP32 edits and displays sequencer state; it never generates audio trigger
timing. Callback playhead state crosses a mailbox to the main loop, which
coalesces UART publication. The existing Play grid and live-parameter controls
are independent of the missing step-editor workflow.

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
Row `r` resolves MIDI note 60 on Track index `r`; an empty or loading Track
is silent and never borrows another Track's Instrument. The prepared map and
voice limit retain their existing sizes.

Resolution still uses velocity 127 to choose zones; a step's velocity changes
the resulting voice amplitude. Velocity-layer selection and crossfade weights
are therefore not yet step-accurate. Editable per-step notes and velocity-aware
prepared resolution must arrive together with the melodic note lanes.

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
`MSG_SEQ_PATTERN_SYNC` remains reserved without a payload implementation;
`KIT_OP` is not a live competing instrument format.

Locks are stored by the pattern model and carried in `TriggerEvent`, but
`drain_sequencer()` does not yet apply them to voice parameters. The
trigger-override and one-step lifetime rules are in
[param-locks-and-modulation.md](param-locks-and-modulation.md#2-parameter-locks).
Add pure mapping/clamping tests when implementing that path.

## 5. UI surfaces

The existing Play page provides Pads and Keys with shared note lifecycle,
Track selection, binding status and live sound controls. Navigation and
threading are described in [ui-architecture.md](../ui-architecture.md).

Remaining surfaces are the step editor, parameter-lock editing, pattern/song
selection and groove controls. Panel keys already have a logical model;
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
scale, velocity and probability tiles to edit. Play/Stop leaves the pattern
running across navigation. Shift exposes Track mute, Step off and confirmed
Clear row (all sixty-four steps, including locks and hidden pages).

Daisy owns the pending pattern and publishes requested sixteen-step windows
from the callback through a snapshot mailbox. Request IDs reject stale
responses after a page/Track change. The UI disables unread cells and retries
lost readback; link loss invalidates its editable cache. Main-loop UART
publication retains unsent state, including a coalesced 25 Hz playhead.
Tempo configuration preserves transport position. Rows currently trigger
their matching Track at MIDI note 60; arbitrary note lanes and parameter-lock
application remain separate work.

The connected-board regression test covers edits at Track 16/step 64, tempo
and swing changes, navigation while playing, and confirmed row clearing
through on-screen softkeys. It does not verify physical panel wiring, audible
timing, external MIDI synchronization or the Phase 2 gate.
