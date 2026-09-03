# Inter-MCU Protocol — As-Built Wire Specification

**Status**: As-built reference for the live UART framing in `firmware/shared/uart_protocol/uart_protocol.h` and the shared payload catalog in `firmware/shared/spi_protocol/protocol.h` (PROTOCOL_VERSION 1).
**Rule**: `protocol.h` is the contract. This document explains it; if they diverge, fix one of them in the same commit that changed the other. Every message type must have a round-trip test in `firmware/shared/tests/`.
**Supersedes**: the former `communication-protocol.md`, which described an ESP32-S3 / ESP-master / 0xAA-sync design that was never what shipped. Deleted; see git history.

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
| MSG_CONTROL_CHANGE | 0x01 | E→D | `ControlChangeMessage{param, channel, value}` | parameter set (see `ControlParameter` enum). Live ids are 0x01–0x0A; 0x0B–0x15 are reserved for `param-locks-and-modulation.md` §1. `PARAM_LFO_RATE`/`PARAM_LFO_DEPTH` sit at 0x16/0x17 — they used to duplicate `PARAM_PAN`/`PARAM_PITCH` at 0x08/0x09 in the same enum, dead but one wiring-up away from a mis-route. |
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
| MSG_HEARTBEAT | 0x12 | both | `HeartbeatMessage{uptime, rx_total, loop_counter, cpu avg/min/max ×10}` | health + CPU load telemetry. `uptime` is the **only** source of backend uptime and needs no diagnostics subscription — the Diagnostics Daisy tab reads it from here rather than from `MSG_DIAG_PUSH`, so there is one copy of the number and one cadence for it. |
| MSG_ACK | 0x13 | both | `AckMessage{serial_id}` | explicit ack of a sequence number |
| MSG_BROWSE_REQ / RESP | 0x30/0x31 | E→D / D→E | path + start_index + max_entries / `BrowseRespHeader` + `FileEntryWire[]` | paginated SD directory listing; entries carry WAV metadata (rate, channels, bits, duration_ms) |
| MSG_SAMPLE_PLAY_REQ | 0x32 | E→D | path string | audition by path |
| MSG_SAMPLE_STOP_REQ / RESP | 0x33/0x35 | E→D / D→E | `SampleStopReqMessage{slot}` / `SampleStopRespMessage{success}` | stop audition |
| MSG_SAMPLE_STATUS | 0x34 | D→E | `SampleStatusMessage{sample_id, state, ch, rate, frames}` | playback/load notifications (state 0x10 = load complete) |
| MSG_SAMPLE_PLAY_INDEX_REQ | 0x36 | E→D | `SamplePlayIndexMessage{index}` | audition by directory index |
| MSG_SAMPLE_GET_PATH_REQ / RESP | 0x37/0x38 | E→D / D→E | index / `SamplePathResponseMessage{index, path[200]}` | resolve index → full path |
| MSG_STORAGE_STATUS | 0x39 | D→E | `StorageStatusMessage{mounted, reserved[3]}` | **unsolicited**: SD mounted (1) or lost (0). The frontend has no view of the card slot and cannot poll for this, so ejection/insertion is only observable if the backend says so. On loss the backend also sends `MSG_SAMPLE_STOP_RESP` + an empty `MSG_BROWSE_RESP` so audition exits and the listing clears; on mount the browser re-lists its current path. |
| MSG_DIAG_SUBSCRIBE | 0x3A | E→D | `DiagSubscribeMessage{enable, interval_hz, reserved[2]}` | start/stop the telemetry push. Subscription-gated on purpose: the push flows only while the diagnostics page is open, so it costs nothing the rest of the time. `interval_hz` is clamped 1–10 by the backend. |
| MSG_DIAG_PUSH | 0x3B | D→E | `DiagPushMessage` (102 B → `PKT_SIZE_128`) | **unsolicited** while subscribed: one interval of audio/storage/link/MIDI telemetry plus the backend's system-heap level. Counters are **deltas over `interval_ms`, reset on read**; absolute values only for levels and states. At 2 Hz this is ~216 B/s against a 200 KB/s link — under 0.15%. |
| MSG_SAMPLE_EDIT_SET | 0x3C | E→D | `SampleEditMessage{slot, loop_enabled, gain_db_x10, start_frame, end_frame, loop_start, loop_end, fade_in_ms, fade_out_ms}` | non-destructive playback-edit command (roadmap 1.5.1). Sentinel `end_frame`/`loop_end` 0 = end of file/region. This is the **command**; the backend clamps and never assumes these values were taken verbatim — its reply is the authoritative `MSG_SAMPLE_META`. |
| MSG_SAMPLE_META | 0x3D | D→E | `SampleMetadata{sample_id, generation, sample_rate, total_frames, start_frame, end_frame, loop_start, loop_end, gain_db_x10, fade_in_ms, fade_out_ms, channels, bits_per_sample, loop_enabled, channel_mode, flags, reserved, name[48]}` | authoritative per-sample record (roadmap 1.5.5 item 1), owned by the Daisy and pushed on every change — geometry, edit markers, fades, and load/resident state in one place so playback and display paths cannot disagree. `generation` bumps only on a content change (destructive render), not on marker edits, so a waveform cache keyed on (sample_id, generation) survives edits. |
| MSG_SAMPLE_META_REQ | 0x3E | E→D | `SampleMetaReqMessage{sample_id}` | request a metadata resend; `sample_id` 0 means "every loaded sample" (how the frontend repopulates after its own restart) |
| MSG_ENVELOPE_REQ | 0x3F | E→D | `EnvelopeReqMessage{sample_id, columns, start_frame, end_frame}` | request a min/max envelope for a frame window (roadmap 1.5.5 item 2), not decimated samples — avoids the aliasing `MSG_PREVIEW_REQ`/`MSG_WAVE_CHUNK` are prone to. `sample_id` 0 = most recently loaded; `columns` clamped to `MAX_ENVELOPE_COLUMNS` (1280) |
| MSG_CV_CAL_SET | 0x40 | E→D | `CvCalMessage{group, persist, 7×float}` | apply one group's CV calibration; `persist=1` also writes the table to SD; Daisy replies with MSG_CV_CAL_RESP |
| MSG_CV_CAL_GET | 0x41 | E→D | `CvCalGetMessage{group}` | request one group's calibration |
| MSG_CV_CAL_RESP | 0x42 | D→E | `CvCalMessage` (persist unused) | reply to SET and GET |
| MSG_CV_TEST | 0x43 | E→D | `CvTestMessage{group, enable, cutoff, resonance, vca}` | calibration procedure: while enabled, the control tick stages these fixed CVs instead of the paraphonic law |
| MSG_ENVELOPE_CHUNK | 0x44 | D→E | `EnvelopeChunkMessage{sample_id, generation, start_frame, end_frame, total_columns, first_column, columns, channels, reserved}` + `EnvelopeColumn{min_sample, max_sample}[columns × channels]`, channel-interleaved per column | reply to `MSG_ENVELOPE_REQ`; self-describing (repeats the whole window + generation) so a frontend that missed a chunk or has since moved the view can tell without keeping request state |
| MSG_SAMPLE_SELECT | 0x45 | E→D | `SampleSelectMessage{sample_id, slot, reserved}` | binds `sample_id` for playback on `slot` (0..15, matches `MSG_NOTE_ON`'s channel & 0x0F); `sample_id` 0 clears that slot's binding, so its note-on drops rather than falling back to any other slot's or the most-recently-loaded sample (roadmap Phase 2.5 item 1, "retire the fallback" - the any-channel single-selection behaviour this replaced) |
| MSG_SAMPLE_UNLOAD | 0x46 | E→D | `SampleUnloadMessage{sample_id}` | frees a loaded sample's RAM; voices sounding from it are stopped first. `sample_id` 0 is rejected, not treated as "unload everything" |
| MSG_SEQ_TRANSPORT | 0x50 | E→D | `SeqTransportMessage{command, clock_source, input_mode, quantize, tempo_bpm_x100, song_position}` | play/stop/continue, tempo, clock source (internal/MIDI), input mode (play/step-rec/live-rec/erase) — `sequencer.md` §4, `midi-sync-tempo-follower.md` §3 |
| MSG_SEQ_PATTERN_OP | 0x51 | E→D | `SeqPatternOpMessage{op, track, step, arg_u8, arg_u16, arg_s16}` | one small idempotent pattern edit; `op` (`SeqPatternOpCode`) selects which fields apply — see the table in `protocol.h` above the struct |
| MSG_SEQ_PATTERN_SYNC | 0x52 | both | *(reserved — struct not yet defined)* | bulk pattern read/write for project load/save (`sequencer.md` §4); deferred, project persistence uses the WXCF container on SD |
| MSG_SEQ_PLAYHEAD | 0x53 | D→E | `SeqPlayheadMessage{pattern, step, playing, sync_state, measured_bpm_x100, loop_count}` | coalesced playhead + sync-lock feedback for the UI (≤ 30 Hz) |
| MSG_MIDI_CLOCK_EVENT | 0x55 | E→D | `MidiClockEventMessage{event, source, tick_seq, esp_delta_us, spp_beats16}` | forwarded MIDI real-time/transport byte; `esp_delta_us` is the ESP-domain **delta** (never an absolute timestamp) so the tempo follower can't mix clock domains — `midi-sync-tempo-follower.md` §2/§3 |
| MSG_MIDI_CC | 0x56 | E→D | `MidiCcMessage{cc, value, channel}` | forwarded MIDI control change; Daisy owns the CC→mod-source map (`param-locks-and-modulation.md` §6) |
| MSG_SEQ_CLOCK_OUT | 0x57 | D→E | `SeqClockOutMessage{event, tick_seq, spp_beats16}` | Daisy-generated MIDI clock/transport for the ESP32 to serialize onto DIN + USB immediately |
| MSG_INST_OP | 0x60 | E→D | `InstOpMessage{request_id, slot, op, path[96], mod_slot_index, mod_source, mod_dest, mod_depth, mod_curve, mod_flags}` | `op` (`InstOpCode`) selects the shape: `INST_OP_SFZ_PROBE`/`INST_OP_SFZ_LOAD` inspect or load an SFZ instrument (`path` set, `mod_*` unused, `request_id` rejects stale selection replies); `INST_OP_SET_MOD_SLOT` writes one of `slot`'s 8 modulation-matrix slots (`path` unused, `mod_*` fields mirror `mod_matrix.hpp`'s `ModSlot` field-for-field — `param-locks-and-modulation.md` §3/§9 stage 4) |
| MSG_INST_STATUS | 0x61 | D→E | `InstStatusMessage{request_id, slot, op, state, flags, error, zone/sample counts, byte totals/progress, current_name[48]}` | preflight result plus total/current-WAV load progress; flags report missing/invalid WAVs and insufficient resident memory |
| MSG_INST_ZONE_SYNC | 0x62 | both | *(reserved — struct not yet defined)* | future editable-zone synchronization |
| MSG_MIX_OP | 0x78 | E→D | `MixOpMessage{op, track, value}` | one mixer control change. `value` is op-dependent: gain/master are **centi-dB above the −60 dB floor** (0 = silence, 6000 = 0 dB, 6600 = +6 dB); pan reuses PARAM_PAN's convention (0 left, 32768 centre, 65535 right); `SET_MUTE_MASK` carries a bit per track. Conversions live in `WaveX::Mix` (`shared/audio/track_mix.hpp`) so both ends use one implementation |
| MSG_MIX_METERS | 0x79 | D→E | `MixMetersMessage{peak[16]}` | per-track peak, log-mapped by `Mix::PeakToMeterByte` with 0 reserved for true silence. Sent only between `SUB_METERS` and `UNSUB_METERS`, at the existing meter cadence; master stereo meters stay on MSG_METER_PUSH |
| MSG_ERROR | 0xFF | both | `ErrorMessage{code, msg[48]}` | error report |

