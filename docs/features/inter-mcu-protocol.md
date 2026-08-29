# Inter-MCU Protocol — As-Built Wire Specification

**Status**: As-built reference for the live UART framing in `firmware/shared/uart_protocol/uart_protocol.h` and the shared payload catalog in `firmware/shared/spi_protocol/protocol.h` (PROTOCOL_VERSION 1).
**Rule**: `protocol.h` is the contract. This document explains it; if they diverge, fix one of them in the same commit that changed the other. Every message type must have a round-trip test in `firmware/shared/tests/`.
**Supersedes**: `archive/communication-protocol.md` (described an ESP32-S3/ESP-master/0xAA-sync design that was never what shipped).

## 1. Physical link

| Property | Value |
|---|---|
| Transport | UART, 8-N-1, full duplex, 2 Mbaud |
| Endpoints | ESP32-P4 UART1 ↔ Daisy UART4; pins live only in `pin_config.h` |
| Daisy RX | continuous circular DMA1 Stream 5 |
| Daisy TX | asynchronous DMA2 Stream 4, one staged frame + four-entry software queue |
| ESP32 | ESP-IDF legacy interrupt-driven UART with 8 KiB RX / 4 KiB TX rings |
| CRC | CRC16-CCITT over flags/type/sequence/payload framing bytes |

The Daisy streams RX and TX simultaneously through independent DMA streams; TX never waits for frame wire time in the main loop. The compiled-out SPI transport remains wired but `WAVEX_SPI_LINK_ENABLED=0`; its fixed-size `WaveXPacket` framing is dormant and is not the shipped wire format.

## 2. Packet framing

```
start(0xA5) | payload_len(u16 LE) | flags(u8) | msg_type(u8) |
sequence(u16 LE) | payload[0..2048] | crc16(u16 LE) | end(0x5A)
```

- Frame overhead is 10 bytes and maximum payload is 2048 bytes.
- Flags: `UART_FLAG_ACK` (0x80), `UART_FLAG_NACK` (0x40), priority and fragmentation bits reserved.
- `FrameScanner` resynchronizes on markers/length/CRC and `SequenceTracker` rejects duplicates or severe out-of-order frames while accepting a peer-reboot resync.

## 3. Message catalog (msg_type → payload struct)

