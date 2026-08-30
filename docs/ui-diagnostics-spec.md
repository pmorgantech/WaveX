# Diagnostics Page Specification

What the diagnostics screen should show, why each figure earns its place, and
the one protocol addition the whole thing needs. Written to be handed to a
design pass alongside [`ui-design-constraints.md`](ui-design-constraints.md).

Availability is marked per stat:

- **✅** available on the ESP32 today
- **🔌** exists on the Daisy today, needs a wire message (see
  [MSG_DIAG_PUSH](#the-one-protocol-addition))
- **🆕** needs new instrumentation

## Why replace the current page

Today's page is three static columns of four values each — roughly 90% empty
space, and every figure is a since-boot total. During the August 2026 audition
debugging, none of the numbers that actually identified faults were on it:
ring low-water, SD read latency and CRC codes, callback rate, link cost. They
were all in the serial log, scrolling past.

Two rules follow from that experience, and they matter more than the widget
choices:

1. **Per-interval, not since-boot.** Every counter resets on read, as
   `SD PERF` and `UART PERF` already do. A since-boot total hides a problem
   that started thirty seconds ago — a frozen `count=5333` read as "idle"
   when in fact every read was failing.
2. **A trend beats a number.** `lv_chart` is enabled and cheap. A 60-sample
   rolling sparkline turns "low-water = 180" into "low-water has been sliding
   for 20 s", which is the difference between noticing and diagnosing.

## Layout

Content area is 1280×545 (see constraints doc). A `lv_tabview` with a 52 px
tab bar leaves **1280×493** per tab.

```
┌ tab bar 52px ─ System │ Audio │ Link │ Storage │ MIDI ──────────────┐
│ ┌ hero tiles: 4 across, ~150px tall ───────────────────────────────┐│
│ │  LABEL 18/grey                                                   ││
│ │  VALUE 32/white          ← the number you read from a metre away ││
│ │  ▁▂▃▅▇ sparkline or bar  ← trend, green/orange/red by threshold  ││
│ └──────────────────────────────────────────────────────────────────┘│
│ ┌ detail: lv_table, 2 columns (name / value), scrollable ──────────┐│
│ └──────────────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────────────┘
```

Softkeys are shared across tabs: `Back · Tab < · Tab > · Freeze · (Log) ·
Reset`. **Freeze** stops updating so a transient can be read — during this
session, values often changed faster than they could be noted.

---

## Audio

**Hero tiles**

| Tile | Format | Threshold | Availability |
|---|---|---|---|
| Callback rate | `1000.2 Hz` | red if <990 | 🔌 |
| Ring low-water | `1420 / 2048` | orange <400, red <100 | 🔌 |
| Underruns | `0 /min` | orange ≥1 | 🔌 |
| Engine CPU | `7.4%` avg, max in sparkline | orange >60 | ✅ |

Callback rate is the most valuable single number on the page: it separates
"the engine stopped" from "the ring starved", an ambiguity that cost hours
because an underrun is only detectable *inside* the callback — if the callback
stops, the ring stays full and the log stays clean.

**Detail table**

| Row | Availability |
|---|---|
| Output peak / RMS, L and R (as two `lv_bar`s, not text) | ✅ |
| Current WAV: rate / channels / bits | 🔌 |
| Resampling active (ratio ≠ 1.0) | 🔌 |
| Pre-buffer fill (n/1024) | 🔌 |
| Ring pushes/s, discarded passes/s | 🔌 |
| Active voices | 🆕 |

Discarded passes deserve a row of its own: skip-without-consume is how two
separate playback stalls began, and it is invisible in every other figure.

---

## Link

**Hero tiles**

| Tile | Format | Availability |
|---|---|---|
| Link state | `OK · hb 0.4s` (red if age >3 s) | ✅ |
| Frames/s | `21 rx / 43 tx` | ✅🔌 |
| Link CPU cost | `0.02% of loop` | 🔌 |
| Error rate | `0 /min` | ✅🔌 |

Link CPU cost is what settles the "should we move to SPI?" question with data
rather than opinion — see `docs/backlog.md`.

**Detail table**

| Row | Availability |
|---|---|
| Bytes/s each way, against the 200 KB/s ceiling (bar) | 🔌 |
| Sequence drops / resyncs | ✅🔌 |
| CRC errors, frame-sync errors | ✅🔌 |
| TX errors, queue overflows | 🔌 |
| Round-trip latency | 🆕 |
| **Per-message-type counts** | ✅ |

The last row is free: `wavex_packet_stats_t` already counts all 19 message
types on the ESP32. Rendered as a scrollable `lv_table` it needs no new
plumbing at all, and answers "is the thing I expect actually arriving?".

---

## Storage

**Hero tiles**

| Tile | Format | Availability |
|---|---|---|
| Card + bus clock | `mounted · FAST/50MHz` | ✅🔌 |
| Throughput | `177 KB/s` with a target line at the rate playback needs | 🔌 |
| Read latency | `1.7 ms avg · 2.9 ms max` | 🔌 |
| Errors / recoveries | `0 / 0` | 🔌 |

Latency creeping *before* errors appear is the marginal-timing tell; that is
why max belongs on the face of the tile and not in a detail row.

**Detail table**

| Row | Availability |
|---|---|
| Card type + capacity (flag >32 GiB → exFAT trap) | 🔌 |
| Last FatFS result **and** `HAL_SD_GetError` | 🔌 |
| Current file, `data_start` with frame/sector remainders | 🔌 |
| Loop-point period, backoff active | 🔌 |
| Sample RAM: free / largest free block / in use | 🔌 |
| `failed_allocs` | 🔌 |

Carrying the FatFS result *and* the HAL error together is deliberate:
`FR_DISK_ERR` alone says only "the read failed", whereas
`SDMMC_ERROR_DATA_CRC_FAIL` with the card in `TRANSFER` state says "the card
is healthy and the wiring is marginal" — a completely different action. Sample
RAM figures come free from the existing `SampleMemStatusMessage`.

---

## MIDI

**Hero tiles**

| Tile | Format | Availability |
|---|---|---|
| Sync state | `LOCKED` / `acquiring` / `freewheel` / `internal` | 🔌 |
| Measured BPM | `120.04` | 🔌 |
| Notes/s | `12` | ✅🆕 |
| CC/s | `40` | 🆕 |

**Detail table**

| Row | Availability |
|---|---|
| Clock ticks/s vs expected (24 × BPM / 60) — the deviation *is* the jitter | 🆕 |
| Last note: number / velocity / channel | 🆕 |
| Last CC: number / value / channel | 🆕 |
| Transport: playing, pattern, step, loop count | 🔌 |
| Dropped / late events | 🆕 |

"Last note" and "last CC" look trivial but make *is my controller connected
and on the right channel* answerable at a glance, which is otherwise a
serial-log exercise.

---

## The one protocol addition

Almost every 🔌 lives on the Daisy and never reaches the wire — `HeartbeatMessage`
carries only uptime, rx_total, loop_counter and CPU. One periodic push covers
the entire set:

```c
// Daisy -> ESP32, unsolicited while subscribed. All counters are DELTAS over
// interval_ms, matching the reset-on-read convention the Daisy telemetry
// already uses; absolute values are only for levels and states.
struct DiagPushMessage {          // 102 bytes -> PKT_SIZE_128
    // audio
    uint16_t callback_hz_x10;     // 10000 = 1000.0 Hz
    uint16_t ring_low_water;      // frames, of RB_CAP_FRAMES (2048)
    uint16_t underruns;           // episodes this interval
    uint16_t prebuffer_filled;    // frames, of PREBUFFER_FRAMES (1024)
    uint16_t engine_cpu_x10;      // avg
    uint16_t engine_cpu_max_x10;
    uint32_t wav_sample_rate;
    uint8_t  wav_channels;
    uint8_t  wav_bits;
    uint8_t  playing;
    uint8_t  resampling;          // ratio != 1.0
    uint32_t ring_pushes;
    uint32_t ring_discards;
    // storage
    uint8_t  sd_mounted;
    uint8_t  sd_speed_index;      // 0..4; names live on the ESP32
    uint16_t sd_reads;
    uint32_t sd_bytes;
    uint16_t sd_lat_avg_us;
    uint16_t sd_lat_max_us;
    uint16_t sd_errors;
    uint16_t sd_recoveries;
    uint8_t  sd_last_fatfs;       // FRESULT
    uint8_t  reserved0;
    uint32_t sd_hal_err;          // HAL_SD_GetError()
    uint32_t sample_ram_free;
    uint32_t sample_ram_largest;
    uint16_t sample_failed_allocs;
    uint16_t sample_count;
    // link (Daisy side; the ESP32 keeps its own view)
    uint32_t link_total_us;       // time in UartLinkProcess this interval
    uint16_t link_max_us;
    uint16_t link_rx_frames;
    uint16_t link_tx_frames;
    uint16_t link_errors;
    uint16_t link_seq_drops;
    uint16_t link_queue_overflows;
    // midi / transport
    uint16_t midi_notes;
    uint16_t midi_ccs;
    uint16_t midi_clock_ticks;
    uint16_t measured_bpm_x100;
    uint8_t  sync_state;          // 0=internal 1=acquiring 2=locked 3=freewheel
    uint8_t  transport_playing;
    uint8_t  pattern;
    uint8_t  step;
    uint32_t interval_ms;         // window these deltas cover
    // backend runtime (appended after interval_ms, so every field above keeps
    // its offset and a pre-stage-8 backend's 94-byte push still parses)
    uint32_t heap_total;          // linker-reserved heap region, bytes
    uint32_t heap_free;           // headroom above the allocator's break
} __attribute__((packed));
```

Suggested IDs: `MSG_DIAG_SUBSCRIBE = 0x3A` (E→D, `{uint8_t enable; uint8_t
interval_hz;}`) and `MSG_DIAG_PUSH = 0x3B` (D→E). `0x39` is taken by
`MSG_STORAGE_STATUS`.

**Cost:** 102 bytes at 2 Hz is ~216 B/s against a 200 KB/s link — under 0.15%.
Subscription matters more than the size: it should flow only while the
diagnostics page is open, so it costs exactly nothing the rest of the time.

Per `AGENTS.md`, adding these means `protocol.h` + a round-trip test +
`features/inter-mcu-protocol.md` in the same commit.

## Status (August 2026)

Steps 1 and 2 below have landed on `ui-update`. What is live:

- **System** and **Link** tabs — fully live, ESP32-local sources.
- `MSG_DIAG_SUBSCRIBE` / `MSG_DIAG_PUSH` — implemented, subscription driven by
  the diagnostics page's `onEnter`/`onExit`.
- **Audio** and **Storage** tabs — live from the push. Telemetry older than
  1.5 s (three missed pushes at 2 Hz) renders as "no telemetry from backend"
  rather than a frozen figure, because a stale number presented as current is
  precisely how a dead link reads as a healthy one.
- **MIDI** tab — the wire fields exist and are parsed, but the Daisy has no
  sequencer or tempo follower to fill them, so they read zero and the tab says
  so.

Still 🆕 and not yet instrumented: round-trip latency, active voices, per-CC
and per-note detail, dropped/late MIDI events. Backend link fields
(`link_*`) are only populated when `WAVEX_DAISY_UART_PERF_DEBUG` is on —
timing every `UartLinkProcess` call is the overhead that flag exists to gate,
and the frontend already shows its own view of the link.

## Suggested order

1. `MSG_DIAG_SUBSCRIBE` / `MSG_DIAG_PUSH` — unblocks every 🔌, which is most
   of the value.
2. Tabview shell + System tab (all ✅ today) and the Link per-message table
   (also free).
3. Audio and Storage tabs once the push lands.
4. MIDI last: it has the most 🆕, and the counters should follow the Phase 2
   sequencer work rather than lead it.
