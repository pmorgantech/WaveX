# Bank persistence

Bank Manager, its foreground session owner and selected-Track recall are
implemented for Phase 2.5. The codec, SD transactions, staged recall and UI are
host-tested and both firmware images compile. Physical SD/audio acceptance is
still open in HV-016. A sparse two-board HIL regression passed on 2026-09-18;
explicit Bank sample preload and channel-routed MIDI Program Change recall are
implemented. Electrical MIDI and full-load timing remain hardware gates.

## Contents

- [Model and format](#model-and-format)
- [Foreground transactions](#foreground-transactions)
- [SD transaction adapter](#sd-transaction-adapter)
- [Bank Manager and Track recall](#bank-manager-and-track-recall)
- [Sample preload](#sample-preload)
- [MIDI Program Change recall](#midi-program-change-recall)
- [Validation and remaining work](#validation-and-remaining-work)
- [Related](#related)

## Model and format

A Bank owns 128 stable numbered slots. A slot is empty or contains one embedded
WXI Instrument, including both oscillator maps, envelopes, LFO settings and
modulation records. Instruments reference samples by card path. A Track owns
its own editable copy after recall; editing it does not change the Bank.

[bank_file.hpp](../../firmware/shared/wxcf/bank_file.hpp) defines WXCF file
type 7, version 1.0. HEAD holds the Bank name. Each populated slot has a
versioned chunk containing name/tags metadata followed by the complete WXI
stream. The codec reuses the WXI writer and reader rather than defining
another Instrument encoding.

The resident index contains names, tags, offsets and lengths and occupies
4,632 bytes. Empty slots remain empty; slot numbers never compact on save.
A full Bank of 128 Instruments with 32 zones on each of two oscillators and
the current 16-byte LFO records encodes to 2,602,028 bytes. Files are capped
at 4 MiB and each embedded Instrument at 64 KiB. WXI readers retain backward
compatibility with legacy 15-byte LFO records by defaulting the appended
pitch-follow field to zero.

## Foreground transactions

IndexDecoder scans at most 128 bytes per Advance call and rejects duplicate
slots, invalid framing, truncated payloads, metadata errors and unsupported
major versions. Unknown chunks are skipped in bounded slices. Only publish
the private index after Done.

Index loading validates framing and embedded WXI headers, not every
Instrument body. ReadSlot validates the selected WXI through a bounded stream
that ends at that slot's boundary; it cannot read into another Instrument.
Names and tags must match the index metadata. Decode into private scratch
and validate sample admission before changing a Track.

Encoder writes one complete Instrument per Append and reports success only
after Finish. Unlike the cooperative index scan, Append is not a per-record
yielding operation. BeginCopy/CopyNext preserve the serialized WXI bytes of
unchanged slots, including embedded extensions, in at most 128-byte slices.
Incomplete copies cannot finish successfully. The SD adapter owns temporary
files, checked close/rename, serialized slot copies and failure cleanup. All of this runs in the foreground; no Bank I/O
belongs in the callback.

## SD transaction adapter

[`BankFileJob`](../../firmware/daisy/src/storage/bank_file_job.hpp) supports
creating an empty named `.wxb`, copying an existing Bank to a new name with
an optional stored/cleared slot, scanning its index, and decoding a selected
Instrument into private scratch. Slot numbers remain stable. It holds one
index and two FatFs handles; it never allocates 128 Instrument documents.

The caller owns card-job serialization and keeps the source file and borrowed,
validated WXI document immutable until completion. Put the job in AXI SRAM
because its FatFs sector windows must be DMA accessible. Read/write adapters
transfer less than a sector; unchanged slot copies and index scans advance in
128-byte slices. New Instrument encoding and selected-slot decoding process
one bounded WXI document per foreground pump, so their worst-case service
latency still needs physical measurement before this integration is accepted
for performance use. Sparse UI/preload/Program Change timings are recorded in
HV-016; they do not bound a full Bank or the largest Instrument.

Saves calculate the resulting sparse-file size and call the existing free-space
admission helper before creating directories or files. They use a unique
exclusive temporary file, check both file closes, then publish under the new
name. Existing destinations and unowned temporaries are preserved. Cancellation
and failure remove only the temporary owned by that job. FatFs requires files
to be closed before [rename](https://elm-chan.org/fsw/ff/doc/rename.html), and
[read-only seeks](https://elm-chan.org/fsw/ff/doc/lseek.html) can clip at EOF;
the adapter verifies the actual position before reading a slot.

Index/read success alone never installs a Track or replaces a live Bank.
The foreground session publishes completed operations and serializes this job
with Project, Instrument, Pattern and card operations.

## Bank Manager and Track recall

Project → Shift → Banks opens the Manager. Previous/Next or encoder rotation
selects one of 128 stable slots. Recall targets the globally selected Track.
Shift exposes Open, New, Save copy and Clear copy. Preload loads Bank samples. Enter a saved Bank's
name to open it, or a new destination name to create/store/clear/save a copy.
The initial UI opens files by name; it has no file-list or slot-grid browser.

[`BankSession`](../../firmware/daisy/src/storage/bank_session.hpp) owns one active
index and saved Bank name. This first version uses immutable named files:
Store copy snapshots the selected Track into a slot of a new Bank, Clear copy
empties that slot in a new Bank, and Save copy duplicates the active file.
Only a successful write followed by an index reload changes the active Bank.
There is no unsaved Bank working copy. Editing a recalled Track never updates
the Bank; explicitly Store copy to keep those edits. Embedded Instrument labels
may contain punctuation such as an imported `.sfz` name; the stricter safe-name
rule applies to Bank filenames.

Recall, Store copy and Clear copy require UI confirmation. Recall warns that
the target Track's current Instrument and unsaved sound edits will be replaced.
Cancellation before submission changes nothing. Changing Track or Bank revision
cancels the draft; stale/disconnected status disables submission. Requests carry
an identity and expected Bank revision, and reconnect never replays a mutation.
There is no UI cancellation after a card operation starts.

Recall decodes one WXI into allocator-backed SDRAM, clones the Pool ownership
table, retains every other Track's ownership bits and loads dependencies into
private candidate state. Missing/invalid samples or allocation failures leave
the live Track and Pool untouched and release newly staged PCM. On success,
the foreground stops only the target Track through the existing audio fence,
installs its Instrument and commits Pool ownership before republishing prepared
voices. Track mix and MIDI routing remain unchanged. A refused stop preserves
the old Instrument. The callback performs no Bank I/O or allocation.

Resident sequencing and MIDI clock continue while the foreground job runs.
Sample audition closes and competing edits/new note requests are held off;
note-off and clock/transport handling remain available. Worst-case foreground
service latency and audible continuity require the physical checks below.

## Sample preload

Preload loads dependencies from every occupied slot of the active Bank. It
ignores the selected slot and name field, and needs no replacement confirmation.
It uses the same private loader lease and admission limits as recall, with one
WXI document at a time in allocator-backed scratch. It does not replace any
Instrument, change editor revisions or stop Tracks.

The candidate Pool retains all live records and Track ownership. Existing paths
are reused after the loader validates their file dimensions; missing PCM is
loaded privately. Every Bank dependency is pinned on successful completion,
including samples already resident. Pins are explicit user residency requests,
not Bank ownership: opening another Bank does not unpin them. Unload samples
through the Pool when they are no longer needed. Pins retain residency for this
session; they do not make Bank-path restoration automatic on reboot.

Only after all occupied slots succeed does the foreground publish the additive
Pool metadata. No old PCM is retired, so no audio stop fence is required. A
missing/unsupported dependency, card error or memory refusal discards all newly
staged PCM and pin changes, preserving live Tracks and the original Pool. The
frontend invalidates its Pool cache once for the successful completion.

Competing card/edit requests remain gated for the whole operation, while resident
sequencing and note-off/clock handling continue. There is no in-flight cancel.
The existing selected-document reader rescans the Bank index for each occupied
slot; dense Banks therefore require separate total-time and responsiveness
measurements. Preload removes later PCM reads, but recall still reads/validates
the Instrument and WAV headers; it is not a guarantee of instant switching.

## MIDI Program Change recall

DIN and USB share the MIDI parser/forwarder. Raw program 0–127 selects Bank slot
1–128 on screen. The Daisy captures all Tracks whose MIDI input matches the
message channel (including Omni) and whose Program Change setting is enabled.
Project → Shift → **Program: On/Off** changes that setting for the selected
Track. New Tracks enable it; saved Projects retain their explicit value,
including older Projects saved with it off. MIDI Input Off never matches.

The MIDI event itself authorizes replacement, so it needs no dialog. The job
reads one Instrument and stages its dependencies once in private Pool state.
All non-target ownership bits remain live. Only after successful admission does
one audio stop fence acknowledge the complete target mask; each target receives
an independent copy of the Instrument, retaining its routing, mix and other
Track settings. Prepared sequencer maps are then republished. No Bank I/O,
allocation or Instrument copying occurs in the callback.

A missing/invalid dependency, memory refusal or refused stop keeps every target
Instrument and live Pool unchanged. No Bank or an empty program reports the
existing Bank error. An invalid/unmatched event or any event arriving during a
Bank/Project/Instrument/Pattern/card operation is ignored, with **no queued
recall or automatic retry**. Send again after the operation finishes. New notes
remain gated during staging; note-offs, resident sequencing and clock handling
continue. Send Program Changes ahead of the notes that need the new sound.
Preload reduces PCM work but does not eliminate card/header reads or guarantee
instant switching. Bank Select remains deferred.

Program Change outcomes use retained Bank status with the distinct
`BANK_PROGRAM_RECALL` operation and a foreground-generated request ID. The Bank
page displays the last observed MIDI outcome. Completion identity includes the
operation, so MIDI results cannot acknowledge a coincident UI request ID. The
same program sent again is a new recall, allowing stored sound restoration.

## Validation and remaining work

Eight codec tests cover sparse and full-capacity Banks, both oscillator maps,
modulator persistence, slot boundaries, unknown chunks, malformed files,
duplicate slots, terminal write errors, bounded serialized copies and empty-Bank
index replacement. Seven SD-adapter tests cover sparse store/copy/clear and
selected-slot reads, source preservation, free-space/query failure, exclusive
publication, short writes, close/rename failures, cancellation, malformed input
and failed reads using the byte-backed FatFs mock.
Host tests establish no SD durability, recall latency or MIDI behavior.
The two-board Bank HIL case and its timing readback are described below.

Session tests cover private Track copies, preserved routing/shared sample
ownership, newly admitted PCM rollback, missing dependencies, full media,
stale/busy requests and explicit replacement confirmation. Preload tests cover
shared/new dependencies, preserved unpinned records, late-failure rollback,
reserve-aware memory refusal, repeated/empty Banks and all 128 slots with both
full oscillator maps. MIDI cases cover matched/Omni/Off routing, opt-out,
all 16 targets, busy rejection, repeat events, independent copies and rollback
for missing dependencies or an unacknowledged target stop. Wire/dispatch tests
reject malformed messages; real-LVGL tests cover confirmation, Track changes,
stale/offline status, stable slot selection, no mutation replay and idle rendering.

`tests/hil/test_bank_files.py` exercises the Manager through the real UI/UART/SD
path: sparse Store/Save/Clear copies, Open, cancellation, LFO recall, preserved
Track routing/mix, shared and newly admitted samples, duplicate destinations and
missing sources, plus preload with cold/shared dependencies and an unrelated
held voice. It also injects Program Change through the frontend parser/forwarder,
verifies matching and Omni targets, opt-out, empty slots and repeated recall.
This injection does not validate DIN/USB electrical behavior. It retains per-operation `BANKSTATS` and UI wall times in JUnit.
See [the bench command](../testing_guide.md#bank-files-and-track-recall).

`BANKSTATS` is debug-only foreground telemetry. Each accepted job resets its
request/op, pump count, maximum pump wall time and summed pump wall time in
microseconds. It reads the raw TIM2 counter with wrap-safe subtraction around
each pump; interrupt preemption and blocking SD time are included. Summed work
saturates at UINT32_MAX. UI/poll/confirmation time is separate. These are not
callback DWT, CPU utilization, analog continuity or MIDI wire-jitter results.
Timing instrumentation compiles out when the debug harness is disabled.

The [physical gate](../hardware-validation.md#hv-016--bank-sd-transactions)
remains open. Next software work is slot-to-slot copy/move and a decision on deferred
unsaved Bank working copies. Project save/load does not yet persist the selected
Bank path; reopen it by name after reboot. See the [roadmap](../roadmap.md).

## Related

- [Track, Instrument and Bank ownership](track-and-patch-model.md)
- [Project persistence](project-persistence.md)
- [Instrument model](instrument-model.md)
