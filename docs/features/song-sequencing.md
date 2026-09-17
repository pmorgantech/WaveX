# Song sequencing

**Status:** Phase 2 software implementation; physical acceptance remains open in
[HV-009](../hardware-validation.md#hv-009--song-arrangement-and-playback).

## Data and ownership

The foreground Project owns 16 stable Song slots. Each used Song has a name,
tempo and 1–128 ordered sections. A section references one occupied Pattern slot
and repeats it 1–255 times. Pattern names are labels; references use stable slot
identity. Create starts with one section referencing the current working Pattern.
Removing the last section is refused. Existing Project WXCF records already
persist this model; no on-disk version change is needed.

All arrangement edits and Song starts first capture the stopped, unarmed working
Pattern through the existing row-wise exchange. This preserves hidden steps and
locks before the Project is used. Create, rename, insert, remove, move, Pattern /
repeat changes and Song tempo changes require this boundary. A used Song always
remains serializable; invalid or empty Pattern references are rejected.

During playback the Project owner lends an immutable Project to the callback and
rejects competing Project, Pattern-slot and file jobs. Pattern edits are refused
in the callback, including commands queued before a section change. Scoped grid
readback carries a read-only flag, so the grid remains visible without offering
edits. Track voices, mixer controls and note releases retain their usual paths.
The UI and foreground never generate section-change times.

The scheduler reads the frozen Patterns directly. Section transitions do not copy
a full Pattern. On Stop or normal completion the callback restores the current
Pattern into its owned working buffers before release acknowledgement. Only then
may the foreground mutate or free the Project. Foreground pauses cannot miss a
section deadline. Allocation, filesystem access and UART remain outside the
callback. Start/stop and steady playback still need measured DWT acceptance.

## Playback rules

- **Play from here** starts the selected section at step zero and uses the Song
  tempo. In MIDI-clock mode it arms and waits for Start/Continue, as regular
  Pattern playback does; the external clock governs timing.
- Each section plays its complete Pattern loop for its repeat count. The next
  section begins at the exact full-loop grid boundary, including a boundary
  inside an audio block. Length/scale changes start the next Pattern's own grid.
- **Loop off** stops scheduling at the final boundary. **Loop on** returns to
  section one, even when playback initially started at a later selected section.
  Loop choice is a playback option, not a persisted Song field.
- **Stop** cancels playback/arming; it does not cut existing voice tails. It waits
  for callback acknowledgement before reopening arrangement edits. The final
  sounding Pattern becomes the normal editable active Pattern.
- Seeking is stopped-only: Stop, select another section, Play from here. There
  is no mid-section resume or Song-position-pointer seek. Ordinary transport
  Play/Continue exits Song mode and restarts the current Pattern; MIDI Stop exits
  Song mode. Start/Continue only starts an armed Song, following the existing
  transport contract.
- Pattern edits are frozen while the Song owns playback. Live session tempo
  configuration retains existing transport behavior; it does not rewrite the
  saved Song tempo. Edit the Song's tempo while stopped to retain a new default.
- Negative micro-offsets cannot anticipate the outgoing next-loop downbeat across
  a section boundary; destination step zero clamps to that boundary. Retriggers
  from the outgoing section cannot cross it. Probability RNG continues across
  sections. These are the same rules as [Pattern launch](pattern-management.md).

## Touch workflow

Open **Sequencer → Shift → Patterns → Shift → Songs**. Song −/+ selects a slot.
Enter a name and Create, or inspect an existing Song. Six touch rows show the
current section window. Select **Section** and turn the encoder to navigate the
full arrangement. Select **Pattern**, **Repeats** or **Tempo** and turn the encoder
to stage a change; **Apply** confirms and **Revert** discards it. Pattern/repeat
edits can be combined. A dirty edit holds section navigation until resolved.

**Insert after** duplicates the selected reference/repeat pair into a new section;
**Remove** deletes it. Shift exposes **Rename**, **Move up**, **Move down**,
**Patterns** and **Project**. Project Save copy persists all Songs and Patterns.
**Play from here**, **Stop** and the **Loop** option control playback. A marker and
status line show the actual playing Song, section and repeat, independently of
editor selection. Polling uses retained request results and never retransmits a
mutation automatically. Identical status replies leave widgets unchanged.

## Verification

Host tests cover wire round trips, malformed messages, exact section boundaries,
repeat counts, loop/selected-section starts, MIDI arming, stop ownership, immutable
playback through a foreground pause, edit refusal, arrangement mutations, Project
save/recall and real-LVGL action/readiness behavior. Hardware audio continuity,
external-clock timing, display cadence, DWT capacity and soak remain unverified.