## 4. Conventions & invariants

1. **All payload structs are `__attribute__((packed))` and fixed-layout.** Never reorder fields; append only, or bump `PROTOCOL_VERSION`.
2. **Every payload struct has a named constructor** (`Type(field1, field2, ...)`) plus a zero-initializing default constructor, and no other constructors — this makes the type a non-aggregate, so `Type x = {a, b, c};` / designated-initializer construction is a **compile error**, not just a style rule (field-order bugs have bitten before — found in the 2026-06-26 external architecture assessment). Build with the named constructor (`Type x(a, b, c);`); reserved/padding fields are not constructor parameters and are always zeroed internally. `SampleMemStatusMessage` additionally has `AddEntry()` for bounds-checked appends to its fixed `entries[]` array. When adding a new field, update the constructor's parameter list (and every call site the compiler then flags) in the same commit. **One deliberate exception:** `DiagPushMessage` has only the zeroing default constructor. The named constructor exists to force call sites to be re-checked when a field moves; with ~40 telemetry fields filled one at a time by a collector, a 40-argument constructor would be unreadable and would not achieve that. Its fields are assigned by name instead, which fails loudly on a rename and is immune to reordering — and its round-trip test sets every field to a distinct value so a swap of two same-width neighbours is caught.
3. **String fields** are fixed-size, null-terminated, `FILE_NAME_MAX=48`, `BROWSE_PATH_MAX=96` (path response uses 200).
4. **Flow control**: packet statistics (per-type counters, CRC error counts) are tracked on both sides; NACK triggers resend; a failed Daisy DMA frame retries and the queue self-recovers within 1 s.
5. **Nothing latency-critical rides the link**: audio never crosses it; note events do (from MIDI on the ESP32), so keep the note path under 5 ms end-to-end — this bounds acceptable polling cadence.

