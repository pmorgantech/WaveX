# Melodic sequencing — lanes, gates and recording

**Status:** Implemented, 2026-09-20. Hardware acceptance is tracked in
[HV-024](../hardware-validation.md#hv-024--melodic-sequencing).
The broader [Phase 2.5 gate](../roadmap.md#phase-25--sampler-instrument-layer)
remains open.

## Pattern ownership and persistence

Every Pattern has sixteen rows of sixty-four steps. A row selects drum or
melodic playback; switching type preserves both representations. The existing
step on/off, probability, micro-offset and parameter locks govern the whole
chord. Melodic playback ignores the drum retrigger fields.

Each step has four fixed lanes: MIDI note 0–127, velocity 0–127 and a gate of
0–32767 ticks at 96 PPQN. **Velocity zero means empty**, so MIDI note zero is
playable. An enabled step with no occupied lanes is silent. Nonzero lane edits
enable their step. Drum rows retain their original single-note behavior.

The callback owns edits and swaps pending data into the scheduled Pattern at
the existing step boundary. Song playback borrows a frozen Project and rejects
recording and edits. Scoped editor commands carry Pattern slot and epoch;
replacing a Pattern invalidates an outgoing editor's commands.

Pattern WXCF schema 1.1 uses 36-byte step records and a row melodic flag.
Schema 1.0 still loads as drum rows with empty lanes, including into reused
scratch storage. Project schema 1.3 embeds the same lanes; earlier Project
Pattern chunks retain their 20-byte decoding. Hidden steps are saved too.
Instrument/Track polyphony remains separate from Pattern data.

A native Pattern is approximately 40 KiB. The foreground exchange buffer lives
in CPU-only D2 SRAM; the transport's active and pending buffers remain internal.
No Pattern-sized temporary is placed on the callback stack. A complete Project
is approximately 5.3 MiB, allocated from the existing SDRAM arena; loading can
require both retained and candidate Projects. The file-size ceiling is 8 MiB.
Allocation failure leaves the current session intact.

## Gate and voice lifetime

Positive gates end at the trigger's musical tick plus the lane duration.
Tempo changes re-anchor the tick-to-frame conversion, preserving the remaining
musical duration. Microtiming and swing affect the trigger and thus its gate.
Positive gates may overlap later notes in the same lane, subject to admission.

Gate zero is **Hold**: it continues through rests until that row/lane plays
again, or transport/binding/Pattern cleanup ends it. The next note retriggers
its envelopes; this is not pitch glide or true legato. An attempted new lane
trigger ends the previous hold even if the new note is refused at capacity.
A muted row or switching it to drum mode also ends its melodic voices.
Gates do not create sample loops or extend a finite source/envelope lifetime;
natural completion or stealing may end a note earlier.

Melodic duration is explicit: it releases even a one-shot Zone. Live keyboard
note-off and drum playback retain their existing one-shot semantics. Release
tails use the sound's envelopes and continue to consume allocation capacity.
Stop/restart/locate, Pattern replacement and Song boundaries clean up melodic
ownership without releasing independent live keys. Sample/binding retirement
still uses the existing callback fence.

Gate state belongs to each sounding group member, alongside its stable group
identity. Stealing or reusing a render slot replaces that state, so no old
queued note-off can cut a newer voice. There is no growing pending-off queue.
This supersedes the proposed pending-off queue/overflow policy: each admitted
voice carries its own gate, so gate storage cannot overflow independently of
the fixed voice pool. Slot-reuse and finite/Hold release tests cover this model.
The scheduler emits at most 64 events per callback, enough for sixteen
simultaneous four-note chords. This event capacity does not increase the
physical voice/channel budget: admission still keeps or refuses whole notes.

Audio renders chronologically to each event frame before admitting that batch.
A later steal therefore cannot remove earlier audio from the block. Global LFO
and mixer updates remain once per callback; voice modulators advance by each
render segment's duration. Gate releases occur at the exact sample within the
segment. All state is fixed-capacity; the callback performs no allocation,
logging, file access or blocking transactions.

## Editor and capture

Open **Sequencer → Notes** beside the clock/Stop controls. Select a lane and
edit note, velocity or gate with the tiles or focused encoder. Step −/+ can
reach hidden steps. Shift exposes drum/melodic type, quantize and clear-lane
controls. The four-lane summary and record cursor use confirmed backend state.
Changes to the displayed step during recording/erase briefly show **Step updated**,
including overdub replacements; identical polling does not retrigger the notice.
Save through the existing Pattern or Project workflow.

The mode softkey cycles Play, Step rec, Live rec and Erase. Capture is armed for
the selected Track/step on entering the editor or selecting another step.
Other Tracks still monitor normally. Navigating away does not silently change
the session's input mode. Quantize cycles **off / step / half-step** using the
existing transport field. Half-step capture uses the step's shared micro-offset.

- **Step record:** MIDI/direct-Track presses fill the selected step. The cursor
  advances when all captured keys are released. Each lane initially receives
  one grid interval of gate; stopped key-hold duration does not set a gate.
- **Live record:** while running, the Daisy callback captures the consumed
  key event's musical time. Quantize selects the nearest step or half-step;
  without quantize, capture uses the preceding step and stores the first
  note's residual as the shared step micro-offset, compensating for swing.
  Note-off records a rounded duration of at least one tick. Notes within one
  chord share step timing; there is no per-lane micro-offset.
- **Overdub:** empty lanes fill first. A fifth note replaces the oldest capture
  in that step; pre-existing lanes are treated as older than this capture pass.
  The replaced capture loses its lane ownership, so its later release cannot
  rewrite the replacement's duration.
- **Erase:** hold pitches while the playhead passes to clear matching lanes on
  the armed Track. Other pitches/Tracks are preserved. Monitoring remains live.

A fixed 64-key capture ledger uses the live queue's source/pitch/press identity,
including repeated pitches. A full ledger refuses additional capture while
normal monitoring proceeds. Input events refused before reaching the callback
cannot be recorded. Queue-overflow release watermarks retire captures; their
initial finite gate remains safe. Mode/target/run-epoch/Pattern changes clear
capture ownership. Unfinished notes retain their initial one-step gate.
Queued presses invalidated by a Track rebind are excluded from capture, as
they are from playback. A manual lane replacement invalidates that lane's
held capture, preventing its later release from rewriting the edited gate.

The additive `MSG_SEQ_NOTES` request/reply and scoped lane/type/target edits
are defined in [protocol.h](../../firmware/shared/spi_protocol/protocol.h) and
mirrored in the [protocol guide](inter-mcu-protocol.md). The existing grid page
stays within UART's payload limit; one melodic step has a separate readback.

## Validation and remaining work

Host coverage includes full chords, MIDI note zero, whole-chord probability,
64-event bounds, exact gate frames, old-slot/live-key isolation, tempo mapping,
capture quantization/duration, oldest-lane replacement, erase and old/new file
round trips. HIL procedures and dated results belong to HV-024.

Physical MIDI latency, listening, panel interaction and the complete Phase 2.5
performance gate require their own evidence. `--melodic` in the callback bench
adds four-note chord pressure to the existing dual-oscillator/stream/file load;
a short screen does not replace the required soak.

Portamento/true legato, per-lane probability/microtiming, scale-constrained pitch
editing, polyphonic aftertouch and external melodic MIDI output remain separate
follow-ups. The arpeggiator and audio sampling/recording are not part of this
note recorder.

The original pitch-entry proposal depended on an active scale mask. That mask
and its source of truth are still the unimplemented Phase 5
[tuning/scales design](tuning-and-scales.md); current pitch entry is chromatic.
Scale snapping remains an explicit dependency, not a completed editor feature.
