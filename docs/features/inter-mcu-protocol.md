# Inter-MCU Protocol — As-Built Wire Specification

**Status**: As-built reference for the live UART framing in `firmware/shared/uart_protocol/uart_protocol.h` and the shared payload catalog in `firmware/shared/spi_protocol/protocol.h` (PROTOCOL_VERSION 4).
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

The Daisy streams RX and TX simultaneously through independent DMA streams; TX never waits for frame wire time in the main loop. The shared selector defaults to UART. The opt-in SPI experiment uses the same length-bearing codec and payload catalog; see below.

### Experimental SPI implementation

The shared `WAVEX_SPI_LINK_ENABLED` selector in
[link_config.h](../../firmware/shared/config/link_config.h) selects initialization,
routing, sends and foreground service on both MCUs. Rebuild and flash both boards
with the same value; only the selected link starts, with no automatic fallback.
SPI DMA follows the selector. Independent USB consoles and flashing remain available.

SPI carries one existing length-bearing `UartProtocol` frame per fixed physical
DMA slot, zero-padded, or an all-zero idle slot. This preserves exact payload
lengths through the maximum payload, including commands whose handlers require
an exact struct size. The older dormant `WaveXPacket` size-class codec cannot
preserve those lengths and is no longer used by the SPI adapters. The UART
wire format, shared message payloads and protocol version are unchanged; old
experimental SPI images are incompatible and must not be mixed with these images.

