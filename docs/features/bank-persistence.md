# Bank persistence

The Bank container and index codec are the storage foundation for Phase 2.5.
The codec, index and SD file transactions are host-tested. The Bank page,
working-copy session owner, Track recall, preload and MIDI Program Change
handling remain unimplemented.

## Contents

- [Model and format](#model-and-format)
- [Foreground transactions](#foreground-transactions)
- [SD transaction adapter](#sd-transaction-adapter)
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
latency still needs measurement before UI/Program Change integration.

Saves calculate the resulting sparse-file size and call the existing free-space
admission helper before creating directories or files. They use a unique
exclusive temporary file, check both file closes, then publish under the new
name. Existing destinations and unowned temporaries are preserved. Cancellation
and failure remove only the temporary owned by that job. FatFs requires files
to be closed before [rename](https://elm-chan.org/fsw/ff/doc/rename.html), and
[read-only seeks](https://elm-chan.org/fsw/ff/doc/lseek.html) can clip at EOF;
the adapter verifies the actual position before reading a slot.

Index/read success never installs a Track or replaces a live Bank. The future
session owner publishes successful results, manages dirty working copies and
sample admission, and schedules this job alongside Project/Instrument/card
operations. This adapter is compiled into the device build but has no UI or
console entry point yet; it has not been run against physical SD storage.

## Validation and remaining work

Eight codec tests cover sparse and full-capacity Banks, both oscillator maps,
modulator persistence, slot boundaries, unknown chunks, malformed files,
duplicate slots, terminal write errors, bounded serialized copies and empty-Bank
index replacement. Seven SD-adapter tests cover sparse store/copy/clear and
selected-slot reads, source preservation, free-space/query failure, exclusive
publication, short writes, close/rename failures, cancellation, malformed input
and failed reads using the byte-backed FatFs mock.
No SD durability, recall latency or MIDI behavior is established by these
tests.

Device integration and its [physical gate](../hardware-validation.md#hv-016--bank-sd-transactions)
follow the expanded Instrument engine in the
[roadmap](../roadmap.md). It must provide a working Bank/index owner, explicit
Track replacement for UI recall, channel-routed Program Change recall,
Pool admission and the Bank page.

## Related

- [Track, Instrument and Bank ownership](track-and-patch-model.md)
- [Project persistence](project-persistence.md)
- [Instrument model](instrument-model.md)