| Type | Value | Direction | Payload | Purpose |
|---|---|---|---|---|
| MSG_SYNC | 0x00 | both | `SyncMessage{timestamp_ms}` | keepalive/resync |
| MSG_CONTROL_CHANGE | 0x01 | E→D | `ControlChangeMessage{param, channel, value}` | parameter set (see `ControlParameter` enum) |
| MSG_NOTE_ON / OFF | 0x02/0x03 | E→D | `NoteMessage{note, velocity, channel}` | note events (MIDI-shaped) |
| MSG_SAMPLE_LOAD | 0x04 | E→D | `SampleLoadMessage{sample_id, hints, path[96]}` | load sample from Daisy SD into sample RAM (path-based; metadata fields are hints, Daisy re-reads) |
| MSG_SAMPLE_DATA | 0x05 | E→D | raw chunk | sample bytes pushed from ESP32 (rare path; SD-local loads preferred) |
| MSG_PARAMETER_UPDATE | 0x06 | D→E | — | parameter echo/update |
| MSG_STATUS_REQUEST | 0x07 | E→D | `StatusRequestMessage{category}` | request status (`GENERAL`, `SAMPLE_MEM`) |
| MSG_STATUS_RESPONSE | 0x08 | D→E | e.g. `SampleMemStatusMessage` | status payload incl. sample-RAM allocator stats + up to 8 `SampleMemEntryMessage` |
| MSG_SAMPLE_CTRL | 0x09 | E→D | `SampleCtrlMessage{slot, cmd, rate}` | rec start/stop, play start/stop |
| MSG_PREVIEW_REQ | 0x0A | E→D | `PreviewReqMessage{slot, start, end, decim}` | request decimated waveform preview |
| MSG_DATA_REQUEST | 0x0B | E→D | `DataRequestMessage{request_type}` | slave pulls queued data (any/meter/wave) |
| MSG_METER_PUSH | 0x10 | D→E | `MeterPushMessage{rms L/R, peak L/R}` | level meters (20–50 ms cadence) |
| MSG_WAVE_CHUNK | 0x11 | D→E | `WaveChunkMessage{offset, count}` + int16[] | preview waveform data |
| MSG_HEARTBEAT | 0x12 | both | `HeartbeatMessage{uptime, rx_total, loop_counter, cpu avg/min/max ×10}` | health + CPU load telemetry |
| MSG_ACK | 0x13 | both | `AckMessage{serial_id}` | explicit ack of a sequence number |
| MSG_BROWSE_REQ / RESP | 0x30/0x31 | E→D / D→E | path + start_index + max_entries / `BrowseRespHeader` + `FileEntryWire[]` | paginated SD directory listing; entries carry WAV metadata (rate, channels, bits, duration_ms) |
| MSG_SAMPLE_PLAY_REQ | 0x32 | E→D | path string | audition by path |
| MSG_SAMPLE_STOP_REQ / RESP | 0x33/0x35 | E→D / D→E | `SampleStopReqMessage{slot}` / `SampleStopRespMessage{success}` | stop audition |
| MSG_SAMPLE_STATUS | 0x34 | D→E | `SampleStatusMessage{sample_id, state, ch, rate, frames}` | playback/load notifications (state 0x10 = load complete) |
| MSG_SAMPLE_PLAY_INDEX_REQ | 0x36 | E→D | `SamplePlayIndexMessage{index}` | audition by directory index |
| MSG_SAMPLE_GET_PATH_REQ / RESP | 0x37/0x38 | E→D / D→E | index / `SamplePathResponseMessage{index, path[200]}` | resolve index → full path |
| MSG_STORAGE_STATUS | 0x39 | D→E | `StorageStatusMessage{mounted, reserved[3]}` | **unsolicited**: SD mounted (1) or lost (0). The frontend has no view of the card slot and cannot poll for this, so ejection/insertion is only observable if the backend says so. On loss the backend also sends `MSG_SAMPLE_STOP_RESP` + an empty `MSG_BROWSE_RESP` so audition exits and the listing clears; on mount the browser re-lists its current path. |
| MSG_CV_CAL_SET | 0x40 | E→D | `CvCalMessage{group, persist, 7×float}` | apply one group's CV calibration; `persist=1` also writes the table to SD; Daisy replies with MSG_CV_CAL_RESP |
| MSG_CV_CAL_GET | 0x41 | E→D | `CvCalGetMessage{group}` | request one group's calibration |
| MSG_CV_CAL_RESP | 0x42 | D→E | `CvCalMessage` (persist unused) | reply to SET and GET |
| MSG_CV_TEST | 0x43 | E→D | `CvTestMessage{group, enable, cutoff, resonance, vca}` | calibration procedure: while enabled, the control tick stages these fixed CVs instead of the paraphonic law |
| MSG_SEQ_TRANSPORT | 0x50 | E→D | `SeqTransportMessage{command, clock_source, input_mode, quantize, tempo_bpm_x100, song_position}` | play/stop/continue, tempo, clock source (internal/MIDI), input mode (play/step-rec/live-rec/erase) — `sequencer.md` §4, `midi-sync-tempo-follower.md` §3 |
| MSG_SEQ_PATTERN_OP | 0x51 | E→D | `SeqPatternOpMessage{op, track, step, arg_u8, arg_u16, arg_s16}` | one small idempotent pattern edit; `op` (`SeqPatternOpCode`) selects which fields apply — see the table in `protocol.h` above the struct |
| MSG_SEQ_PATTERN_SYNC | 0x52 | both | *(reserved — struct not yet defined)* | bulk pattern read/write for project load/save (`sequencer.md` §4); deferred, project persistence uses the WXCF container on SD |
| MSG_SEQ_PLAYHEAD | 0x53 | D→E | `SeqPlayheadMessage{pattern, step, playing, sync_state, measured_bpm_x100, loop_count}` | coalesced playhead + sync-lock feedback for the UI (≤ 30 Hz) |
| MSG_MIDI_CLOCK_EVENT | 0x55 | E→D | `MidiClockEventMessage{event, source, tick_seq, esp_delta_us, spp_beats16}` | forwarded MIDI real-time/transport byte; `esp_delta_us` is the ESP-domain **delta** (never an absolute timestamp) so the tempo follower can't mix clock domains — `midi-sync-tempo-follower.md` §2/§3 |
| MSG_MIDI_CC | 0x56 | E→D | `MidiCcMessage{cc, value, channel}` | forwarded MIDI control change; Daisy owns the CC→mod-source map (`param-locks-and-modulation.md` §6) |
| MSG_SEQ_CLOCK_OUT | 0x57 | D→E | `SeqClockOutMessage{event, tick_seq, spp_beats16}` | Daisy-generated MIDI clock/transport for the ESP32 to serialize onto DIN + USB immediately |
| MSG_ERROR | 0xFF | both | `ErrorMessage{code, msg[48]}` | error report |

