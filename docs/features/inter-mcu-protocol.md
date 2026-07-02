# Inter-MCU Protocol — As-Built Wire Specification

**Status**: As-built reference for `firmware/shared/spi_protocol/protocol.h` (PROTOCOL_VERSION 1).
**Rule**: `protocol.h` is the contract. This document explains it; if they diverge, fix one of them in the same commit that changed the other. Every message type must have a round-trip test in `firmware/shared/tests/`.
**Supersedes**: `archive/communication-protocol.md` (described an ESP32-S3/ESP-master/0xAA-sync design that was never what shipped).

## 1. Physical link

| Property | Value |
|---|---|
| Transport | SPI, mode 0 (CPOL=0, CPHA=0), 8-bit, MSB first |
| **Master** | **Daisy Seed** (SPI1, software CS on D7, pins in `pin_config.h`) |
| **Slave** | **ESP32-P4** (`SPI3_HOST`, DMA via `spi_slave` driver, queue depth 8) |
| Clock | SPI1 kernel clock ÷ 16 (`PS_16`, conservative bring-up setting; raising it is roadmap Phase 1.4) |
| ATTN line | ESP32 GPIO31 → Daisy D0, active high: "slave has TX data queued, please clock a transaction" |
| CRC | CRC16-CCITT over the whole packet except the CRC field; STM32 hardware CRC peripheral with software fallback |

Because the slave can only talk when clocked, the duplex model is: the Daisy clocks transactions when (a) it has data to send, (b) ATTN is asserted, or (c) on a periodic poll. The ESP32 can also explicitly pull queued data with `MSG_DATA_REQUEST`.

## 2. Packet framing

```
struct WaveXPacket {           // packed
    uint8_t  flags_size;       // low nibble: size code, high nibble: flags
    uint8_t  msg_type;         // MessageType
    uint16_t seq;              // sequence number
    uint8_t  payload[...];     // structured message (zero-padded to packet size)
    uint16_t crc;              // CRC16-CCITT, at the END of the fixed-size packet
};
```

- **Size codes** (low nibble): 0→32 B, 1→64, 2→128, 3→256, 4→512, 5→1024, 6→2048 total packet size. Fixed power-of-two transaction lengths keep DMA slave buffer management trivial on the ESP32. `ProtocolHandler::GetOptimalSizeCode()` picks the smallest class that fits.
- **Flags** (high nibble): `PKT_FLAG_ACK` (0x80), `PKT_FLAG_NACK` (0x40 — corrupted, resend), two reserved.
- `MAX_PAYLOAD_SIZE` = 220 B for single-struct messages; larger structured payloads (browse responses, wave chunks) use the bigger size classes.

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
| MSG_ERROR | 0xFF | both | `ErrorMessage{code, msg[48]}` | error report |

## 4. Conventions & invariants

1. **All payload structs are `__attribute__((packed))` and fixed-layout.** Never reorder fields; append only, or bump `PROTOCOL_VERSION`.
2. **Every payload struct has a named constructor** (`Type(field1, field2, ...)`) plus a zero-initializing default constructor, and no other constructors — this makes the type a non-aggregate, so `Type x = {a, b, c};` / designated-initializer construction is a **compile error**, not just a style rule (field-order bugs have bitten before — see `archive/ARCHITECTURE_ASSESSMENT_20260626.md`). Build with the named constructor (`Type x(a, b, c);`); reserved/padding fields are not constructor parameters and are always zeroed internally. `SampleMemStatusMessage` additionally has `AddEntry()` for bounds-checked appends to its fixed `entries[]` array. When adding a new field, update the constructor's parameter list (and every call site the compiler then flags) in the same commit.
3. **String fields** are fixed-size, null-terminated, `FILE_NAME_MAX=48`, `BROWSE_PATH_MAX=96` (path response uses 200).
4. **Flow control**: packet statistics (per-type counters, CRC error counts) are tracked on both sides; NACK triggers resend; a stuck TX queue must self-recover within 1 s (regression requirement from the UART-era hang).
5. **Nothing latency-critical rides the link**: audio never crosses it; note events do (from MIDI on the ESP32), so keep the note path under 5 ms end-to-end — this bounds acceptable polling cadence.

## 5. Planned extensions (design first, then implement — see roadmap)

- **Phase 2 (sequencer)**: pattern-edit ops, transport control, playhead/step feedback (coalesced), kit management.
- **Phase 4 (offline editing)**: render-job submit/progress/cancel, sidecar marker sync.
- Consider a generational "capabilities" handshake at boot (versions on both sides) before the first extension ships.
