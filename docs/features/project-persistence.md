# Project persistence

**Status:** Project Save copy, Load and New are connected to the Project files
screen and foreground session owner. Both firmware images compile; host tests
exercise the real codec, file job, loader, session boundary and UI lifecycle.
Reboot, card interruption, memory-pressure recovery and callback timing remain
open in [HV-007](../hardware-validation.md#hv-007--project-save-load-and-recovery).
[Pattern management and queued launch](pattern-management.md) are connected;
[Song management and execution](song-sequencing.md) use the same persisted records.

## Contents

- [Ownership and contents](#ownership-and-contents)
- [File transaction](#file-transaction)
- [Isolated load staging](#isolated-load-staging)
- [Session workflow](#session-workflow)
- [Remaining work](#remaining-work)
- [Validation](#validation)
- [Related](#related)

## Ownership and contents

A Project owns up to 128 named Patterns, 16 Songs, one Performance (16 Track
settings and mixer strips), the default tempo and transport input settings,
and references to the saved Instruments and current Bank. A Song contains up
to 128 ordered Pattern references with nonzero repeat counts and its own
tempo. Pattern steps retain all 64 positions, including positions hidden by a
shorter pattern length, and all four parameter locks.

The data types live in
[project_data.hpp](../../firmware/shared/sequencer/project_data.hpp). Pattern
references are stable slot numbers, not compacted array positions. An unused
slot stays unused across save/load. Each Track stores an Instrument path,
MIDI input, polyphony limit, priority, Program Change setting, gain, pan offset
and mute; the Project also stores master gain. Runtime sample IDs and pointers
are never persisted.

Sample edits are Project-owned snapshots keyed by the full, case-sensitive WAV
path, matching Pool identity. Up to 1024 unique paths retain trim/loop markers,
gain, fades and channel mode. Saved rate/frame/channel/bit-depth dimensions
must match the loaded dependency before the candidate receives those edits;
its runtime ID, generation and residency remain intact. These dimension checks
do not detect replacement PCM with identical dimensions. Ordered records and
an explicit count detect missing/duplicate entries without a large decoder
bitmap. This is Project-scoped metadata, not a global sidecar that changes the
same WAV in every Project. The coordinator captures every referenced sample and restores edits before
preparing voice maps. Standalone Sample sidecar saving remains separate.

Pattern swing remains explicit (50–75), matching the current scheduler.
The future Song-default swing inheritance described in the target model has
not been added to this codec or the scheduler. Scenes, effects, melodic gate
lanes and future settings require their own versioned chunks when implemented.

## File transaction

[project_file.hpp](../../firmware/shared/wxcf/project_file.hpp) defines the
schema: WXCF Project file type 6, version 1.1, with HEAD, Bank-reference,
per-Track, per-Pattern, per-Song and per-sample edit chunks. Version 1.0 files
remain readable with no saved sample edits. Float bit patterns and integer
records use explicit little-endian encoders. No native C++ structure layout
is an on-disk contract.

Encoder and decoder expose Advance(), returning More, Done, Invalid or
IoError. Each advance transfers at most 280 bytes; a pattern step or song
entry is processed independently so the foreground storage job can yield.
Unknown chunks are skipped in bounded slices. Files are capped at 4 MiB.
Required metadata and all 16 Track records must be present, chunks cannot
repeat, booleans and finite numeric ranges are checked, and Song references
must name populated Pattern slots before the decoder returns Done.

The caller must own a private transaction workspace. The complete Project
occupies roughly 5.3 MiB: never construct it on either MCU's stack or make it
callback-visible. The device reserves retained and candidate documents through the sample
allocator; they count against available sample memory. The retained document
keeps inactive Pattern and Song slots between transactions. Candidate storage
is released after failure or promoted only on success. Playback retains its
current prepared Pattern while decoding takes place.

A failed decode may have changed the private workspace. Publish nothing until
Done. File adapters must check every read, write, close and rename, and use a
new temporary file plus checked rename to publish a save, following the
existing Pattern store. The codec itself does not create or rename files.

`storage/project_file_job.hpp` provides that foreground file adapter. It first
runs the real encoder against a counting sink in bounded batches, validating
the complete Project and measuring its exact serialized size before any file
is created. Free-space admission includes the standard cluster/directory
headroom. Saves use a new request-specific temporary and checked close/rename;
neither an existing destination nor an abandoned temporary is overwritten.
Cancellation removes only a temporary created by that job. Loads decode into
caller-owned private scratch and report success only after a checked close;
they never install runtime state. Each pump advances at most eight codec
records, and FatFs adapters use sub-sector transfers through an AXI SRAM FIL.
The session owner must keep save input immutable for both encoding passes and
must retain its scratch until completion. The session owner composes this adapter with quiet Instrument snapshots and
private restore staging.

## Isolated load staging

The loader can stage a Project's Instruments into a separate, initially empty
Track bank. The private Sample Pool copies metadata and stable IDs into its
own record storage, borrows existing PCM without changing live ownership, and
admits new samples only into the candidate. Each candidate Track loads once.
Live names, editor revisions, Revert points and callback modulation tables
remain unchanged during staging. Close failures, missing dependencies and
cancellation latch failure and prevent commit.

Abort first cancels/drains the loader, then releases only newly admitted PCM.
Commit requires every dependency to succeed and the engine's audio stop
acknowledgement. The transaction owner keeps notes and transport gated while
it retires unreferenced/unpinned old PCM, installs the candidate Pool and Track
bank, and publishes all prepared voice maps. Existing explicit user pins are
retained. Ordinary loader pumping defers throughout the staging lease.

The session coordinator must reserve candidate storage and freeze live edits
for the lease. Staging does not reclaim the current session's memory for new
samples: insufficient headroom must fail while retaining the old session.
The session owner now orchestrates these primitives under the Project transaction lease.

## Session workflow

Open **Project > Project files**. Enter a name without an extension and choose
Save copy or Load. Names follow the same bounded character rules as Pattern
files. Load and New ask for confirmation before replacing unsaved state.
Status reads correlate to requests and retain completion; reconnects and long
jobs never automatically resend a mutation. Stale readback disables actions.

`storage/project_session.cpp` is the single foreground owner. During a job,
new notes, edits and competing SD operations are gated. Existing resident
playback continues during Save; transport Stop, MIDI clock ticks/stops and note releases remain
available. Streaming audition stops to give the file job the card. A failed
Load leaves the previous session intact with playback stopped.

Save captures the callback-owned Pattern and transport settings, accepted
mixer targets, all foreground Track settings and the selected Bank file path.
It exports quiet WXI copies
into a newly owned `wavex/projects/<name>/` directory and snapshots edits for
every Track-referenced sample. WAV audio stays at its existing card path;
this is not a self-contained audio collection or a PCM render. Free-space
admission applies to directory creation, every WXI and the final Project file.
The `.wxp` publishes last, after checked writes/closes. Existing files,
directories and abandoned temporaries are never overwritten. A failed save
removes only its owned assets; a failed cleanup reports I/O failure and may
leave an orphan directory, which a later save must not overwrite.

Load decodes a private document, validates a referenced Bank's index, then
stages Instruments and PCM while retaining the live Pool and Revert points.
File dimensions must match borrowed PCM as well as saved sample edits.
Missing dependencies, malformed metadata, insufficient memory or a failed
voice-stop acknowledgement abort before live replacement. The callback first
acknowledges transport pause without changing its settings. After all staging
succeeds, the owner commits the Pool/Tracks and selected Bank index, publishes
voice maps and mixer
settings, and awaits callback installation of the active Pattern and transport
settings before reporting completion. New uses the same stopped boundary with
empty Tracks/Patterns/Songs and default settings. Explicitly pinned Pool
samples remain resident; unreferenced, unpinned old PCM is retired.

Bank restoration follows the [Bank policy](bank-persistence.md#project-restoration-and-editing-policy):
the saved filename identifies the Bank, only its index is restored, and Bank-only
samples are not preloaded. Missing/invalid Bank dependencies preserve the entire
old session. New and empty Bank references clear the selection on commit.

Solo is transient and cleared on successful Load/New; manual mutes are saved.
The frontend invalidates sample metadata caches at completion, since a retained
PCM ID can have different saved markers. Page entry reads retained Project
status and does not replay a stale Solo mask. Save leaves editor names, Revert
points and Solo unchanged. Inactive Pattern/Song slots remain in the retained
Project and survive a subsequent save; the active Pattern is editable/playable through the sequencer. Stopped slot
selection captures outgoing edits before installing another Pattern.

## Remaining work

- Run HV-007: real-card durability/reboot, interrupted saves, memory-pressure
  rollback, panel interaction and DWT measurements. Host/compile results do
  not establish a power-loss guarantee or a callback performance improvement.
- Validate Song editing/playback on hardware ([HV-009](../hardware-validation.md#hv-009--song-arrangement-and-playback))
  after defining the roadmap's Daisy-clock transition and edit rules.
- Complete selected-Bank restoration and Program Change hardware checks in
  [HV-016](../hardware-validation.md#hv-016--bank-sd-transactions). Portable audio
  collection and missing asset repair follow their own roadmap designs.

## Validation

Host tests cover sparse and full-capacity round trips, hidden steps and locks,
Track mix/routing, Song references, bounded I/O, malformed/truncated files,
unknown chunks, duplicate chunks, future major versions and write failures.
They do not establish physical SD durability, audible continuity or power-loss
recovery. Host session tests cover commit/rollback ordering, mixer/settings and
hidden-step recall, missing later Instruments, full cards, write failure and
missing voice-stop acknowledgement. Real LVGL tests cover confirmation,
long-running jobs, reconnects, stale state and timer retirement.

Loader host tests also verify Project snapshots retain the live name, undo,
revision and Sample Pool ownership, restore the edited sound into another
Track, and reject full cards and samples outside recall admission.

Staging tests cover a later missing Instrument after an earlier one loaded,
close failure, cancellation during sample admission, retained live PCM/undo/
modulation state, and commit of shared/pinned samples. The registry snapshot
has independent records with preserved IDs/generations. No hardware session
replacement or recovery result is implied.

## Related

See [roadmap.md](../roadmap.md), [sequencer.md](sequencer.md) and
[track-and-patch-model.md](track-and-patch-model.md) for implementation order
and the full target ownership model.

### Melodic Pattern compatibility

Project schema 1.3 writes four note/velocity/gate lanes in each Pattern step
and a melodic bit in each Pattern-row flag. Older Pattern chunks decode their
20-byte records as drum rows with empty lanes. Track allocation settings retain
the schema 1.2 encoding. The file-size limit is 8 MiB; full-capacity Project
round trips and per-Advance I/O bounds are host-tested. Two live transaction
buffers can consume about 10.6 MiB of the shared SDRAM arena; allocation failure
must preserve the current session rather than evicting its dependencies.

### Sample loop crossfade (schema 1.4)

Project 1.4 retains each sample's bounded playback `loop_crossfade_ms` in the
versioned sample-edit record. Earlier sample chunks must have the former
reserved bytes zero and restore crossfade off. Runtime IDs and PCM remain
outside the edit record. Recall applies the captured setting before preparing
voice snapshots and does not adopt a newer external sidecar. See
[stereo sample editing](offline-sample-editing.md#stereo-markers-seam-checks-and-playback-crossfade)
for overlap and loop-period semantics.