## 4. Conventions & invariants

1. **All payload structs are `__attribute__((packed))` and fixed-layout.** Never reorder fields; append only, or bump `PROTOCOL_VERSION`.
2. **Every payload struct has a named constructor** (`Type(field1, field2, ...)`) plus a zero-initializing default constructor, and no other constructors — this makes the type a non-aggregate, so `Type x = {a, b, c};` / designated-initializer construction is a **compile error**, not just a style rule (field-order bugs have bitten before — see `archive/ARCHITECTURE_ASSESSMENT_20260626.md`). Build with the named constructor (`Type x(a, b, c);`); reserved/padding fields are not constructor parameters and are always zeroed internally. `SampleMemStatusMessage` additionally has `AddEntry()` for bounds-checked appends to its fixed `entries[]` array. When adding a new field, update the constructor's parameter list (and every call site the compiler then flags) in the same commit.
3. **String fields** are fixed-size, null-terminated, `FILE_NAME_MAX=48`, `BROWSE_PATH_MAX=96` (path response uses 200).
4. **Flow control**: packet statistics (per-type counters, CRC error counts) are tracked on both sides; NACK triggers resend; a failed Daisy DMA frame retries and the queue self-recovers within 1 s.
5. **Nothing latency-critical rides the link**: audio never crosses it; note events do (from MIDI on the ESP32), so keep the note path under 5 ms end-to-end — this bounds acceptable polling cadence.

## 5. Planned extensions (design first, then implement — see roadmap)

Message-ID blocks are **reserved** for the 2026-07-05 feature-design suite — see the reservation table in `feature-expansion-ideas.md` (0x50–0x5F sequencer/clock/arp, 0x60–0x6F instrument/tuning, 0x70–0x7F recording/mix/scenes, 0xA0–0xAF render jobs). Do not assign new IDs outside that table without updating it.

- **Phase 2 (sequencer)**: pattern-edit ops, transport control, playhead/step feedback (coalesced), MIDI clock in/out (`midi-sync-tempo-follower.md`). Kit management is subsumed by instrument ops (`instrument-model.md` §8; 0x54 stays reserved-unused).
- **Phase 2.5**: instrument ops (0x60–0x62), recording (0x70/0x71), mixer (0x78/0x79), MIDI CC forward (0x56), arp (0x58).
- **Phase 4 (offline editing)**: render-job submit/progress/cancel (0xA0–0xA3), sidecar marker sync.
- **Phase 5**: scene apply (0x7A), tuning (0x68).
- Consider a generational "capabilities" handshake at boot (versions on both sides) before the first extension ships.
