# Project persistence

**Status:** Project data model and cooperative WXCF codec are host-tested.
The cooperative SD file job and isolated Instrument/Pool load staging are
implemented and host-testable; session capture, restore orchestration,
project selection and song playback are not connected
yet. This is the persistence foundation for Phase 2, not a completed save/load
workflow or power-loss guarantee.

## Contents

- [Ownership and contents](#ownership-and-contents)
- [File transaction](#file-transaction)
- [Isolated load staging](#isolated-load-staging)
- [Remaining device work](#remaining-device-work)
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
same WAV in every Project. The coordinator must capture every referenced
sample and restore edits before preparing voice maps; the adapters alone do
not make the current Sample Edit screen persistent.

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
occupies roughly 3.5 MiB: never construct it on either MCU's stack or make it
callback-visible. Device integration must reserve foreground scratch
exclusively and release it when the transaction ends. Playback retains its
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
must retain its scratch until completion. This adapter does not yet capture
Instruments or provide a user-visible Project save/load operation.

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
These primitives are not yet a user-visible or automatically orchestrated
Project load operation.

## Remaining device work

1. Reserve scratch and coordinate Pattern capture, Track edits and Instrument
   export under one foreground transaction owner.
2. Connect Instrument snapshots to the session transaction. The loader's
   `BeginProjectSnapshot` now exports a Project-owned WXI without renaming the
   live Instrument, changing its editor revision, or consuming its Revert
   point. Its parent directory must be owned/created by the session job. It
   checks free space and on-card WAV dependencies, preserves existing files,
   and returns an internal result without an unrelated editor completion.
   References alone do not preserve an edited live Instrument. Apply the same on-card WAV
   format and per-sample admission preflight as
   [Instrument Save copy](instrument-model.md#5-persistence) before publishing
   snapshots; direct-load Pool residency does not prove recall admission.
3. Connect isolated Instrument/Pool staging to the session coordinator,
   including the audio stop fence, prepared-map publication and Bank-reference
   preflight. Report failures without committing a partial Performance.
4. Connect SD save/load, authoritative status and touchscreen selection.
5. Verify reboot restore and interrupted writes on hardware; integrate Song
   selection/playback separately from file decoding.

## Validation

Host tests cover sparse and full-capacity round trips, hidden steps and locks,
Track mix/routing, Song references, bounded I/O, malformed/truncated files,
unknown chunks, duplicate chunks, future major versions and write failures.
They do not establish SD durability, atomic session replacement, audible
continuity or power-loss recovery.

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
