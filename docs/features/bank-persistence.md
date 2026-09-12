# Bank persistence

The Bank container and index codec are the storage foundation for Phase 2.5.
They are host-tested; the Bank page, SD working copy, Track recall, preload
and MIDI Program Change handling remain unimplemented.

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
A full Bank of 128 Instruments with 32 zones on each of two oscillators
encodes to 2,601,772 bytes. Files are capped at 4 MiB and each embedded
Instrument at 64 KiB.

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
yielding operation. The future SD adapter owns scheduling, temporary files,
checked close/rename, serialized slot copies and failure cleanup. All of this
runs in the foreground; no Bank I/O belongs in the callback.

## Validation and remaining work

Six host tests cover sparse and full-capacity Banks, both oscillator maps,
modulator persistence, slot boundaries, unknown chunks, malformed files,
duplicate slots, terminal write errors and empty-Bank index replacement.
No SD durability, recall latency or MIDI behavior is established by these
tests.

Device integration follows the expanded Instrument engine in the
[roadmap](../roadmap.md). It must provide a working Bank/index owner, explicit
Track replacement for UI recall, channel-routed Program Change recall,
Pool admission and the Bank page.

## Related

- [Track, Instrument and Bank ownership](track-and-patch-model.md)
- [Project persistence](project-persistence.md)
- [Instrument model](instrument-model.md)
