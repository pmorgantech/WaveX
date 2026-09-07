# Sequencer and Digital Voice Playback

**Status:** The scheduler, transport, command queue, callback trigger path and
playhead publication are implemented. The current preview maps eight pattern
rows to pitches on Track 1; it is not the target multi-Track sequencer.
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

The main loop resolves the current preview Instrument into a complete voice-map
snapshot and publishes it through a triple-buffer mailbox. The callback uses
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

The current bounded `pattern.hpp` model contains eight rows, up to 64 steps
per row and four parameter locks per step. The default length is 16 steps.
The preview resolves row `r` to `root + r` on fixed Track index 0.
This temporary mapping must be replaced with Track-addressed Instrument
resolution for the four-track panel gate.

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
command-queue handoff and voice rendering. Extend these with the actual
Track mapping and lock application when those replace the preview.

Hardware acceptance remains in the roadmap: audible pitch across the Keys,
SFZ root-note behavior, sample-loop behavior, live parameter sweeps,
sample-offset triggering, MIDI sync/jitter, peer restart during playback,
DWT callback headroom and an eight-voice zero-underrun soak.
No performance improvement is claimed without the corresponding DWT result.

## Related

- [Instrument and sample ownership](instrument-model.md)
- [Melodic sequencing target](melodic-sequencing.md)
- [Testing guide](../testing_guide.md)
