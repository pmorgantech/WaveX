# Project Pattern management

Pattern slots let you build variations within one Project. This Phase 2 increment
adds stopped create, copy, rename and select; queued transitions and Song
execution remain the next increments. Hardware acceptance is tracked separately.

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

For this increment every mutation requires a stopped, unarmed transport. The
callback checks that condition throughout the row-wise capture. Playing or
MIDI-armed requests fail with Stop first and never stop playback implicitly.
The Project owner gates edits, Play/Continue and competing storage jobs during
the exchange; Stop, note releases and readback remain available. There is no SD
I/O for slot operations. Project Save copy persists the complete slot collection.
Standalone Pattern file Load/New replaces the active slot's working contents;
its latest contents and name are captured before leaving that slot.

A single foreground job owns the exchange. GET reads retained completion and
never repeats a mutation. A successful Select is reported only after callback
installation acknowledgement; a missing acknowledgement leaves the job pending.
The existing fixed exchange buffer is reused, with no callback allocation and
one row per capture block. Memory admission for the retained Project occurs in
the foreground and failure leaves the working Pattern unchanged.

## Queued transitions and Songs (target, not implemented here)

The next increment will distinguish the active slot from one queued destination.
A queued selection takes effect at the outgoing Pattern's next full-loop grid
boundary on the Daisy audio clock, before the destination's step zero. The
boundary is independent of Track micro-offsets. Stop cancels the queue; replacing
a queued destination does not replace the playing Pattern. Edits continue to
address the active slot, and the outgoing snapshot must include edits accepted
before the switch. No foreground timer or UART arrival time generates the switch.
Different lengths/scales start the destination at its own step zero while keeping
session tempo and voice tails. Microtiming/retrigger cutoffs and intra-block
switching require scheduler tests before this target is enabled.

Song entries reference these same stable slots with repeat counts. Song
loop/stop, seek and edit-during-play rules must be defined with the Song owner
before adding execution; a saved Song record does not imply runtime playback.

## Related

- [Sequencer](sequencer.md)
- [Project persistence](project-persistence.md)
- [Implementation order](../roadmap.md)
- [Hardware validation](../hardware-validation.md)