## 5. Planned extensions (design first, then implement — see roadmap)

Message-ID blocks are reserved: 0x50–0x5F for sequencer/clock/arp, 0x60–0x6F
for instruments/tuning, 0x70–0x7F for recording/mix/scenes, and 0xA0–0xAF for
render jobs. Do not assign a new ID outside these blocks without updating this
document and `protocol.h`.

- **Phase 2 (sequencer)**: pattern-edit ops, transport control, playhead/step feedback (coalesced), MIDI clock in/out (`midi-sync-tempo-follower.md`). Kit management is subsumed by instrument ops (`instrument-model.md` §8; 0x54 stays reserved-unused).
- **Phase 2.5**: editable zone sync (0x62), recording (0x70/0x71), and arp (0x58). The mixer ops at 0x78/0x79 are now defined and round-trip tested, though nothing drives them yet — the engine application and the mixer page are the next two stages of `output-routing-and-mixer.md` §6. SFZ probe/load uses the now-live instrument ops at 0x60/0x61; MIDI CC forwarding at 0x56 is also live. `INST_OP_SET_MOD_SLOT` (also on 0x60) is now live end to end (ESP32 `inter_mcu_send_mod_slot()` → Daisy `SfzLoader::SetModSlot()`, instrument-scoped storage on `Instrument::mod_slots`) — no UI sends it yet (`param-locks-and-modulation.md` §7/§9 stage 5). `SRC_MODWHEEL`/`SRC_AFTERTOUCH` still read 0: `MSG_MIDI_CC` reaches the Daisy but nothing feeds it into the mod matrix's `ModSources`, and the ESP32 MIDI task still drops incoming CC/aftertouch rather than forwarding it (§9 stage 4, second half).
- **Phase 4 (offline editing)**: render-job submit/progress/cancel (0xA0–0xA3), sidecar marker sync.
- **Phase 5**: scene apply (0x7A), tuning (0x68).
- Consider a generational "capabilities" handshake at boot (versions on both sides) before the first extension ships.
