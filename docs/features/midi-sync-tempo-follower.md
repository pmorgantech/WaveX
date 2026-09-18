# MIDI clock sync and tempo follower

**Status (2026-09-17):** Implemented and host-tested; physical timing and the
Phase 2 DAW gate remain open in [HV-014](../hardware-validation.md#hv-014--midi-ports-and-clock-serialization)
and [the roadmap](../roadmap.md). No firmware has been flashed for this change.
DIN remains disabled until its wiring is confirmed. Configuration and pins live
only in the [canonical headers](../../firmware/shared/config/).

## Timing ownership

The Daisy audio-frame clock owns scheduling. The existing HAL-free scheduler
uses double-precision musical anchors and integer frame positions; the follower
estimates tempo and slews the scheduler rate. ESP32 tasks handle physical MIDI
ports and forward values over the current UART inter-MCU link. No MIDI driver,
link send, allocation or logging runs inside the audio callback.

MIDI uses 24 clocks per quarter note; the scheduler uses 96 internal ticks per
quarter. One MIDI clock therefore spans four internal ticks. SPP is a 14-bit
count of sixteenth notes (six MIDI clocks, or 24 internal ticks per unit),
independent of Pattern step scale. The wire definitions remain centralized in
[`protocol.h`](../../firmware/shared/spi_protocol/protocol.h), with routes in
[the protocol guide](inter-mcu-protocol.md). See the MIDI Association's
[message reference](https://midi.org/expanded-midi-1-0-messages-list) and
[system-message overview](https://midi.org/about-midi-part-3midi-messages).

## Input and clock domains

Each port owns a `ClockInput` parser alongside its note parser. It recognizes
Clock, Start, Continue, Stop and complete SPP, including real-time bytes inside
an SPP message. A new status abandons an incomplete SPP. Sequence numbers advance
for every parsed Clock, even if subsequent inter-MCU admission fails.

DIN timestamps bytes in the UART consumer task. USB timestamps transfers in the
TinyUSB RX callback and hands bounded, immutable chunks to its worker through an
SPSC ring. These are software reception timestamps, not hardware wire timestamps:
DIN backlog and USB transfer grouping limit accuracy. No sub-millisecond ingress
latency is claimed. USB lifecycle generations and chunk sequence gaps reset
partial parser state; data from an old connection cannot complete a new message.
Espressif attach/detach events use its supported
[`event_cb` hook](https://github.com/espressif/esp-usb/blob/master/docs/device/migration-guides/v2/tinyusb.md).

`esp_delta_us` is a per-clock period measured entirely in the ESP32 domain.
Normally it is the interval between adjacent received Clock bytes. Equal USB
batch timestamps produce zero deltas; the next distinct timestamp yields the
mean period across the preceding group's clock count. This estimates period
without inventing individual arrival times. The first clock after parser reset,
Start or Continue has no period sample. Period samples below 3.8 ms or above
2.5 seconds do not enter the estimator.

The Daisy never subtracts an ESP32 timestamp from an audio-frame timestamp.
Inter-MCU sequence gaps advance phase accounting by the number of missing clocks;
they **do not divide the period again**, because the wire already carries a
per-clock interval. Duplicate/backward clock sequences are ignored. Raw USB-ring
loss cannot reconstruct unparsed clock bytes; a lost transport command or peer
restart may require local Stop/re-arm and a fresh master Start.

## Transport and source selection

The sequencer's Internal/MIDI button selects the source while stopped. Tempo is
editable in Internal mode; MIDI mode displays measured BPM and lock/freewheel
status. The Arm softkey enables MIDI following. The separate Stop button always cancels the
arm, including while no external clock is arriving. Console `PAGE CLOCK 0|1`
and `PAGE STOP` provide the same controls; page state includes `seqclock`,
`seqsync` and `seqmeasured`.

- Internal Play starts immediately from zero. Internal Continue locates its
  explicit SPP and emits SPP, Continue, then the first Clock.
- MIDI Play/Continue arms without sounding. An external Start selects zero;
  external Continue uses the last accepted SPP or stopped sixteenth position.
  Playback begins on the **next Clock**, not on Start/Continue itself.
- SPP alone never plays. It is accepted while stopped and ignored while running.
  Seeking skips elapsed notes/retriggers. A standalone Pattern wraps naturally;
  a Song resolves the corresponding section and repeat, wrapping a looping Song
  or stopping beyond the end of a finite Song.
- External Stop pauses a Song while retaining its immutable Project lease, so
  SPP/Continue can resume it. Local Stop, project replacement or source changes
  release that lease and disarm external restart. UI edits remain read-only
  during an externally paused Song until local Stop.
- The first valid clock/transport event selects DIN or USB. Other-port events
  cannot stop, reposition or retime that session. Local arm, local Stop or source
  change releases the selection. There is no automatic failover.
- MIDI-follow mode does not echo clocks to either output port. This prevents a
  feedback loop when a DAW routes both directions.

Local Song starts from a selected section establish a new output-clock origin
there. External SPP addresses the whole loaded Song from its first section.
Continue without SPP currently resumes at the stored sixteenth boundary;
sub-sixteenth pause preservation is not implemented. These are explicit limits,
not sample-accurate resume claims.

## Tempo follower

Five mutually consistent period samples acquire lock. The locked estimator uses
an EMA (alpha 1/8), rejects isolated deviations over 25%, and re-acquires after
three consistent tempo-change samples. Phase error is measured on the Daisy
arrival timeline and slews rate by at most 0.5%; routine clock updates do not
jump scheduler phase. Two missing periods enter freewheel at the last rate.
Returning clocks re-acquire. Explicit Start/SPP/Continue are transport relocations.

The existing jitter, crystal-offset, ramp, tempo-jump and dropout host tests
exercise the follower. Constant transport latency and variable UART/SD/USB
congestion still need physical measurement. Host lock is not evidence of DAW
alignment or audio stability on the boards.

## Output and congestion

Internal playback emits Clock at each four-tick boundary from scheduler phase,
so tempo changes do not introduce a second independent clock. The callback
publishes ticks to a fixed ring and transport state to a lock-free mailbox.
The latest Start/Continue/Stop supersedes old-run ticks; a full clock ring cannot
hide Stop. Foreground publication is bounded to eight events per pass, and ticks
older than 50 ms are discarded. Link-admission failure drops ticks but retries
the latest transport state, including the ordered SPP/Continue pair.

The ESP32 port queues/driver serializers are described in
[panel controls](panel-controls.md#midi-port-implementation-stage-5-2026-09-17).
Port queue or driver failure can still lose an event after link admission;
`MIDIOUT` exposes those counters. Already accepted UART/USB packets cannot be
recalled. Neither a 50-ms expiry nor a successful enqueue promises bounded wire
jitter or lossless delivery. Under congestion, restart transport after recovery;
do not infer correct sync from counters alone.

## Validation

Host coverage includes parser interleaving/wrap/batching, fixed SPSC ordering,
wire validation/round trips, 24-PPQN output at 20/120/300 BPM, transport retry and
Stop priority, source isolation, first-Clock starts, maximum SPP Pattern seeking,
Song section/repeat/end/loop seeking, and the follower's synthetic jitter/drift
suite. Both firmware builds and enabled/disabled port variants are required.

[HV-014](../hardware-validation.md#hv-014--midi-ports-and-clock-serialization)
owns the unrun panel, DIN/USB, DAW drift, fault and DWT/underrun procedures.
The ten-minute drift target is ±3 ms against the DAW metronome, without audible
drift. If output jitter is unacceptable, measure before reconsidering which MCU
owns DIN output. The complete Phase 2 gate remains open.

## LFO sync controls

Instrument per-voice LFO Rate switches between 0.01–100 Hz and musical durations
when Sync is toggled. 3/16 joins the existing divisions without changing saved
IDs, and Hz is retained independently. This follows transport tempo while
preserving held-note phase across Start/SPP and edits. See
[the modulation design](param-locks-and-modulation.md#lfo-rate-controls) and
[HV-015](../hardware-validation.md#hv-015--lfo-range-and-musical-rate-controls).
