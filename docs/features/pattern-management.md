# Project Pattern management

Pattern slots let you build variations within one Project. This Phase 2 increment
adds create, copy, rename and selection plus queued loop-boundary launch.
Song arrangement and execution are implemented in [Song sequencing](song-sequencing.md).
Hardware acceptance is tracked separately.

## Data and ownership

A Project owns 128 stable Pattern slots. Each slot contains a used flag, a
23-character name and the complete Pattern, including hidden steps and locks.
Names are labels, not identities: Songs reference slot numbers. The active slot
has one callback-owned working Pattern; inactive slots belong to the foreground
Project document. The callback working copy is authoritative until captured.

Create initializes an empty destination without selecting it. Copy snapshots the
active working Pattern into an unused destination. Rename changes an occupied
slot's label. Select captures all outgoing edits before installing an occupied
slot. Create/Copy never overwrite an occupied slot; selection preserves tempo,
clock/input settings, Instruments, mixer state, Solo and existing voice lifetimes.
The UI displays slots 1–128; storage and protocol use 0–127.

## Operation boundary

Create, Copy, Rename and the explicit stopped Select operation require a
stopped, unarmed transport. Launch selects immediately while stopped and
queues while playing; it refuses MIDI-armed state until playback starts. The
callback checks that condition throughout the row-wise capture. Playing or
MIDI-armed requests fail with Stop first and never stop playback implicitly.
The Project owner gates edits, Play/Continue and competing storage jobs during
stopped capture/install; Stop, note releases and readback remain available. There is no SD
I/O for slot operations. Project Save copy persists the complete slot collection.
Standalone Pattern file Load/New replaces the active slot's working contents;
its latest contents and name are captured before leaving that slot.

A single foreground job owns the exchange. GET reads retained completion and
never repeats a mutation. A successful Select is reported only after callback
installation acknowledgement; a missing acknowledgement leaves the job pending.
The existing fixed exchange buffer is reused, with no callback allocation and
one row per capture block. Memory admission for the retained Project occurs in
the foreground and failure leaves the working Pattern unchanged.

## Queued transitions (implemented)

The active slot and one queued destination are distinct. A queued launch takes effect at the outgoing Pattern's next full-loop grid
boundary on the Daisy audio clock, before the destination's step zero. The
boundary is independent of Track micro-offsets. Stop or a transport restart cancels the queue. This increment accepts one
launch at a time; replacing the queued destination is not yet exposed. Edits continue to
address the active slot, and the outgoing snapshot must include edits accepted
before the switch. The boundary uses the length/scale at launch acceptance;
later groove edits do not move that armed musical boundary. Tempo changes
preserve phase and move it in audio frames. No foreground timer or UART arrival time generates the switch.
Different lengths/scales start the destination at its own step zero while keeping
session tempo and voice tails. The outgoing next-loop step zero is suppressed even with negative microtiming;
outgoing tails/retriggers end at the launch boundary. A negative destination
step-zero offset clamps to that boundary. The scheduler splits the audio block
when needed, keeping old events before and destination events after the split.
Probability RNG state continues; current voice tails are not stopped.

The exchange lends its immutable destination buffer to the callback. At launch
the callback swaps working-buffer pointers, returning the outgoing working
Pattern (including the latest edits) to the foreground. It then updates its
existing active scheduling copy. All three buffers have engine lifetime, and
none is reused by the foreground until release/acquire acknowledgement. No
large outgoing Pattern is constructed on the callback stack.

Grid readback includes slot identity and a replacement epoch. UI edits carry
that pair, and the callback rejects an old pair even after switching away and
back to the same slot. A new epoch invalidates the grid's other cached rows.
Legacy unscoped debug commands still address the current working Pattern.
Standalone file jobs cannot claim the exchange while a launch is pending.

## Songs

[Song sequencing](song-sequencing.md) arranges these stable Pattern slots with
repeat counts. Its immutable playback loan freezes Pattern edits until the
callback acknowledges Stop or the final section boundary.

## Related

- [Sequencer](sequencer.md)
- [Project persistence](project-persistence.md)
- [Implementation order](../roadmap.md)
- [Hardware validation](../hardware-validation.md)