Both SPI adapters share the ownership/READY rules in
[spi_transport.hpp](../../firmware/shared/spi_protocol/spi_transport.hpp).
The [SPI transport notes](../spi-notes.md#retained-transport-contract) specify
physical completion, short-frame rejection, queue retention and recovery limits.
These source fixes do not change the live UART framing or protocol version.
Macro-selected SPI startup is integrated; production adoption still requires
the remaining hardware verification. The proposed
[hybrid link](hybrid-inter-mcu-link.md) is a separate target design; no hybrid
routing or session wire extension is implemented yet.

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
| MSG_CONTROL_CHANGE | 0x01 | E→D | `ControlChangeMessage{param, channel, value}` | parameter set (see `ControlParameter` enum). **`channel` is the Track index** — filter and envelope belong to that Track's Instrument (`track-and-patch-model.md` §3.2), and the sounding-voice push is limited to it, so a knob on one Track cannot move another's held notes. Live ids are 0x01–0x0A; 0x0B–0x15 are reserved for `param-locks-and-modulation.md` §1. `PARAM_LFO_RATE`/`PARAM_LFO_DEPTH` sit at 0x16/0x17 — they used to duplicate `PARAM_PAN`/`PARAM_PITCH` at 0x08/0x09 in the same enum, dead but one wiring-up away from a mis-route. |
| MSG_NOTE_ON / OFF | 0x02/0x03 | E→D | `NoteMessage{note, velocity, channel}` | note events; note and velocity must be 0–127 and reserved address bits 4–6 must be zero. `channel` is an **addressing byte** (`track-and-patch-model.md` §2.2): bit 7 `NOTE_ADDR_TRACK` set → bits 0–3 are a Track index (Play grid, sequencer, arpeggiator — `inter_mcu_send_note_{on,off}_track()`); clear → bits 0–3 are the 0-based MIDI channel the event arrived on and the backend fans the note out to **every** Track whose `midi_in` matches (`inter_mcu_send_note_{on,off}_midi()`). Backward compatible: a build predating the flag masks `& 0x0F` and behaves as before, so `PROTOCOL_VERSION` does not move |
| MSG_SAMPLE_LOAD | 0x04 | E→D | `SampleLoadMessage{sample_id, hints, path[BROWSE_PATH_MAX]}` | load sample from Daisy SD into the Sample Pool (path-based; metadata fields are hints, Daisy re-reads). `sample_id` is a request tag: the Pool assigns the resident id and reports it in `MSG_SAMPLE_STATUS` LOAD_COMPLETE/PROGRESS. An exact path already resident is a hit (no SD read, the id it had). Nothing is evicted: a full Pool or arena fails with `SampleLoadFailReason`; a concurrent instrument import returns `SAMPLE_LOAD_FAIL_BUSY` |
| MSG_SAMPLE_DATA | 0x05 | E→D | raw chunk | sample bytes pushed from ESP32 (rare path; SD-local loads preferred) |
| MSG_PARAMETER_UPDATE | 0x06 | D→E | — | parameter echo/update |
| MSG_STATUS_REQUEST | 0x07 | E→D | `StatusRequestMessage{category}` | request status (`GENERAL`, `SAMPLE_MEM`) |
| MSG_STATUS_RESPONSE | 0x08 | D→E | e.g. `SampleMemStatusMessage` | status payload incl. sample-RAM allocator stats + up to 8 `SampleMemEntryMessage` |
| MSG_SAMPLE_CTRL | 0x09 | E→D | `SampleCtrlMessage{slot, cmd, rate}` | rec start/stop, play start/stop |
| MSG_DATA_REQUEST | 0x0B | E→D | `DataRequestMessage{request_type}` | slave pulls queued data (any/meter/wave) |
| MSG_METER_PUSH | 0x10 | D→E | `MeterPushMessage{rms L/R, peak L/R}` | level meters (20–50 ms cadence) |
| MSG_HEARTBEAT | 0x12 | both | `HeartbeatMessage{uptime, rx_total, loop_counter, cpu avg/min/max ×10}` | health + CPU load telemetry. `uptime` is the **only** source of backend uptime and needs no diagnostics subscription — the Diagnostics Daisy tab reads it from here rather than from `MSG_DIAG_PUSH`, so there is one copy of the number and one cadence for it. |
| MSG_ACK | 0x13 | both | `AckMessage{serial_id}` | explicit ack of a sequence number |
| MSG_BROWSE_REQ / RESP | 0x30/0x31 | E→D / D→E | path + start_index + max_entries / `BrowseRespHeader` + `FileEntryWire[]` | paginated SD directory listing; entries carry WAV metadata (rate, channels, bits, duration_ms) |
| MSG_SAMPLE_PLAY_REQ | 0x32 | E→D | path string | audition by path |
| MSG_SAMPLE_STOP_REQ / RESP | 0x33/0x35 | E→D / D→E | `SampleStopReqMessage{slot}` / `SampleStopRespMessage{success}` | stop audition |
| MSG_SAMPLE_STATUS | 0x34 | D→E | `SampleStatusMessage{sample_id, state, ch, rate, frames}` | playback/load notifications (`SampleStatusState`: 0x10 load complete, 0x11 progress with `frames` = percent, 0x12 load failed with `frames` = `SampleLoadFailReason`: no SDRAM / open / format / RAM / read / registry full / instrument import busy) |
| MSG_SAMPLE_PLAY_INDEX_REQ | 0x36 | E→D | `SamplePlayIndexMessage{index}` | audition by directory index |
| MSG_SAMPLE_GET_PATH_REQ / RESP | 0x37/0x38 | E→D / D→E | index / `SamplePathResponseMessage{index, path[200]}` | resolve index → full path |
| MSG_STORAGE_STATUS | 0x39 | D→E | `StorageStatusMessage{mounted, reserved[3]}` | **unsolicited**: SD mounted (1) or lost (0). The frontend has no view of the card slot and cannot poll for this, so ejection/insertion is only observable if the backend says so. On loss the backend also sends `MSG_SAMPLE_STOP_RESP` + an empty `MSG_BROWSE_RESP` so audition exits and the listing clears; on mount the browser re-lists its current path. |
| MSG_DIAG_SUBSCRIBE | 0x3A | E→D | `DiagSubscribeMessage{enable, interval_hz, reserved[2]}` | start/stop the telemetry push. Subscription-gated on purpose: the push flows only while the diagnostics page is open, so it costs nothing the rest of the time. `interval_hz` is clamped 1–10 by the backend. |
| MSG_DIAG_PUSH | 0x3B | D→E | `DiagPushMessage` (102 B → `PKT_SIZE_128`) | **unsolicited** while subscribed: one interval of audio/storage/link/MIDI telemetry plus the backend's system-heap level. Counters are **deltas over `interval_ms`, reset on read**; absolute values only for levels and states. At 2 Hz this is ~216 B/s against a 200 KB/s link — under 0.15%. |
| MSG_SAMPLE_EDIT_SET | 0x3C | E→D | `SampleEditMessage{sample_id, loop_enabled, reserved, gain_db_x10, start_frame, end_frame, loop_start, loop_end, fade_in_ms, fade_out_ms}` | non-destructive playback-edit command (roadmap 1.5.1) for Pool sample `sample_id` (16-bit since protocol 3; the one-byte `slot` it replaced truncated every Pool id, and 0 then meant "newest"). An id the backend cannot find is dropped. Sentinel `end_frame`/`loop_end` 0 = end of file/region. This is the **command**; the backend clamps and never assumes these values were taken verbatim — its reply is the authoritative `MSG_SAMPLE_META`. |
| MSG_SAMPLE_META | 0x3D | D→E | `SampleMetadata{sample_id, generation, sample_rate, total_frames, start_frame, end_frame, loop_start, loop_end, gain_db_x10, fade_in_ms, fade_out_ms, channels, bits_per_sample, loop_enabled, channel_mode, flags, reserved, name[48]}` | authoritative per-sample record (roadmap 1.5.5 item 1), owned by the Daisy and pushed on every change — geometry, edit markers, fades, and load/resident state in one place so playback and display paths cannot disagree. `generation` bumps only on a content change (destructive render), not on marker edits, so a waveform cache keyed on (sample_id, generation) survives edits. Since protocol 2 the record carries `used_by` (Track mask) and `flags` bit 0 resident / bit 1 pinned; a record pushed with the resident bit clear means "unloaded, forget it" |
| MSG_SAMPLE_META_REQ | 0x3E | E→D | `SampleMetaReqMessage{sample_id}` | request a metadata resend; `sample_id` 0 means "every loaded sample" (how the frontend repopulates after its own restart) |
| MSG_ENVELOPE_REQ | 0x3F | E→D | `EnvelopeReqMessage{sample_id, columns, start_frame, end_frame}` | request a min/max envelope for a frame window (roadmap 1.5.5 item 2), not decimated samples — avoids the aliasing the decimated preview it replaced (`MSG_PREVIEW_REQ` 0x0A / `MSG_WAVE_CHUNK` 0x11, retired in protocol 3; values not reused) was prone to. `sample_id` 0 = most recently loaded; `columns` clamped to `MAX_ENVELOPE_COLUMNS` (1280) |
| MSG_CV_CAL_SET | 0x40 | E→D | `CvCalMessage{group, persist, 7×float}` | apply one group's CV calibration; `persist=1` also writes the table to SD; Daisy replies with MSG_CV_CAL_RESP |
| MSG_CV_CAL_GET | 0x41 | E→D | `CvCalGetMessage{group}` | request one group's calibration |
| MSG_CV_CAL_RESP | 0x42 | D→E | `CvCalMessage` (persist unused) | reply to SET and GET |
| MSG_CV_TEST | 0x43 | E→D | `CvTestMessage{group, enable, cutoff, resonance, vca}` | calibration procedure: while enabled, the control tick stages these fixed CVs instead of the paraphonic law |
| MSG_ENVELOPE_CHUNK | 0x44 | D→E | `EnvelopeChunkMessage{sample_id, generation, start_frame, end_frame, total_columns, first_column, columns, channels, encoding}` + `EnvelopeColumn8{min_sample, max_sample}[columns × channels]`, channel-interleaved per column | reply to `MSG_ENVELOPE_REQ`; self-describing (repeats the whole window + generation) so a frontend that missed a chunk or has since moved the view can tell without keeping request state |
| MSG_SAMPLE_SELECT | 0x45 | E→D | `SampleSelectMessage{sample_id, slot, reserved}` | binds `sample_id` for playback on `slot` (0..15, matches `MSG_NOTE_ON`'s channel & 0x0F); `sample_id` 0 clears that slot's binding, so its note-on drops rather than falling back to any other slot's or the most-recently-loaded sample (roadmap Phase 2.5 item 1, "retire the fallback" - the any-channel single-selection behaviour this replaced) |
| MSG_SAMPLE_UNLOAD | 0x46 | E→D | `SampleUnloadMessage{sample_id}` | frees a loaded sample's RAM; voices sounding from it are stopped first. `sample_id` 0 is rejected, not treated as "unload everything" |
| MSG_TRACK_BINDING_REQ | 0x47 | E→D | `TrackBindingReqMessage{track}` | requests backend-authoritative binding state for one Track (0..15), or every Track (`track=0xFF`) |
| MSG_SAMPLE_META_PAGE_REQ | 0x49 | E→D | `SampleMetaPageReqMessage{first, count}` | one window of the Sample Pool in registry order: `first` counts resident records, `count` ≤ `MAX_SAMPLE_META_PAGE` (20). The Pool (1024) is paged, not mirrored |
| MSG_SAMPLE_META_PAGE | 0x4A | D→E | `SampleMetaPageHeader{total, first, n}` + n × `SampleMetadata` | the whole window in ONE frame, so a page cannot be lost to the 4-deep TX queue the way n separate `MSG_SAMPLE_META` would be; retried next main-loop pass if the queue is full. Records carry `used_by` (Track mask) and `flags` (resident / pinned) filled from the Pool |
| MSG_SAMPLE_AUDITION | 0x4B | E→D | `SampleAuditionMessage{sample_id}` | stream the Pool sample's card file with its current region, loop, gain and fades; never claims a Track. Zero/unknown ids or missing paths fail. Stop with `MSG_SAMPLE_STOP_REQ`. Requires an SD-backed sample |
| MSG_CARD_OP | 0x4C | E→D | `CardOpMessage` | read status, prepare, explicitly confirm or cancel card formatting; see Card maintenance |
| MSG_CARD_STATE | 0x4D | D→E | `CardStateMessage` | correlated card confirmation/progress/result; retained for read-only recovery |
| MSG_TRACK_BINDING | 0x48 | D→E | `TrackBindingMessage{track, state, sample_id}` | one Track's actual binding: empty, bare resident sample, imported Patch, or Patch loading. The ESP32 uses this instead of inferring playability from its own Load/Select history; `sample_id` is set only for a bare-sample binding. |
| MSG_SEQ_TRANSPORT | 0x50 | E→D | `SeqTransportMessage{command, clock_source, input_mode, quantize, tempo_bpm_x100, song_position}` | play/stop/continue, tempo, clock source (internal/MIDI), input mode (play/step-rec/live-rec/erase) — `sequencer.md` §4, `midi-sync-tempo-follower.md` §3 |
| MSG_SEQ_PATTERN_OP | 0x51 | E→D | `SeqPatternOpMessage{op, track, step, arg_u8, arg_u16, arg_s16}` | one small idempotent pattern edit; `op` (`SeqPatternOpCode`) selects which fields apply — see the table in `protocol.h` above the struct |
| MSG_SEQ_PATTERN_SYNC | 0x52 | both | SeqPatternRequestMessage / SeqPatternSyncMessage | sixteen-step readback window from the callback-owned pending pattern; see Sequencer page readback below |
| MSG_SEQ_PLAYHEAD | 0x53 | D→E | `SeqPlayheadMessage{pattern, step, playing, sync_state, measured_bpm_x100, loop_count}` | coalesced playhead + sync-lock feedback for the UI (≤ 30 Hz) |
| MSG_SEQ_FILE_OP | 0x5A | E→D | SeqFileOpMessage | read retained status or save-copy/load/new a named pattern |
| MSG_SEQ_FILE_STATUS | 0x5B | D→E | SeqFileStatusMessage | active job, retained completion/error and last successful file name |
| MSG_MIDI_CLOCK_EVENT | 0x55 | E→D | `MidiClockEventMessage{event, source, tick_seq, esp_delta_us, spp_beats16}` | forwarded MIDI real-time/transport byte; `esp_delta_us` is the ESP-domain **delta** (never an absolute timestamp) so the tempo follower can't mix clock domains — `midi-sync-tempo-follower.md` §2/§3 |
| MSG_MIDI_CC | 0x56 | E→D | `MidiCcMessage{cc, value, channel}` | forwarded MIDI control change; Daisy owns the CC→mod-source map (`param-locks-and-modulation.md` §6) |
| MSG_SEQ_CLOCK_OUT | 0x57 | D→E | `SeqClockOutMessage{event, tick_seq, spp_beats16}` | Daisy-generated MIDI clock/transport for the ESP32 to serialize onto DIN + USB immediately |
| MSG_INST_OP | 0x60 | E→D | InstOpMessage | SFZ/WXI probe/load, modulation slot update, new drum Instrument, name, new-copy save, pad assignment/choke, and pad-map readback request; see Instrument editor below |
| MSG_INST_STATUS | 0x61 | D→E | `InstStatusMessage{request_id, slot, op, state, flags, error, zone/sample counts, byte totals/progress, current_name[48]}` | preflight result plus total/current-WAV load progress; flags report missing/invalid WAVs and insufficient resident memory |
| MSG_INST_ZONE_SYNC | 0x62 | D→E | InstZoneSyncMessage | sixteen-pad map, Instrument identity, busy state and retained mutation result |
| MSG_INST_PAD_SOUND_OP | 0x65 | E→D | InstPadSoundOpMessage | read one pad or edit its cutoff/amp envelope inheritance |
| MSG_INST_PAD_SOUND_SYNC | 0x66 | D→E | InstPadSoundSyncMessage | effective pad settings, sample identity and retained edit result |
| MSG_MIX_OP (Solo) | 0x78 | E→D | `MixOpMessage` | since 2026-09-14 the Sequencer page's Solo sends `MIX_OP_SET_SOLO_MASK` selecting the audible Track; un-solo sends 0 and preserves user mutes |
| MSG_INST_EDIT_OP | 0x80 | E→D | `InstEditOpMessage` | Track Instrument sound snapshot, filter/amp edit, Apply or Revert; retains one backend undo point |
| MSG_INST_EDIT_SYNC | 0x81 | D→E | `InstEditSyncMessage` | authoritative audible sound values, revision, busy/error, completion and undo-dirty state |
| MSG_TRACK_OP | 0x63 | E→D | `TrackOpMessage{op, track, value}` | one Track setting (`track-and-patch-model.md` §2.1), idempotent like `MSG_MIX_OP`. `TRACK_OP_SET_MIDI_IN` (`value` = `TrackMidiIn`: 0 Omni, 1..16 that channel **as displayed**, 0xFF Off), `TRACK_OP_SET_POLY_LIMIT` (0 = none, else ≤ `WAVEX_NUM_VOICES`), `TRACK_OP_SET_PRIORITY`, `TRACK_OP_SET_PROGRAM_CHANGE` (0/1). Only `midi_in` has behaviour today; the rest are stored for stages 8 and 6. An out-of-range track or value is rejected and logged, not clamped |
| MSG_MIX_OP | 0x78 | E→D | `MixOpMessage{op, track, value}` | one mixer control change. `value` is op-dependent: gain/master are **centi-dB above the −60 dB floor** (0 = silence, 6000 = 0 dB, 6600 = +6 dB); pan reuses PARAM_PAN's convention (0 left, 32768 centre, 65535 right); `SET_MUTE_MASK` changes user mutes; `SET_SOLO_MASK` (op 0x08) selects audible Tracks without changing those mutes, with zero disabling Solo. Conversions live in `WaveX::Mix` (`shared/audio/track_mix.hpp`) so both ends use one implementation |
| MSG_MIX_STATE_REQ | 0x7B | E→D | `MixStateRequest` | correlated mixer read; nonzero request id and Track 0–15, or 0xFF for master gain |
| MSG_MIX_STATE | 0x7C | D→E | `MixStateMessage` | matching identity, validity, gain/pan in existing mixer wire units and mute target; foreground accepted state, applied through the existing block-boundary handoff; no ramp telemetry; master replies use pan=32768 and mute=0 |
| MSG_MIX_METERS | 0x79 | D→E | `MixMetersMessage{peak[16]}` | strongest post-strip voice peak on either side (pre-master), log-mapped by `Mix::PeakToMeterByte` with 0 reserved for true silence. 40 ms peak-hold windows; only while subscribed. `SUB_METERS` renews a three-second lease, `UNSUB_METERS` ends it; master stereo meters stay on MSG_METER_PUSH |
| MSG_ERROR | 0xFF | both | `ErrorMessage{code, msg[48]}` | error report |

## 4. Conventions & invariants

1. **All payload structs are `__attribute__((packed))` and fixed-layout.** Never reorder fields; append only, or bump `PROTOCOL_VERSION`.
2. **Every payload struct has a named constructor** (`Type(field1, field2, ...)`) plus a zero-initializing default constructor, and no other constructors — this makes the type a non-aggregate, so `Type x = {a, b, c};` / designated-initializer construction is a **compile error**, not just a style rule (field-order bugs have bitten before — found in the 2026-06-26 external architecture assessment). Build with the named constructor (`Type x(a, b, c);`); reserved/padding fields are not constructor parameters and are always zeroed internally. `SampleMemStatusMessage` additionally has `AddEntry()` for bounds-checked appends to its fixed `entries[]` array. When adding a new field, update the constructor's parameter list (and every call site the compiler then flags) in the same commit. **One deliberate exception:** `DiagPushMessage` has only the zeroing default constructor. The named constructor exists to force call sites to be re-checked when a field moves; with ~40 telemetry fields filled one at a time by a collector, a 40-argument constructor would be unreadable and would not achieve that. Its fields are assigned by name instead, which fails loudly on a rename and is immune to reordering — and its round-trip test sets every field to a distinct value so a swap of two same-width neighbours is caught.
3. **String fields** are fixed-size, null-terminated, `FILE_NAME_MAX=48`, `BROWSE_PATH_MAX=256` (path response uses 200).
4. **Flow control**: packet statistics (per-type counters, CRC error counts) are tracked on both sides; NACK triggers resend; a failed Daisy DMA frame retries and the queue self-recovers within 1 s.
5. **Nothing latency-critical rides the link**: audio never crosses it; note events do (from MIDI on the ESP32), so keep the note path under 5 ms end-to-end — this bounds acceptable polling cadence.

## 5. Planned extensions (design first, then implement — see roadmap)

Message-ID blocks are reserved: 0x50–0x5F for sequencer/clock/arp, 0x60–0x6F
for instruments/tuning, 0x70–0x7F for recording/mix/scenes, 0x80–0x81 for
the retained Instrument sound edit extension, and 0xA0–0xAF for render jobs.
Do not assign a new ID outside these blocks without updating this document and
`protocol.h`.

- **Phase 2 (sequencer)**: pattern-edit ops, transport control, playhead/step feedback (coalesced), MIDI clock in/out (`midi-sync-tempo-follower.md`). Kit management is subsumed by instrument ops (`instrument-model.md` §8; 0x54 stays reserved-unused).
- **Phase 2.5**: editable zone sync (0x62), recording (0x70/0x71), and arp (0x58). The mixer ops at 0x78/0x79 are now defined and round-trip tested, though nothing drives them yet — the engine application and the mixer page are the next two stages of `output-routing-and-mixer.md` §6. SFZ probe/load uses the now-live instrument ops at 0x60/0x61; MIDI CC forwarding at 0x56 is also live. `INST_OP_SET_MOD_SLOT` (also on 0x60) is now live end to end (ESP32 `inter_mcu_send_mod_slot()` → Daisy `SfzLoader::SetModSlot()`, instrument-scoped storage on `Instrument::mod_slots`) — no UI sends it yet (`param-locks-and-modulation.md` §7/§9 stage 5). `SRC_MODWHEEL`/`SRC_AFTERTOUCH` still read 0: `MSG_MIDI_CC` reaches the Daisy but nothing feeds it into the mod matrix's `ModSources`, and the ESP32 MIDI task still drops incoming CC/aftertouch rather than forwarding it (§9 stage 4, second half).
- **Phase 4 (offline editing)**: render-job submit/progress/cancel (0xA0–0xA3), sidecar marker sync.
- **Phase 5**: scene apply (0x7A), tuning (0x68).
- Consider a generational "capabilities" handshake at boot (versions on both sides) before the first extension ships.

## Waveform transfer scheduling (as-built)

The wire envelope uses signed 8-bit min/max per channel; it is a visual summary,
not PCM or a bitmap. Keeping both extremes preserves transients and separate
channel levels without cancellation. The source audio remains unchanged.
Request identity remains the sample, content generation and frame window; marker/gain edits do not invalidate the underlying PCM cache.

Protocol 4 uses the former reserved header byte as an explicit encoding tag.
Both firmware images must be updated together: encoding 0 (the previous 16-bit
layout) and unknown encodings are rejected. The canonical codec is
EnvelopeColumn8 in protocol.h. Its 256 levels include exact silence and both
signed full-scale endpoints. Minima round down and maxima round up; exhaustive
host tests bound each endpoint's error to 258 units on the 16-bit display
scale. Quantization runs once per completed column, outside the audio callback.

The ESP32 router validates the encoding, counts and payload before expanding
a chunk into at most 512 bytes of stack scratch. Its synchronous listener copies
those values into the existing receive staging; the UI cache and renderer keep
their signed 16-bit coordinate scale, without gaining additional precision.
No allocation or LVGL work is added to the receive path. At 1,140 stereo columns,
the amplitude payload is 4,560 bytes, and headers/framing bring the transfer to
5,100 bytes in 18 packets. Cache-tier rounding can still request extra columns;
8-bit encoding does not change the horizontal cache/request policy.

EnvelopeScan (firmware/daisy/src/audio/envelope_scan.hpp) owns a partial-column
cursor and one retained packet on the Daisy main loop. Each pump reads at most
24,576 source PCM values, including both reads for summed stereo. Column scans
can yield mid-column, and completed columns accumulate across pump calls until
a packet is full or the run ends. TX backpressure retains the packet without
rescanning. Sample unload/replacement cancels the job; the engine revalidates
the sample and generation before each scan or send.

Envelope packets carry at most 256 data bytes. They are admitted only to an
idle UART TX queue, including the DMA-active frame. This leaves queue slots for
normal replies and prevents a backlog of waveform packets. One already-admitted
frame cannot be preempted: its maximum serialization time is 1.43 ms at the
current link rate, excluding main-loop scheduling and unrelated traffic.
The scan runs after streaming refill and does no SD I/O, allocation, interrupt
masking or blocking TX. Incoming controls use the independent RX direction.

The frontend rounds requests to a cache-tier grid. When only the last bin
extends beyond EOF, the backend preserves those requested bin boundaries and
clips the last bin, while the response header reports the actual EOF. Spreading
the EOF-clamped span evenly over every bin would shift transients into the wrong
cache columns. Arbitrary requests extending by more than a final bin retain
the bounded, evenly divided clamped-window behavior.

The ESP32 still assembles a complete run under the existing receive-state
handoff before the UI commits it. It immediately renders cached data on window
changes; only missing-data requests wait for the settle. A complete finer-tier
run satisfies a coarser view without another scan or transfer.

Host regression: an 8,000,000-frame stereo run at 1,280 columns takes 20 packets
and 5,720 framed bytes, versus 640 packets and 29,440 bytes under the previous
per-pump packet flush. This is a byte-count comparison, not measured hardware
latency. Small scans can use more framing bytes with the smaller packets.
Bench acceptance remains concurrent streamed playback, control bursts, repeated
sample/window changes, UART queue statistics and a zero-underrun soak.


## Sequencer page readback

MSG_SEQ_PATTERN_SYNC now has directional payloads defined in protocol.h:
the frontend requests one Track and one aligned 16-step window with a nonzero
request id; the backend echoes that id with the complete pending-pattern page,
global groove/transport settings and all four locks per step. Invalid windows
return valid = 0. Runtime sample ids and Instrument bindings are absent:
Patterns own steps, and Tracks retain their Instruments.

Clients keep one page request outstanding, reject stale request ids and retry
a timed-out read rather than replaying non-idempotent edits. Callback-owned
readback is published as one immutable value to the main loop; serialization
and UART transmission remain foreground work. Multiple reads in one callback
may coalesce to the newest request.

SEQ_TRANSPORT_CONFIGURE updates tempo/mode without restarting playback.
SEQ_OP_CLEAR_TRACK clears every step and lock in one row while preserving its
mute setting. The wire shapes, sizes and bounds are centralized in
firmware/shared/spi_protocol/protocol.h.


## Instrument editor (protocol 5)

InstOpMessage appends explicit pad_index, pad_choke and pad_sample_id fields.
Both images must use the current protocol version (6). Existing probe/load/modulation fields retain
their meanings. The central enums and packed structs in protocol.h define
the wire layout.

NEW creates an empty drum Instrument after its Track's stop acknowledgement;
SET_PAD_SAMPLE assigns a resident Pool id to a fixed pad, sets its choke group
(0 off, 1-15), or clears it when the id is zero. Pad indices 0-15 map to notes
60-75. Imported key/velocity maps outside that shape are read-only here.
SET_NAME changes the in-memory name. SAVE writes a new WXI copy using path as
the filename stem. NEW, SET_NAME and SAVE accept 1-23 ASCII letters, digits,
spaces, hyphens and underscores, without leading/trailing spaces.

GET_PAD_MAP returns MSG_INST_ZONE_SYNC. Each response echoes the read id and
retains the Track's completed mutation id/error, so retrying a read cannot
mistake a lost or failed mutation for success. Repeated completed mutation
ids return their outcome without repeating the mutation. Main-loop replies
are retained on a full UART queue. The UI never changes LVGL objects on RX.

The bounded debug MSG payload now accommodates the 512-byte packet class's
payload, including these extended requests.

## Per-pad sound editing (additive to protocol 6)

MSG_INST_PAD_SOUND_OP and MSG_INST_PAD_SOUND_SYNC carry the types and bounds
defined in `firmware/shared/spi_protocol/protocol.h`. No existing payload or
WXI layout changes. GET returns effective cutoff, amp attack/decay/sustain,
inheritance state and sample identity. Mutations require a populated fixed
drum pad and its current sample id; invalid, stale and busy edits return a
retained error without changing the Instrument. INHERIT clears the zone's
shared filter/envelope override flag. Editing the first field copies the
Instrument defaults into the zone before changing that field, preserving
untouched floating-point values.

The Daisy foreground owns these edits and republishes only the affected
Track's prepared voice map. No sample allocation, retirement or voice-stop
barrier is needed. Edits apply to subsequent hits. Sounding overridden voices
keep their cutoff and amp envelope when Instrument controls move; resonance
continues to follow the Instrument. Release is not exposed for one-shot pads,
which ignore note-off.

The UI consumes synchronized snapshots under its normal LVGL lock, retains
coalesced desired values while one mutation is pending, and retries GET to
recover a dropped completion. Timed-out edits return to authoritative
readback. The Pad Sound page is reached from Pad Map's shifted Sound key;
Save copy on Pad Map persists the existing zone fields in WXI.

## Track Instrument sound preview and undo

`MSG_INST_EDIT_OP` (0x80) and `MSG_INST_EDIT_SYNC` (0x81) extend the exhausted
Instrument message range with typed filter and amp edits plus GET, Apply and
Revert. The backend keeps one `InstrumentSoundUndo` point per Track for sound
controls only: filter cutoff/resonance and Instrument amp trim gain/pan. It
does not include PCM references, key maps, zones or names. The first edit
captures the baseline; subsequent edits preview automatically, while Apply
commits the current audible values and Revert restores the baseline. A
successful WXI save also commits the audible working values; a failed save
leaves the undo point intact. Switching pages or Tracks retains it, while
replacing the Instrument clears it.

The frontend coalesces edits and uses the common Edited marker/actions. Page
navigation waits for an outstanding delivery, not for Apply/Revert, so the
working sound remains audible while the user moves among Osc, Env, Mod, Filter
and Amp. Held-note updates use the bounded live handoff described in
`instrument-model.md`; map/sample assignment and Instrument replacement retain
their stop/next-note boundaries.

The matrix destination id `INST_MOD_RESONANCE` is 5; ids 0–4 remain unchanged
and the oscillator pitch destinations use ids 6 (`OSC1_PITCH`) and 7
(`OSC2_PITCH`). The destination count is 8; ids 8 and above are unsupported
until assigned. The existing revisioned
Instrument matrix snapshot and 34-byte request/112-byte sync payloads are
unchanged. Resonance routes use a signed normalized offset, sum before
clamping to `[-1, 1]`, and apply to the base value before the filter's
`[0, 1]` clamp. This reuses the existing matrix preview, Apply/Revert and WXI
save path. Pitch routes use the source oscillator identity and compose with
the common pitch scale; full-depth source 1 spans ±2 semitones. This pitch
checkpoint does not add sync, FM, mix or LFO-rate wire behavior.

The filter edit group supports four Instrument-owned modes: `LP` (0), `HP`
(1), `BP` (2) and `Notch` (3). `INST_EDIT_FILTER_SETTINGS` (op 5) carries
cutoff, resonance and the mode in the former reserved byte at offset 10;
that byte is zero for every other edit op. The sync message returns the mode
in its former reserved byte at offset 17, with the remaining reserved bytes
zero. The packed payload sizes were 28 bytes for the edit request and 36
bytes for sync until the slope/drive growth described below. Modes outside 0–3 and nonzero reserved fields are rejected;
legacy filter edits preserve the current mode.

The same op also carries the Instrument's filter **topology**, which
implementation renders that mode: `SVF` (0, the first-party 12/24 dB
state-variable filter) or `LADDER` (1, the first-party zero-delay-feedback
four-pole ladder). Values 2 and 3 carried two Huovilainen ladders for one
day (2026-09-14) and are retired, not reused. It
occupies the edit request's byte at offset 11 and the sync message's byte at
offset 18, both formerly reserved; the sync message's last byte at offset 19
stays reserved and zero. Topologies at or above
`INST_FILTER_TOPOLOGY_COUNT` are rejected, and like the mode the byte must
be zero for every op other than 5, so a legacy filter edit preserves the
current topology. Zones never override it.

Since 2026-09-14 op 5 also carries the Instrument's filter **slope**
(`INST_FILTER_SLOPE_12` = 0 or `INST_FILTER_SLOPE_24` = 1) and **drive**
(float 0..1: the SVF's soft-clip amount, the ladder's input drive). They
follow the 16-byte sound block: the edit request grew from 28 to 36 bytes
(slope at offset 28, three reserved zero bytes, drive at 32) and the sync
message from 36 to 44 bytes (slope at 36, three reserved bytes, drive at
40). Like mode and topology they must be zero for every op other than 5.
Both are Instrument-owned and applied by whichever topology renders the
voice; the former engine-wide `WAVEX-FILTER` bench switch is gone. The
`.wxi` FILT chunk is 23 bytes (topology at 17, slope at 18, drive at 19);
17- and 18-byte chunks from earlier files load with the defaults `SVF`,
12 dB and no drive.

Because this changes the meaning of formerly reserved bytes, the frontend
and Daisy images must be deployed as a pair. A peer that does not understand
op 5 or the returned mode and topology bytes is not a compatible
mixed-version endpoint; the reserved byte remains zero for all other
operations.

## Per-step notes (protocol 6)

SEQ_OP_SET_STEP_NOTE sets arg_u8 to a MIDI note 0-127, independently of
step on/off and velocity. Out-of-range notes are rejected. SeqStepState uses
its former reserved byte for note (default 60), preserving the page's byte
size. Both MCUs must use protocol 6: a protocol-5 page's reserved zero is
not a valid substitute for a protocol-6 step's default note.

Daisy resolves prepared zone keys against the scheduled note and velocity,
including layer selection and crossfade weights. Retriggers preserve the
primary hit's note and velocity. This is one note per drum-shaped step,
not the future melodic chord/gate payload.


### Pattern file operations

`SeqFileOpMessage` and `SeqFileStatusMessage` in `protocol.h` define the
32-byte request and 40-byte response. GET is read-only; SAVE_COPY, LOAD and
NEW are mutations. Nonzero request IDs correlate replies. GET returns the
last mutation's completion independently of its own read ID; a duplicate
active or most recently completed mutation is not replayed. The UI never
automatically replays a timed-out mutation.

File names are bounded, terminated path components, not arbitrary paths.
LOAD/NEW replace the working pattern after confirmation; tempo and Track
instruments are not file-owned. Busy, invalid name, missing/duplicate file,
I/O, invalid format, insufficient space and capture-busy errors are explicit. See
[sequencer.md](sequencer.md#pattern-files-as-built) for storage and handoff
behavior. These additive messages keep protocol version 6; both updated
MCUs are needed for the new page. The arpeggiator's 0x58 reservation remains.

### Card maintenance

`CardOpMessage` and `CardStateMessage` in `protocol.h` are the authoritative
payload definitions (12-byte request, 16-byte response). These additive
messages retain protocol 6 and require both updated firmware images.
Nonzero request IDs correlate replies; reserved bytes must be zero. The
frontend stores a complete response snapshot outside LVGL and the Storage
page consumes it on its UI timer.

GET only reads state. PREPARE_FORMAT arms a token; CONFIRM_FORMAT must echo
that token within 60 seconds on the same observed card generation. CANCEL
disarms it. The page only exposes **Erase all data** after its own explicit
PREPARE receives the matching token, alongside **ALL CARD DATA WILL BE LOST**
and Cancel. Unsolicited or stale confirmation cannot arm the page. While the
format is accepted/running, mutations are ignored; the last completion ID
prevents replay of the same confirmation. GET retains that completion ID
separately from its read ID, and reconnect polling never sends CONFIRM again.

The Daisy retains pending responses under TX backpressure, leaves a short
foreground interval after acceptance, then rechecks storage ownership and
stops playback before formatting. During maintenance, the dispatcher admits
only card status/control and heartbeat messages. Errors distinguish busy,
missing card, invalid/expired confirmation, unsafe audio stop and I/O failure.
An I/O failure may mean the card was already erased or only partly initialized;
only DONE means format, remount and directory creation all succeeded. Firmware
reboot loses the retained result; the page reports an unknown result when its
confirmed ID is no longer present. See [architecture](../architecture.md#card-saves-and-formatting-as-built)
for foreground timing and resident-sample behavior.

Save admission adds `INST_ERROR_NO_SPACE` and `SEQ_FILE_NO_SPACE`; query
failure remains an I/O error. CV calibration uses its existing boolean storage
result/logging path; `MSG_CV_CAL_RESP` still reports the runtime table, not
successful persistence.

## Track page readback (additive to protocol 6)

The Track page requests the selected Track through MSG_TRACK_STATE_REQ and
consumes MSG_TRACK_STATE, using the shared TrackStateRequest/TrackStateMessage
definitions in `firmware/shared/spi_protocol/protocol.h`. A nonzero request id
and Track identity distinguish current replies from old selections. The Daisy
foreground snapshots the binding and routing fields and retains a reply when
the UART queue is full. No callback work is added.

MIDI input edits use the existing MSG_TRACK_OP, followed by authoritative
readback. Values are validated before narrowing to the engine's byte fields;
wide values cannot wrap into another channel. The UI offers Omni, 1-16 and Off.
Polyphony/priority and Program Change fields remain stored for their later
engine stages and are not exposed as working controls.

### Filtered browser requests (as built, 2026-09-11)

MSG_BROWSE_REQ retains its legacy start-index byte and NUL-terminated directory
path. An optional final BrowseFilter byte selects samples or instruments;
omitting it lists all supported files. The values and bounded encoder/decoder
live in protocol.h. Directories remain visible, and the Daisy applies filtering
before counting and pagination. Sample listings contain WAV; Instrument listings
contain WXI and SFZ, case-insensitively. Empty, unterminated, overlong or malformed
requests are rejected without accessing a silently shortened path.

### Keyboard Key Map (as built, 2026-09-11)

The centralized Key Map request/snapshot records in protocol.h carry all 32
stable zone slots, inclusive key and velocity ranges, root notes and pool sample
ids. A per-Track revision plus the expected sample id guards each mutation;
replacing the Instrument invalidates edits even when its sample ids are reused.
Range changes are atomic and affect newly prepared triggers. Sample assignment
or clearing waits for the target Track's voice-stop acknowledgement, rechecks
the revision, then updates pool references. Other Tracks keep their references.

Read requests retain the last completed editor request id and error, so a lost
reply cannot turn an unconfirmed mutation into success. New keyboard Instruments
use the additive INST_OP_NEW_KEYBOARD operation; naming and WXI save copies use
the existing Instrument operations. Host round-trip, dispatch, stale revision,
pool ownership and sparse WXI tests cover the contract.

### Parameter-lock editing

The existing MSG_SEQ_PATTERN_OP/MSG_SEQ_PATTERN_SYNC messages carry four locks
per step. SET_PARAM_LOCK retains its by-parameter update/oldest-eviction policy.
CLEAR_PARAM_LOCK removes a single parameter. SET_PARAM_LOCK_SLOT atomically
replaces one slot, rejects unsupported or duplicate ids, and accepts parameter
zero to clear it. Field assignments and stable ids live in
[protocol.h](../../firmware/shared/spi_protocol/protocol.h). Full pattern readback
acknowledges accepted edits; no optimistic UI value becomes the pattern authority.

## Oscillator settings and readback

The centralized `InstOscOpMessage` / `InstOscSyncMessage` contract in
`protocol.h` adds oscillator GET, SET and copy-into-empty-map operations.
SET updates level, coarse/fine tuning, key tracking, Mono and the Instrument's
oscillator mix. `InstOscSettings::mono` uses its formerly reserved final byte,
validated as 0/1 with default 0; request/snapshot sizes remain unchanged.
Mono applies to new notes only so held voices retain their channel reservation.
Paired firmware is needed for the Mono control; older receivers reject a
nonzero formerly reserved byte rather than silently applying it. Reads return the authoritative values, map occupancy,
Instrument revision and retained mutation outcome. Mutations require that
revision; a stale edit, busy loader or occupied copy destination is rejected.
Copying a map reuses this Track's existing Sample Pool references. These
operations change future triggers through the immutable prepared-map handoff.
The ESP32 consumes complete synchronized snapshots for the Osc editor.
SET accepts the same level and signed tuning ranges as saved WXI settings;
unrelated edits preserve untouched fields exactly. These operations do not
implement per-oscillator stereo pan, wavetable playback or Bank recall.

## Oscillator selection in Key Map

The former reserved byte in InstKeyMapOpMessage now selects oscillator 0 or
1. Default zero retains the first-map behavior and the request/reply sizes
are unchanged. Each read request ID is scoped to the selected oscillator;
the reply carries the requested map under that ID. A page change creates a
new request identity, so an old map cannot replace the current selection.
Mutations share the Instrument revision across both maps and keep the
existing expected-sample and voice-stop checks. Assignment enables a Sample
source; it cannot replace a reserved Wavetable source.

## Instrument envelopes and modulation matrix

InstModOpMessage / InstModSyncMessage provide GET, SET_ENV and SET_SLOT.
The centralized schema in protocol.h defines the complete envelope and
eight-slot snapshot. Every mutation carries the Instrument revision and
retains its completion id/error for polling and duplicate delivery.
Envelope values use seconds and linear sustain; matrix depth is signed,
with existing source/destination/curve numbering retained. Env 1 and Env 3
append source ids without changing the original Env 2 source. GET never
changes a Track; malformed, busy or stale edits are rejected.

## Instrument LFO snapshots

`MSG_INST_LFO_OP` (0x6E) and `MSG_INST_LFO_SYNC` (0x6F) provide typed,
revisioned GET/SET snapshots for the two Instrument-owned per-voice LFOs.
The request identifies the Track and LFO index and carries the expected
Instrument revision for mutations; the retained completion id and error status
make duplicate delivery and timeout recovery observable without retrying a
blind edit. Snapshots carry waveform, rate mode/value, sync division,
retrigger, pitch-follow, delay and fade. Hz values are limited to 0.02–20 and
sync divisions span 1/16 through 4 bars; pitch-follow is meaningful only for
Hz mode. Source id 5 remains retired and reads zero, while source ids 6 and 17
identify voice LFO 1 and 2.

The Daisy backend evaluates both LFOs from a Q32 free-running frame/beat epoch
and retains them in WXI. This transport is host-tested and covered by a
two-board save/reload check. The ESP32 LFO touch tab and live audition
Apply/Revert flow are not yet connected; the typed messages therefore expose
the backend foundation rather than a completed panel workflow.

### Project transactions

`MSG_PROJECT_OP` / `MSG_PROJECT_STATUS` use the centralized Project operation,
status and error definitions in `firmware/shared/spi_protocol/protocol.h`.
GET reads retained state; SAVE_COPY, LOAD and NEW carry nonzero request IDs.
Active/completed IDs, progress, failure Track and the last successful name let
the frontend distinguish a long operation from an unconfirmed completion.
Readback never repeats an operation. See [Project persistence](project-persistence.md)
for lease, stop acknowledgement, commit and failure semantics. Round-trip tests
cover every operation/error and reject malformed status bounds.


## Project Pattern slots

`MSG_SEQ_SLOT_OP` / `MSG_SEQ_SLOT_STATUS` add stopped Create, Copy active,
Rename, Select and retained GET for 128 Project Pattern slots. Exact payloads,
operation values and validators live in `protocol.h`; both images need this
additive protocol-v6 feature. Destination IDs are stable zero-based slots;
Create/Copy refuse occupied slots. GET may inspect another slot while a job
runs without changing its destination. Active and last completed request IDs
allow lost replies to be recovered without repeating mutations. The callback
rejects playing or MIDI-armed captures, and Select completes only after install
acknowledgement. See [Pattern management](pattern-management.md).
